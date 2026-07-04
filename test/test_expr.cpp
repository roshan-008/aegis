// Expression-layer tests: parse+evaluate must equal direct kernel calls.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/column.hpp"
#include "../src/expr.hpp"
#include "../src/rolling.hpp"
#include "../src/rolling_fast.hpp"

using namespace aegis;

static int failures = 0;
static bool same(double a, double b) {
    if (std::isnan(a) && std::isnan(b)) return true;
    return std::fabs(a - b) <= 1e-9 * (1.0 + std::fabs(b));
}
static void check(const char* expr, const std::vector<double>& got,
                  const std::vector<double>& want) {
    if (got.size() != want.size()) { std::printf("  FAIL %s size\n", expr); ++failures; return; }
    for (size_t i = 0; i < got.size(); ++i)
        if (!same(got[i], want[i])) {
            std::printf("  FAIL %s at %zu: %.10g vs %.10g\n", expr, i, got[i], want[i]);
            ++failures;
            return;
        }
    std::printf("  ok   %s\n", expr);
}

int main() {
    const size_t N = 5000;
    TickTable t;
    t.reserve(N);
    double px = 100.0;
    for (size_t i = 0; i < N; ++i) {
        px *= (1.0 + 1e-3 * std::sin(0.01 * i));
        t.price.push(px);
        t.volume.push(1.0 + (i % 7));
        t.ts_ns.push(static_cast<double>(i));
    }
    const auto& P = t.price.data;

    auto vec = [&](auto f) {
        std::vector<double> o(N);
        for (size_t i = 0; i < N; ++i) o[i] = f(i);
        return o;
    };

    std::puts("expression parser/evaluator:");
    check("price", expr::evaluate(t, "price"), P);
    check("mean(price,100)", expr::evaluate(t, "mean(price,100)"),
          fast::rolling_mean(t.price, 100));
    check("std(price,50)", expr::evaluate(t, "std(price,50)"),
          fast::rolling_std(t.price, 50));
    check("ema(price,0.1)", expr::evaluate(t, "ema(price,0.1)"),
          naive::ema(t.price, 0.1));
    check("vwap(price,volume,50)", expr::evaluate(t, "vwap(price,volume,50)"),
          fast::rolling_vwap(t.price, t.volume, 50));
    check("price + 1", expr::evaluate(t, "price + 1"),
          vec([&](size_t i) { return P[i] + 1.0; }));
    check("2 * price", expr::evaluate(t, "2 * price"),
          vec([&](size_t i) { return 2.0 * P[i]; }));
    check("-price", expr::evaluate(t, "-price"),
          vec([&](size_t i) { return -P[i]; }));
    check("(price + price) / 2", expr::evaluate(t, "(price + price) / 2"), P);
    {  // precedence: 1 + 2*3 = 7, broadcast
        auto r = expr::evaluate(t, "1 + 2 * 3");
        check("1 + 2 * 3", r, std::vector<double>(N, 7.0));
    }
    {  // composed feature
        auto vw = fast::rolling_vwap(t.price, t.volume, 50);
        auto em = naive::ema(t.price, 0.1);
        check("vwap(price,volume,50) - ema(price,0.1)",
              expr::evaluate(t, "vwap(price,volume,50) - ema(price,0.1)"),
              vec([&](size_t i) { return vw[i] - em[i]; }));
    }
    check("price - mean(price,100)", expr::evaluate(t, "price - mean(price,100)"),
          vec([&](size_t i) {
              auto m = fast::rolling_mean(t.price, 100);
              return P[i] - m[i];
          }));

    // malformed inputs must throw, not wrap or read out of bounds
    auto expect_throw = [&](const char* src) {
        try {
            expr::evaluate(t, src);
            std::printf("  FAIL %s: expected an exception\n", src);
            ++failures;
        } catch (const std::exception&) {
            std::printf("  ok   %s throws\n", src);
        }
    };
    expect_throw("mean(price,-1)");     // negative window: no (size_t) wrap
    expect_throw("mean(price,2.5)");    // fractional window
    expect_throw("vwap(price,volume,-3)");
    expect_throw("mean(price)");        // wrong arity
    expect_throw("nope(price,1)");      // unknown function
    expect_throw("price + )");          // parse error

    if (failures) { std::printf("\n%d FAILED\n", failures); return 1; }
    std::puts("\nall expression tests passed");
    return 0;
}
