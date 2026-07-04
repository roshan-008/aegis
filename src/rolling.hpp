#pragma once
// Rolling-window statistics over columns.
//
// EVERY function here is the NAIVE BASELINE: correct, scalar, cache-friendly
// only by accident of being sequential. Your job (M2) is to beat these with:
//   - sliding-sum O(n) instead of O(n*w) where applicable
//   - SIMD (AVX2) vectorization
//   - multi-threaded chunking
// Keep these as the reference implementations for correctness tests.
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include "column.hpp"

namespace aegis::naive {

// Rolling mean, window w. O(n*w) on purpose — beat it.
inline std::vector<double> rolling_mean(const Column& c, size_t w) {
    const size_t n = c.size();
    std::vector<double> out(n, std::nan(""));
    if (w == 0 || n < w) return out;
    for (size_t i = w - 1; i < n; ++i) {
        double s = 0.0;
        for (size_t j = i + 1 - w; j <= i; ++j) s += c[j];
        out[i] = s / static_cast<double>(w);
    }
    return out;
}

// Rolling population std-dev. O(n*w), two-pass per window.
inline std::vector<double> rolling_std(const Column& c, size_t w) {
    const size_t n = c.size();
    std::vector<double> out(n, std::nan(""));
    if (w == 0 || n < w) return out;
    for (size_t i = w - 1; i < n; ++i) {
        double s = 0.0;
        for (size_t j = i + 1 - w; j <= i; ++j) s += c[j];
        const double mu = s / static_cast<double>(w);
        double q = 0.0;
        for (size_t j = i + 1 - w; j <= i; ++j) {
            const double d = c[j] - mu;
            q += d * d;
        }
        out[i] = std::sqrt(q / static_cast<double>(w));
    }
    return out;
}

// Exponential moving average, smoothing alpha in (0,1].
inline std::vector<double> ema(const Column& c, double alpha) {
    const size_t n = c.size();
    std::vector<double> out(n);
    if (n == 0) return out;
    out[0] = c[0];
    for (size_t i = 1; i < n; ++i)
        out[i] = alpha * c[i] + (1.0 - alpha) * out[i - 1];
    return out;
}

// Rolling VWAP over window w.
inline std::vector<double> rolling_vwap(const Column& price,
                                        const Column& volume, size_t w) {
    const size_t n = price.size();
    std::vector<double> out(n, std::nan(""));
    if (volume.size() != n) throw std::invalid_argument("vwap column size mismatch");
    if (w == 0 || n < w) return out;
    for (size_t i = w - 1; i < n; ++i) {
        double pv = 0.0, v = 0.0;
        for (size_t j = i + 1 - w; j <= i; ++j) {
            pv += price[j] * volume[j];
            v += volume[j];
        }
        out[i] = v > 0.0 ? pv / v : std::nan("");
    }
    return out;
}

}  // namespace aegis::naive
