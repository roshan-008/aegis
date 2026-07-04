// Benchmark harness. Reports rows/sec for each rolling op over synthetic
// ticks. Fixed seed => reproducible. Extend with p50/p99 per-tick latency
// when you add the streaming (ring-buffer) path.
#include <chrono>
#include <cstdio>
#include <random>

#include "../src/column.hpp"
#include "../src/rolling.hpp"
#include "../src/rolling_fast.hpp"
#include "../src/simd.hpp"
#include "harness.hpp"

using namespace aegis;
using Clock = std::chrono::steady_clock;

static TickTable gen_ticks(size_t n, uint64_t seed = 42) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> ret(0.0, 1e-4);
    std::lognormal_distribution<double> vol(4.0, 1.0);
    TickTable t;
    t.reserve(n);
    double px = 100.0;
    uint64_t ts = 0;
    for (size_t i = 0; i < n; ++i) {
        px *= (1.0 + ret(rng));
        ts += 1000 + (rng() % 9000);  // 1–10 microsecond gaps
        t.price.push(px);
        t.volume.push(vol(rng));
        t.ts_ns.push(static_cast<double>(ts));
    }
    return t;
}

template <typename F>
static double time_op(const char* name, size_t n, F&& f) {
    // Warmup once, then time best-of-3.
    volatile double sink = f();
    double best = 1e30;
    for (int r = 0; r < 3; ++r) {
        auto t0 = Clock::now();
        sink = f();
        auto t1 = Clock::now();
        double s = std::chrono::duration<double>(t1 - t0).count();
        if (s < best) best = s;
    }
    (void)sink;
    const double mrows = n / best / 1e6;
    std::printf("%-22s %12.1f M rows/sec   (%.3f s)\n", name, mrows, best);
    return mrows;
}

int main(int argc, char** argv) {
    const size_t N = argc > 1 ? std::stoull(argv[1]) : 10'000'000;
    const size_t W = 100;
    std::printf("aegis bench: N=%zu, window=%zu\n\n", N, W);
    TickTable t = gen_ticks(N);

    std::puts("-- naive O(n*w) --");
    time_op("rolling_mean(100)", N, [&] {
        auto v = naive::rolling_mean(t.price, W);
        return v.back();
    });
    time_op("rolling_std(100)", N, [&] {
        auto v = naive::rolling_std(t.price, W);
        return v.back();
    });
    time_op("ema(0.1)", N, [&] {
        auto v = naive::ema(t.price, 0.1);
        return v.back();
    });
    time_op("rolling_vwap(100)", N, [&] {
        auto v = naive::rolling_vwap(t.price, t.volume, W);
        return v.back();
    });

    std::printf("\n-- naive + SIMD O(n*w), backend: %s --\n", simd::backend());
    time_op("rolling_mean(100)", N, [&] {
        auto v = simd::rolling_mean(t.price, W);
        return v.back();
    });

    std::puts("\n-- fast O(n) sliding --");
    aegis::bench::record_metric("rolling", "fast_mean_mrows",
                                time_op("rolling_mean(100)", N, [&] {
                                    auto v = fast::rolling_mean(t.price, W);
                                    return v.back();
                                }));
    aegis::bench::record_metric("rolling", "fast_std_mrows",
                                time_op("rolling_std(100)", N, [&] {
                                    auto v = fast::rolling_std(t.price, W);
                                    return v.back();
                                }));
    aegis::bench::record_metric("rolling", "fast_vwap_mrows",
                                time_op("rolling_vwap(100)", N, [&] {
                                    auto v = fast::rolling_vwap(t.price, t.volume, W);
                                    return v.back();
                                }));
    return 0;
}
