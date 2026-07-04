#pragma once
// O(n) sliding-window kernels — the M2 algorithmic win over aegis::naive.
//
// The whole idea: a length-w window that advances one step does not need to
// be re-summed. Add the entering element, subtract the leaving one. Each
// output becomes O(1) instead of O(w), so the pass is O(n) instead of O(n*w).
//
// Two things make this subtle, and both are interview material:
//   1. Accumulator drift. Millions of += / -= on a floating-point running
//      sum accumulate rounding error. We bound it by periodically discarding
//      the accumulator and re-summing the current window exactly (RESYNC).
//   2. Variance cancellation. The one-pass identity var = E[x^2] - E[x]^2
//      subtracts two large nearly-equal numbers when the data is far from
//      zero (prices ~100, or worse ~1e6). That loses precision no matter how
//      accurate the accumulator is. We defeat it by SHIFTING the data by a
//      constant K near the mean before accumulating — variance is invariant
//      under translation, and (x-K) is small, so the cancellation vanishes.
//      K is re-centered to the window mean at each resync. See rolling_std.
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include "column.hpp"
#include "rolling.hpp"  // naive:: is the small-window fallback and the oracle

namespace aegis::fast {

// Re-sum the accumulators exactly every RESYNC steps to bound drift.
// w/RESYNC extra adds amortized (~2.4% at w=100) buys full accuracy.
inline constexpr size_t RESYNC = 4096;

// Rolling mean, window w. Sliding sum, O(n). Matches naive to ~1e-12.
inline std::vector<double> rolling_mean(const Column& c, size_t w) {
    const size_t n = c.size();
    std::vector<double> out(n, std::nan(""));
    if (w == 0 || n < w) return out;

    const double invw = 1.0 / static_cast<double>(w);  // avoid per-output divide
    double sum = 0.0;
    for (size_t j = 0; j < w; ++j) sum += c[j];  // warm the first window
    out[w - 1] = sum * invw;

    size_t since_resync = 0;
    for (size_t i = w; i < n; ++i) {
        sum += c[i] - c[i - w];
        if (++since_resync == RESYNC) {  // discard drift, re-sum exactly
            sum = 0.0;
            for (size_t j = i + 1 - w; j <= i; ++j) sum += c[j];
            since_resync = 0;
        }
        out[i] = sum * invw;
    }
    return out;
}

// Rolling VWAP, window w. Two sliding accumulators, O(n).
inline std::vector<double> rolling_vwap(const Column& price,
                                        const Column& volume, size_t w) {
    const size_t n = price.size();
    std::vector<double> out(n, std::nan(""));
    if (volume.size() != n) throw std::invalid_argument("vwap column size mismatch");
    if (w == 0 || n < w) return out;

    double pv = 0.0, v = 0.0;
    for (size_t j = 0; j < w; ++j) {
        pv += price[j] * volume[j];
        v += volume[j];
    }
    out[w - 1] = v > 0.0 ? pv / v : std::nan("");

    size_t since_resync = 0;
    for (size_t i = w; i < n; ++i) {
        pv += price[i] * volume[i] - price[i - w] * volume[i - w];
        v += volume[i] - volume[i - w];
        if (++since_resync == RESYNC) {
            pv = 0.0;
            v = 0.0;
            for (size_t j = i + 1 - w; j <= i; ++j) {
                pv += price[j] * volume[j];
                v += volume[j];
            }
            since_resync = 0;
        }
        out[i] = v > 0.0 ? pv / v : std::nan("");
    }
    return out;
}

// Rolling population std-dev, window w. Sliding sum + sum-of-squares of the
// SHIFTED data (x - K), O(n). K is re-centered to the window mean at each
// resync so (x-K) stays ~std-sized, killing the cancellation in
// var = (sumsq - sum^2/w)/w. Matches naive two-pass to ~1e-12 even at large
// offsets. Compare with rolling_std_onepass below to see why the shift matters.
// Sliding only pays off for large windows. Below this, the exact O(w) two-pass
// recompute is both cheap AND numerically exact — and incremental one-pass
// variance provably cannot match a two-pass oracle when the window is tiny and
// the variance is near zero (the running sum-of-squares carries rounding at the
// magnitude of the data). So we hand small windows to the exact implementation.
inline constexpr size_t STD_SLIDE_MIN = 33;

inline std::vector<double> rolling_std(const Column& c, size_t w) {
    const size_t n = c.size();
    std::vector<double> out(n, std::nan(""));
    if (w == 0 || n < w) return out;
    if (w < STD_SLIDE_MIN) return naive::rolling_std(c, w);

    auto exact_resync = [&](size_t i, double& K, double& sum, double& sumsq) {
        // center on the window mean, then accumulate shifted moments exactly
        double s = 0.0;
        for (size_t j = i + 1 - w; j <= i; ++j) s += c[j];
        K = s / static_cast<double>(w);
        sum = 0.0;
        sumsq = 0.0;
        for (size_t j = i + 1 - w; j <= i; ++j) {
            const double d = c[j] - K;
            sum += d;
            sumsq += d * d;
        }
    };

    double K = 0.0, sum = 0.0, sumsq = 0.0;
    exact_resync(w - 1, K, sum, sumsq);
    const double invw = 1.0 / static_cast<double>(w);
    auto emit = [&](double sum_, double sumsq_) {
        double var = (sumsq_ - sum_ * sum_ * invw) * invw;
        return std::sqrt(var > 0.0 ? var : 0.0);  // clamp tiny negatives
    };
    out[w - 1] = emit(sum, sumsq);

    size_t since_resync = 0;
    for (size_t i = w; i < n; ++i) {
        // shifted sliding update: -K cancels in the sum difference; sumsq keeps
        // the shift so it stays ~w·variance in magnitude (small → no cancel).
        const double dn = c[i] - K;
        const double dold = c[i - w] - K;
        sum += dn - dold;
        sumsq += dn * dn - dold * dold;
        if (++since_resync == RESYNC) {  // re-center K + re-sum exactly
            exact_resync(i, K, sum, sumsq);
            since_resync = 0;
        }
        out[i] = emit(sum, sumsq);
    }
    return out;
}

// UNSHIFTED one-pass sliding std (K=0). Fast but numerically fragile: at large
// data offsets sumsq and sum^2/w are huge and nearly equal, so the subtraction
// loses ~8 significant digits. Kept ONLY as the negative example the torture
// test contrasts against rolling_std above. Do not use in production.
inline std::vector<double> rolling_std_onepass(const Column& c, size_t w) {
    const size_t n = c.size();
    std::vector<double> out(n, std::nan(""));
    if (w == 0 || n < w) return out;
    double sum = 0.0, sumsq = 0.0;
    for (size_t j = 0; j < w; ++j) {
        sum += c[j];
        sumsq += c[j] * c[j];
    }
    const double invw = 1.0 / static_cast<double>(w);
    auto emit = [&] {
        double var = (sumsq - sum * sum * invw) * invw;
        return std::sqrt(var > 0.0 ? var : 0.0);
    };
    out[w - 1] = emit();
    for (size_t i = w; i < n; ++i) {
        sum += c[i] - c[i - w];
        sumsq += c[i] * c[i] - c[i - w] * c[i - w];
        out[i] = emit();
    }
    return out;
}

}  // namespace aegis::fast
