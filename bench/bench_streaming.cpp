// Streaming path: a producer thread feeds synthetic ticks through the SPSC
// ring; a consumer thread pops each tick, updates an online O(1) rolling mean,
// and records the enqueue→dequeue latency. We report percentiles, not the mean,
// because tail latency is the quant-relevant number — a p99.9 stall is the fill
// you lose, and an average hides it.
//
// REGIME NOTE: the producer runs full-throttle. If the consumer keeps up, the
// ring stays near-empty and the latency measured is the true SPSC handoff
// (~inter-core line latency). If the consumer falls behind, the ring fills and
// the number becomes queue-residency (backlog). The output prints the mean
// queue occupancy so you can tell which regime you measured — this is the
// distinction that makes the number meaningful.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

#include "../src/ring_buffer.hpp"
#include "../src/mem/alloc_counter.hpp"

using namespace aegis;
using Clock = std::chrono::steady_clock;

static inline uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               Clock::now().time_since_epoch())
        .count();
}

int main(int argc, char** argv) {
    const size_t N = argc > 1 ? std::stoull(argv[1]) : 5'000'000;
    // Optional target producer rate in M ticks/sec (0 = full throttle). Pacing
    // the producer below the consumer's drain rate keeps the ring near-empty so
    // the measured latency is the true SPSC handoff, not queue backlog.
    const double rate_m = argc > 2 ? std::stod(argv[2]) : 0.0;
    const double interval_ns = rate_m > 0.0 ? 1000.0 / rate_m : 0.0;
    constexpr size_t CAP = 4096;
    constexpr size_t W = 100;
    std::printf("aegis streaming: N=%zu, cap=%zu, rolling_mean(%zu), producer=%s\n\n",
                N, CAP, W,
                rate_m > 0.0 ? (std::to_string(rate_m) + "M/s paced").c_str()
                             : "full-throttle");

    SpscRing<Tick, CAP> ring;
    std::vector<uint32_t> lat(N);   // per-tick latency, ns (uint32 = up to 4.3s)

    auto t0 = Clock::now();
    std::thread producer([&] {
        std::mt19937_64 rng(42);
        std::normal_distribution<double> ret(0.0, 1e-4);
        double px = 100.0;
        const uint64_t start = now_ns();
        for (uint64_t i = 0; i < N; ++i) {
            if (interval_ns > 0.0) {  // pace: spin until this tick's slot
                const uint64_t due = start + static_cast<uint64_t>(i * interval_ns);
                while (now_ns() < due) { /* busy-wait to target rate */ }
            }
            px *= (1.0 + ret(rng));
            Tick tk{now_ns(), px, 1.0};
            while (!ring.try_push(tk)) { /* spin: consumer behind */ }
        }
    });

    // consumer: online rolling mean over a circular buffer + running sum
    std::vector<double> win(W, 0.0);
    size_t wi = 0, filled = 0;
    double rsum = 0.0;
    volatile double sink = 0.0;
    uint64_t empty_polls = 0;
    mem::HotPathAllocationScope no_allocations;
    for (uint64_t got = 0; got < N;) {
        auto v = ring.try_pop();
        if (!v) {
            ++empty_polls;  // ring was empty → consumer is keeping up
            continue;
        }
        const uint64_t t_now = now_ns();
        // online O(1) rolling mean
        rsum -= win[wi];
        win[wi] = v->price;
        rsum += v->price;
        wi = (wi + 1) % W;
        if (filled < W) ++filled;
        sink = rsum / static_cast<double>(filled);
        // latency
        uint64_t d = t_now - v->ts_ns;
        lat[got] = d > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(d);
        ++got;
    }
    producer.join();
    if (mem::AllocationCounter::hot_path() != 0) {
        std::fprintf(stderr, "FAIL: hot path performed %llu project allocations\n",
                     static_cast<unsigned long long>(mem::AllocationCounter::hot_path()));
        return 1;
    }
    (void)sink;
    auto t1 = Clock::now();

    double secs = std::chrono::duration<double>(t1 - t0).count();
    std::sort(lat.begin(), lat.end());
    auto pct = [&](double p) { return lat[static_cast<size_t>(p * (N - 1))]; };

    std::printf("throughput    %8.1f M ticks/sec\n", N / secs / 1e6);
    std::printf("latency p50   %8u ns\n", pct(0.50));
    std::printf("latency p99   %8u ns\n", pct(0.99));
    std::printf("latency p99.9 %8u ns\n", pct(0.999));
    std::printf("latency max   %8u ns\n", lat.back());
    // Regime indicator: fraction of consumer polls that found the ring empty.
    // High → consumer keeps up, ring near-empty, latency ≈ true handoff.
    // ~0  → consumer saturated, ring full, latency ≈ queue backlog.
    std::printf("\nconsumer empty-poll ratio %.1f%%  (%s)\n",
                100.0 * empty_polls / (empty_polls + N),
                empty_polls > N / 10 ? "handoff regime" : "backlog regime");
    return 0;
}
