// SPSC ring throughput: two threads, one pushing and one popping N ticks.
// Measures three layouts to isolate the cost of each optimization:
//   baseline       : head_/tail_ share a cache line, no index caching
//   +alignas(64)   : head_/tail_ on separate lines (kills false sharing)
//   +cached indices : also skip the cross-core atomic load on the hot path
//
// NOTE ON PINNING: on Linux you'd pin producer/consumer to sibling-free cores
// with pthread_setaffinity_np so the false-sharing effect is clean and stable.
// macOS has no portable affinity API, so threads float — the deltas still show
// but with more run-to-run noise. Numbers below are best-of-3 to compensate.
#include <chrono>
#include <cstdio>
#include <thread>

#include "../src/ring_buffer.hpp"

using namespace aegis;
using Clock = std::chrono::steady_clock;

static inline void cpu_relax() {
#if defined(__x86_64__)
    __asm__ __volatile__("pause");
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}

template <typename Ring>
static double run_once(size_t n) {
    Ring ring;
    uint64_t checksum = 0;
    auto t0 = Clock::now();

    std::thread producer([&] {
        for (uint64_t i = 0; i < n; ++i) {
            Tick tk{i, 1.0, 1.0};
            while (!ring.try_push(tk)) cpu_relax();
        }
    });
    // consumer on this thread
    uint64_t got = 0;
    while (got < n) {
        auto v = ring.try_pop();
        if (v) {
            checksum += v->ts_ns;
            ++got;
        } else {
            cpu_relax();
        }
    }
    producer.join();
    auto t1 = Clock::now();

    // guard against the compiler optimizing the loop away
    const uint64_t expect = n * (n - 1) / 2;
    if (checksum != expect) {
        std::printf("  CHECKSUM MISMATCH (%llu vs %llu)\n",
                    (unsigned long long)checksum, (unsigned long long)expect);
    }
    return std::chrono::duration<double>(t1 - t0).count();
}

template <typename Ring>
static void bench(const char* name, size_t n) {
    double best = 1e30;
    for (int r = 0; r < 3; ++r) best = std::min(best, run_once<Ring>(n));
    std::printf("%-24s %8.1f M ops/sec   (%.3f s)\n", name, n / best / 1e6,
                best);
}

int main(int argc, char** argv) {
    const size_t N = argc > 1 ? std::stoull(argv[1]) : 100'000'000;
    constexpr size_t CAP = 65536;
    std::printf("aegis ring bench: N=%zu, capacity=%zu\n\n", N, CAP);

    bench<SpscRing<Tick, CAP, false, false>>("baseline (shared line)", N);
    bench<SpscRing<Tick, CAP, true, false>>("+alignas(64)", N);
    bench<SpscRing<Tick, CAP, true, true>>("+cached indices", N);
    return 0;
}
