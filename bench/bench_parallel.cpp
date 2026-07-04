// Scaling of the halo-partitioned parallel rolling mean: rows/sec vs thread
// count. Expected sub-linear — the per-core kernel is light on compute and the
// shared memory subsystem saturates after a few cores. The point is to SHOW
// where it flattens and be able to explain why (bandwidth, not Amdahl: the
// serial fraction here is ~0).
#include <cstdio>
#include <chrono>
#include <random>

#include "../src/column.hpp"
#include "../src/parallel.hpp"
#include "../src/rolling_fast.hpp"

using namespace aegis;
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) {
    const size_t N = argc > 1 ? std::stoull(argv[1]) : 50'000'000;
    const size_t W = 100;
    Column c;
    c.reserve(N);
    std::mt19937_64 rng(42);
    std::normal_distribution<double> ret(0.0, 1e-4);
    double px = 100.0;
    for (size_t i = 0; i < N; ++i) {
        px *= (1.0 + ret(rng));
        c.push(px);
    }

    std::printf("aegis parallel scaling: N=%zu, window=%zu, hw_threads=%u\n\n",
                N, W, std::thread::hardware_concurrency());

    // Preallocate the output ONCE, outside timing, so we measure the kernel and
    // not a 400MB malloc+memset per call. First-touch it single-threaded here.
    std::vector<double> out(N, 0.0);

    // Honest single-core baseline: run the sliding kernel INLINE on the main
    // thread (a performance core). A spawned std::thread on M1 can land on an
    // efficiency core, which would make T=1 artificially slow and every speedup
    // look superlinear. So the baseline is the main-thread compute.
    const double invw = 1.0 / static_cast<double>(W);
    const double* p = c.raw();
    double base = 1e30;
    for (int r = 0; r < 3; ++r) {
        auto t0 = Clock::now();
        double sum = 0.0;
        for (size_t j = 0; j < W; ++j) sum += p[j];
        out[W - 1] = sum * invw;
        for (size_t i = W; i < N; ++i) {
            sum += p[i] - p[i - W];
            out[i] = sum * invw;
        }
        base = std::min(base, std::chrono::duration<double>(Clock::now() - t0).count());
    }

    std::printf("%-14s %12s %10s\n", "config", "M rows/sec", "speedup");
    std::printf("%-14s %12.1f %10.2f\n", "1 (inline)", N / base / 1e6, 1.0);
    for (unsigned T : {2u, 4u, 8u}) {
        double best = 1e30;
        for (int r = 0; r < 3; ++r) {
            auto t0 = Clock::now();
            par::rolling_mean_into(c, W, T, out);
            best = std::min(best,
                            std::chrono::duration<double>(Clock::now() - t0).count());
        }
        std::printf("%-14u %12.1f %10.2f\n", T, N / best / 1e6, base / best);
    }
    std::puts(
        "\nNote: M1 big.LITTLE + no thread pinning (no portable macOS API) makes"
        "\nthe curve noisy; a clean scaling plot needs homogeneous cores / Linux"
        "\naffinity. Trend (parallelism helps, sub-linear) is the takeaway.");
    return 0;
}
