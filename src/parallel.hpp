#pragma once
// Multi-core rolling mean. The window makes this look sequential, but it
// parallelizes cleanly with OVERLAPPING HALOS: partition the OUTPUT indices
// into T contiguous chunks; each thread owns outputs [a, b) and reads the
// w-1 input elements before a (its halo) to warm its own running sum. No
// shared state, no locks, no atomics — embarrassingly parallel once you pay
// the halo (each thread redundantly sums one window to start).
//
// Expected scaling: sub-linear. The kernel is latency/bandwidth-light per core
// (see docs/profiling.md) so a few cores saturate the shared memory subsystem;
// the scaling bench shows exactly where it flattens.
#include <cstddef>
#include <thread>
#include <vector>

#include "column.hpp"
#include "rolling_fast.hpp"

namespace aegis::par {

// In-place variant: writes into a caller-owned buffer (out.size() >= c.size()).
// Keeps allocation OUT of the timed region so the scaling bench measures the
// kernel, not malloc/memset. Returns the number of valid outputs written.
inline void rolling_mean_into(const Column& c, size_t w, unsigned nthreads,
                              std::vector<double>& out) {
    const size_t n = c.size();
    if (w == 0 || n < w) return;
    if (nthreads < 1) nthreads = 1;

    const size_t first = w - 1;          // first index with a full window
    for (size_t i = 0; i < first; ++i) out[i] = std::nan("");
    const size_t total = n - first;      // number of valid outputs
    if (nthreads > total) nthreads = static_cast<unsigned>(total);
    const double invw = 1.0 / static_cast<double>(w);
    const double* p = c.raw();

    auto worker = [&](size_t a, size_t b) {
        // a..b are OUTPUT indices this thread owns (a >= first). Warm the sum
        // over the window ending at a — this reads the halo [a-w+1, a].
        double sum = 0.0;
        for (size_t j = a + 1 - w; j <= a; ++j) sum += p[j];
        out[a] = sum * invw;
        size_t since_resync = 0;
        for (size_t i = a + 1; i < b; ++i) {
            sum += p[i] - p[i - w];
            if (++since_resync == fast::RESYNC) {
                sum = 0.0;
                for (size_t j = i + 1 - w; j <= i; ++j) sum += p[j];
                since_resync = 0;
            }
            out[i] = sum * invw;
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(nthreads);
    const size_t per = total / nthreads;
    for (unsigned t = 0; t < nthreads; ++t) {
        const size_t a = first + t * per;
        const size_t b = (t == nthreads - 1) ? n : first + (t + 1) * per;
        pool.emplace_back(worker, a, b);
    }
    for (auto& th : pool) th.join();
}

// Allocating convenience wrapper (same result as fast::rolling_mean).
inline std::vector<double> rolling_mean(const Column& c, size_t w,
                                        unsigned nthreads) {
    std::vector<double> out(c.size(), std::nan(""));
    rolling_mean_into(c, w, nthreads, out);
    return out;
}

}  // namespace aegis::par
