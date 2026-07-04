#pragma once
// SIMD-vectorized window sum, used to build the three-way comparison that is
// the whole point of this stage:
//     naive scalar   O(n·w)   — baseline
//     naive + SIMD   O(n·w)   — vectorize the inner window sum
//     fast sliding   O(n)     — the algorithmic win from Stage 2.1
// The lesson the table teaches: a better big-O (sliding) beats a vectorized
// worse one. SIMD is applied to the *reduction* (window sum), which is a pure
// data-parallel add tree. It canNOT be applied to the sliding update, because
// that is a loop-carried dependency (each output needs the previous running
// sum) — which is exactly why the fast path is already near its ceiling and
// SIMD has nothing left to give there.
//
// NEON is the primary backend (arm64 dev machine, 2-wide float64x2_t). The
// AVX2 path (4-wide) is provided for an x86 build/CI; it is compiled only under
// -mavx2 and is UNTESTED on this Apple M1 — label it as such in any writeup.
#include <cmath>
#include <cstddef>
#include <vector>

#include "column.hpp"

#if defined(__ARM_NEON)
#include <arm_neon.h>
#elif defined(__AVX2__)
#include <immintrin.h>
#endif

namespace aegis::simd {

inline const char* backend() {
#if defined(__ARM_NEON)
    return "NEON (2-wide f64)";
#elif defined(__AVX2__)
    return "AVX2 (4-wide f64)";
#else
    return "scalar (no SIMD)";
#endif
}

// Sum w contiguous doubles. Two accumulators break the FP-add dependency chain
// so the pipeline stays full (add latency > throughput on both µarchs).
inline double window_sum(const double* p, size_t w) {
#if defined(__ARM_NEON)
    float64x2_t a0 = vdupq_n_f64(0.0), a1 = vdupq_n_f64(0.0);
    size_t j = 0;
    for (; j + 4 <= w; j += 4) {
        a0 = vaddq_f64(a0, vld1q_f64(p + j));
        a1 = vaddq_f64(a1, vld1q_f64(p + j + 2));
    }
    double s = vaddvq_f64(vaddq_f64(a0, a1));  // horizontal add of 2 lanes
    for (; j < w; ++j) s += p[j];
    return s;
#elif defined(__AVX2__)
    __m256d a0 = _mm256_setzero_pd(), a1 = _mm256_setzero_pd();
    size_t j = 0;
    for (; j + 8 <= w; j += 8) {
        a0 = _mm256_add_pd(a0, _mm256_loadu_pd(p + j));
        a1 = _mm256_add_pd(a1, _mm256_loadu_pd(p + j + 4));
    }
    __m256d a = _mm256_add_pd(a0, a1);
    // horizontal add of 4 lanes: hadd pairs within 128-bit halves, so fold the
    // upper 128 into the lower first, THEN hadd. (_mm256_hadd_pd alone is wrong.)
    __m128d lo = _mm256_castpd256_pd128(a);
    __m128d hi = _mm256_extractf128_pd(a, 1);
    __m128d s2 = _mm_add_pd(lo, hi);
    double s = _mm_cvtsd_f64(_mm_hadd_pd(s2, s2));
    for (; j < w; ++j) s += p[j];
    return s;
#else
    double s = 0.0;
    for (size_t j = 0; j < w; ++j) s += p[j];
    return s;
#endif
}

// Naive O(n·w) rolling mean with a vectorized inner window sum.
inline std::vector<double> rolling_mean(const Column& c, size_t w) {
    const size_t n = c.size();
    std::vector<double> out(n, std::nan(""));
    if (w == 0 || n < w) return out;
    const double* p = c.raw();
    const double invw = 1.0 / static_cast<double>(w);
    for (size_t i = w - 1; i < n; ++i) out[i] = window_sum(p + i + 1 - w, w) * invw;
    return out;
}

}  // namespace aegis::simd
