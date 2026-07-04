#pragma once
// A tiny expression language over columns. Recursive-descent parser → AST →
// tree-walking evaluator that dispatches to the fast:: kernels. This is the
// "parsing / compilers" checkbox: a real lexer, a real grammar, a real AST —
// NOT a SQL engine and NOT a query planner. ~250 lines, one file.
//
// Grammar (precedence-climbing):
//   expr   := term   (('+' | '-') term)*
//   term   := factor (('*' | '/') factor)*
//   factor := number | ident | call | '(' expr ')' | '-' factor
//   call   := ident '(' arg (',' arg)* ')'
// Column identifiers (price|volume|ts) evaluate to that whole column; a bare
// number is a scalar broadcast across the vector. Functions:
//   mean(col, w)  std(col, w)  ema(col, alpha)  vwap(col, col, w)
// Everything else composes elementwise, so `vwap(price,volume,50)-ema(price,0.1)`
// is a length-n vector.
#include <cmath>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "column.hpp"
#include "rolling_fast.hpp"

namespace aegis::expr {

// ---- value: scalar (broadcast) or vector ----------------------------------
struct Value {
    bool scalar = true;
    double s = 0.0;
    std::vector<double> v;
    double at(size_t i) const { return scalar ? s : v[i]; }
    size_t size(size_t n) const { return scalar ? n : v.size(); }
};

// ---- lexer ----------------------------------------------------------------
struct Token {
    enum Kind { Num, Ident, Op, LParen, RParen, Comma, End } kind;
    double num = 0;
    std::string text;
    char op = 0;
};

class Lexer {
public:
    explicit Lexer(const std::string& src) : s_(src) {}
    Token next() {
        while (i_ < s_.size() && std::isspace((unsigned char)s_[i_])) ++i_;
        if (i_ >= s_.size()) return {Token::End, 0.0, {}, 0};
        char c = s_[i_];
        if (std::isdigit((unsigned char)c) || c == '.') {
            size_t j = i_;
            while (j < s_.size() &&
                   (std::isdigit((unsigned char)s_[j]) || s_[j] == '.' ||
                    s_[j] == 'e' || s_[j] == 'E' ||
                    ((s_[j] == '+' || s_[j] == '-') && (s_[j - 1] == 'e' || s_[j - 1] == 'E'))))
                ++j;
            Token t{Token::Num, 0.0, {}, 0};
            t.num = std::stod(s_.substr(i_, j - i_));
            i_ = j;
            return t;
        }
        if (std::isalpha((unsigned char)c)) {
            size_t j = i_;
            while (j < s_.size() && std::isalnum((unsigned char)s_[j])) ++j;
            Token t{Token::Ident, 0.0, {}, 0};
            t.text = s_.substr(i_, j - i_);
            i_ = j;
            return t;
        }
        ++i_;
        switch (c) {
            case '(': return {Token::LParen, 0.0, {}, 0};
            case ')': return {Token::RParen, 0.0, {}, 0};
            case ',': return {Token::Comma, 0.0, {}, 0};
            case '+': case '-': case '*': case '/': {
                Token t{Token::Op, 0.0, {}, 0};
                t.op = c;
                return t;
            }
        }
        throw std::runtime_error(std::string("bad character: ") + c);
    }

private:
    const std::string& s_;
    size_t i_ = 0;
};

// ---- AST ------------------------------------------------------------------
struct Node {
    enum Kind { Num, Col, Call, Bin, Neg } kind;
    double num = 0;                         // Num
    std::string name;                       // Col / Call
    char op = 0;                            // Bin
    std::vector<std::unique_ptr<Node>> kids;  // Call args / Bin lhs,rhs / Neg
};
using Ptr = std::unique_ptr<Node>;

// ---- parser ---------------------------------------------------------------
class Parser {
public:
    explicit Parser(const std::string& src) : lex_(src) { cur_ = lex_.next(); }
    Ptr parse() {
        Ptr e = expr();
        if (cur_.kind != Token::End) throw std::runtime_error("trailing tokens");
        return e;
    }

private:
    void advance() { cur_ = lex_.next(); }

    Ptr expr() {
        Ptr lhs = term();
        while (cur_.kind == Token::Op && (cur_.op == '+' || cur_.op == '-')) {
            char op = cur_.op;
            advance();
            auto n = std::make_unique<Node>();
            n->kind = Node::Bin;
            n->op = op;
            n->kids.push_back(std::move(lhs));
            n->kids.push_back(term());
            lhs = std::move(n);
        }
        return lhs;
    }
    Ptr term() {
        Ptr lhs = factor();
        while (cur_.kind == Token::Op && (cur_.op == '*' || cur_.op == '/')) {
            char op = cur_.op;
            advance();
            auto n = std::make_unique<Node>();
            n->kind = Node::Bin;
            n->op = op;
            n->kids.push_back(std::move(lhs));
            n->kids.push_back(factor());
            lhs = std::move(n);
        }
        return lhs;
    }
    Ptr factor() {
        if (cur_.kind == Token::Op && cur_.op == '-') {
            advance();
            auto n = std::make_unique<Node>();
            n->kind = Node::Neg;
            n->kids.push_back(factor());
            return n;
        }
        if (cur_.kind == Token::Num) {
            auto n = std::make_unique<Node>();
            n->kind = Node::Num;
            n->num = cur_.num;
            advance();
            return n;
        }
        if (cur_.kind == Token::LParen) {
            advance();
            Ptr e = expr();
            if (cur_.kind != Token::RParen) throw std::runtime_error("expected )");
            advance();
            return e;
        }
        if (cur_.kind == Token::Ident) {
            std::string name = cur_.text;
            advance();
            if (cur_.kind == Token::LParen) {  // function call
                advance();
                auto n = std::make_unique<Node>();
                n->kind = Node::Call;
                n->name = name;
                n->kids.push_back(expr());
                while (cur_.kind == Token::Comma) {
                    advance();
                    n->kids.push_back(expr());
                }
                if (cur_.kind != Token::RParen) throw std::runtime_error("expected )");
                advance();
                return n;
            }
            auto n = std::make_unique<Node>();  // bare column
            n->kind = Node::Col;
            n->name = name;
            return n;
        }
        throw std::runtime_error("unexpected token in factor");
    }

    Lexer lex_;
    Token cur_;
};

// ---- evaluator ------------------------------------------------------------
class Evaluator {
public:
    explicit Evaluator(const TickTable& t) : t_(t) {}

    std::vector<double> run(const std::string& src) {
        Ptr ast = Parser(src).parse();
        Value r = eval(*ast);
        const size_t n = t_.size();
        if (r.scalar) return std::vector<double>(n, r.s);
        return r.v;
    }

private:
    const Column& column(const std::string& name) const {
        if (name == "price") return t_.price;
        if (name == "volume") return t_.volume;
        if (name == "ts") return t_.ts_ns;
        throw std::runtime_error("unknown column: " + name);
    }

    // constant-fold a numeric argument (window sizes, alphas)
    double num_arg(const Node& n) const {
        Value v = eval(n);
        if (!v.scalar) throw std::runtime_error("expected numeric argument");
        return v.s;
    }

    Value from_vec(std::vector<double> v) const {
        Value r;
        r.scalar = false;
        r.v = std::move(v);
        return r;
    }

    Value eval(const Node& n) const {
        switch (n.kind) {
            case Node::Num: {
                Value v;
                v.scalar = true;
                v.s = n.num;
                return v;
            }
            case Node::Col:
                return from_vec(std::vector<double>(column(n.name).raw(),
                                                    column(n.name).raw() +
                                                        column(n.name).size()));
            case Node::Neg: {
                Value a = eval(*n.kids[0]);
                if (a.scalar) { a.s = -a.s; return a; }
                for (double& x : a.v) x = -x;
                return a;
            }
            case Node::Bin: {
                Value a = eval(*n.kids[0]);
                Value b = eval(*n.kids[1]);
                return apply(n.op, a, b);
            }
            case Node::Call:
                return call(n);
        }
        throw std::runtime_error("bad node");
    }

    Value call(const Node& n) const {
        const std::string& f = n.name;
        auto col_of = [&](size_t k) -> const Column& {
            if (n.kids[k]->kind != Node::Col)
                throw std::runtime_error(f + ": arg must be a column name");
            return column(n.kids[k]->name);
        };
        if (f == "mean" && n.kids.size() == 2)
            return from_vec(fast::rolling_mean(col_of(0),
                                               (size_t)num_arg(*n.kids[1])));
        if (f == "std" && n.kids.size() == 2)
            return from_vec(fast::rolling_std(col_of(0),
                                              (size_t)num_arg(*n.kids[1])));
        if (f == "ema" && n.kids.size() == 2)
            return from_vec(naive::ema(col_of(0), num_arg(*n.kids[1])));
        if (f == "vwap" && n.kids.size() == 3)
            return from_vec(fast::rolling_vwap(col_of(0), col_of(1),
                                               (size_t)num_arg(*n.kids[2])));
        throw std::runtime_error("unknown function/arity: " + f);
    }

    Value apply(char op, const Value& a, const Value& b) const {
        if (a.scalar && b.scalar) {
            Value r;
            r.scalar = true;
            r.s = binop(op, a.s, b.s);
            return r;
        }
        const size_t n = a.scalar ? b.v.size() : a.v.size();
        std::vector<double> out(n);
        for (size_t i = 0; i < n; ++i) out[i] = binop(op, a.at(i), b.at(i));
        return from_vec(std::move(out));
    }

    static double binop(char op, double x, double y) {
        switch (op) {
            case '+': return x + y;
            case '-': return x - y;
            case '*': return x * y;
            case '/': return x / y;
        }
        return std::nan("");
    }

    const TickTable& t_;
};

// convenience one-shot
inline std::vector<double> evaluate(const TickTable& t, const std::string& src) {
    return Evaluator(t).run(src);
}

}  // namespace aegis::expr
