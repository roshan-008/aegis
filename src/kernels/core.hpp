#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>

#include "../span.hpp"

namespace aegis::kernels {

inline void require_same_shape(ConstMatView a, MatView out) {
    if (a.rows != out.rows || a.cols != out.cols)
        throw std::invalid_argument("kernel shape mismatch");
}

namespace naive {

inline double sum(ConstColView x) {
    double s = 0.0;
    for (size_t i = 0; i < x.rows; ++i) s += x(i);
    return s;
}
inline double max(ConstColView x) {
    double m = -std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < x.rows; ++i) m = std::max(m, x(i));
    return m;
}
inline double dot(ConstColView a, ConstColView b) {
    if (a.rows != b.rows) throw std::invalid_argument("dot shape mismatch");
    double s = 0.0;
    for (size_t i = 0; i < a.rows; ++i) s += a(i) * b(i);
    return s;
}
inline void ema(ConstColView x, double alpha, ColView out) {
    if (x.rows != out.rows || alpha <= 0.0 || alpha > 1.0)
        throw std::invalid_argument("ema arguments");
    if (!x.rows) return;
    out(0) = x(0);
    for (size_t i = 1; i < x.rows; ++i)
        out(i) = alpha * x(i) + (1.0 - alpha) * out(i - 1);
}
inline void zscore(ConstColView x, ColView out) {
    if (x.rows != out.rows) throw std::invalid_argument("zscore shape");
    if (!x.rows) return;
    const double mean = sum(x) / static_cast<double>(x.rows);
    double q = 0.0;
    for (size_t i = 0; i < x.rows; ++i) {
        const double d = x(i) - mean;
        q += d * d;
    }
    const double sd = std::sqrt(q / static_cast<double>(x.rows));
    for (size_t i = 0; i < x.rows; ++i)
        out(i) = sd == 0.0 ? 0.0 : (x(i) - mean) / sd;
}
inline void gelu(ConstMatView x, MatView out) {
    require_same_shape(x, out);
    constexpr double inv_sqrt2 = 0.70710678118654752440;
    for (size_t r = 0; r < x.rows; ++r)
        for (size_t c = 0; c < x.cols; ++c)
            out(r, c) = 0.5 * x(r, c) *
                        (1.0 + std::erf(x(r, c) * inv_sqrt2));
}
inline void softmax(ConstMatView x, MatView out) {
    require_same_shape(x, out);
    for (size_t r = 0; r < x.rows; ++r) {
        double mx = -std::numeric_limits<double>::infinity();
        for (size_t c = 0; c < x.cols; ++c) mx = std::max(mx, x(r, c));
        double denom = 0.0;
        for (size_t c = 0; c < x.cols; ++c) {
            out(r, c) = std::exp(x(r, c) - mx);
            denom += out(r, c);
        }
        for (size_t c = 0; c < x.cols; ++c) out(r, c) /= denom;
    }
}
inline void layernorm(ConstMatView x, MatView out, double eps = 1e-5) {
    require_same_shape(x, out);
    if (x.cols == 0) return;
    for (size_t r = 0; r < x.rows; ++r) {
        double mean = 0.0;
        for (size_t c = 0; c < x.cols; ++c) mean += x(r, c);
        mean /= static_cast<double>(x.cols);
        double var = 0.0;
        for (size_t c = 0; c < x.cols; ++c) {
            const double d = x(r, c) - mean;
            var += d * d;
        }
        const double inv = 1.0 / std::sqrt(var / x.cols + eps);
        for (size_t c = 0; c < x.cols; ++c)
            out(r, c) = (x(r, c) - mean) * inv;
    }
}
inline void matmul(ConstMatView a, ConstMatView b, MatView out) {
    if (a.cols != b.rows || out.rows != a.rows || out.cols != b.cols)
        throw std::invalid_argument("matmul shape mismatch");
    for (size_t i = 0; i < out.rows; ++i)
        for (size_t j = 0; j < out.cols; ++j) {
            double s = 0.0;
            for (size_t k = 0; k < a.cols; ++k) s += a(i, k) * b(k, j);
            out(i, j) = s;
        }
}

}  // namespace naive

namespace best {

inline double sum(ConstColView x) {
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
    size_t i = 0;
    for (; i + 4 <= x.rows; i += 4) {
        s0 += x(i); s1 += x(i + 1); s2 += x(i + 2); s3 += x(i + 3);
    }
    for (; i < x.rows; ++i) s0 += x(i);
    return (s0 + s1) + (s2 + s3);
}
inline double max(ConstColView x) { return naive::max(x); }
inline double dot(ConstColView a, ConstColView b) {
    if (a.rows != b.rows) throw std::invalid_argument("dot shape mismatch");
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
    size_t i = 0;
    for (; i + 4 <= a.rows; i += 4) {
        s0 += a(i) * b(i); s1 += a(i + 1) * b(i + 1);
        s2 += a(i + 2) * b(i + 2); s3 += a(i + 3) * b(i + 3);
    }
    for (; i < a.rows; ++i) s0 += a(i) * b(i);
    return (s0 + s1) + (s2 + s3);
}
inline void ema(ConstColView x, double alpha, ColView out) {
    naive::ema(x, alpha, out);  // scan dependency: intentionally scalar
}
inline void zscore(ConstColView x, ColView out) { naive::zscore(x, out); }
inline void gelu(ConstMatView x, MatView out) { naive::gelu(x, out); }
inline void softmax(ConstMatView x, MatView out) { naive::softmax(x, out); }
inline void layernorm(ConstMatView x, MatView out, double eps = 1e-5) {
    naive::layernorm(x, out, eps);
}
inline void matmul(ConstMatView a, ConstMatView b, MatView out) {
    if (a.cols != b.rows || out.rows != a.rows || out.cols != b.cols)
        throw std::invalid_argument("matmul shape mismatch");
    // Zero row by row: a single fill over rows*row_stride would scribble on
    // the gap columns of a strided (submatrix) output view.
    for (size_t r = 0; r < out.rows; ++r)
        std::fill(out.ptr + r * out.row_stride,
                  out.ptr + r * out.row_stride + out.cols, 0.0);
    constexpr size_t BS = 32;
    for (size_t ii = 0; ii < a.rows; ii += BS)
        for (size_t kk = 0; kk < a.cols; kk += BS)
            for (size_t jj = 0; jj < b.cols; jj += BS)
                for (size_t i = ii; i < std::min(ii + BS, a.rows); ++i)
                    for (size_t k = kk; k < std::min(kk + BS, a.cols); ++k) {
                        const double av = a(i, k);
                        for (size_t j = jj; j < std::min(jj + BS, b.cols); ++j)
                            out(i, j) = std::fma(av, b(k, j), out(i, j));
                    }
}

}  // namespace best
}  // namespace aegis::kernels
