#pragma once
// Single-producer single-consumer lock-free ring buffer.
// This is the ONE concurrency primitive in the project. The memory_order
// choices and the cache-line layout are the interview questions.
//
// Two template flags exist so bench/bench_ring.cpp can measure the cost of
// each optimization against a deliberately-naive baseline:
//   Padded : put head_ and tail_ on separate 64-byte cache lines. Without it
//            the producer's store to tail_ invalidates the consumer's cached
//            line holding head_ on every push — false sharing, MESI coherence
//            traffic on the hot path.
//   Cached : producer keeps a private copy of head_ (and consumer of tail_),
//            reloading the real atomic only when the ring *appears* full/empty.
//            Turns a cross-core acquire-load on every call into ~one per wrap.
// Defaults are the optimized version; the smoke test uses those.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace aegis {

template <typename T, size_t CapacityPow2, bool Padded = true,
          bool Cached = true>
class SpscRing {
    static_assert((CapacityPow2 & (CapacityPow2 - 1)) == 0,
                  "capacity must be power of two");
    static constexpr size_t MASK = CapacityPow2 - 1;
    // Baseline packs the two atoms adjacent (same line → false sharing);
    // padded gives each its own 64B line.
    static constexpr size_t ALIGN = Padded ? 64 : alignof(std::atomic<size_t>);

public:
    // Producer side. Returns false if full.
    bool try_push(const T& v) {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if constexpr (Cached) {
            if (t - head_cache_ == CapacityPow2) {          // maybe full
                head_cache_ = head_.load(std::memory_order_acquire);
                if (t - head_cache_ == CapacityPow2) return false;  // really full
            }
        } else {
            const size_t h = head_.load(std::memory_order_acquire);
            if (t - h == CapacityPow2) return false;
        }
        buf_[t & MASK] = v;
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns nullopt if empty.
    std::optional<T> try_pop() {
        const size_t h = head_.load(std::memory_order_relaxed);
        if constexpr (Cached) {
            if (h == tail_cache_) {                          // maybe empty
                tail_cache_ = tail_.load(std::memory_order_acquire);
                if (h == tail_cache_) return std::nullopt;   // really empty
            }
        } else {
            const size_t t = tail_.load(std::memory_order_acquire);
            if (h == t) return std::nullopt;
        }
        T v = buf_[h & MASK];
        head_.store(h + 1, std::memory_order_release);
        return v;
    }

    size_t approximate_size() const {
        const size_t t = tail_.load(std::memory_order_acquire);
        const size_t h = head_.load(std::memory_order_acquire);
        return t - h;
    }
    static constexpr size_t capacity() { return CapacityPow2; }

private:
    // Producer-owned line: tail_ + producer's cached head.
    alignas(ALIGN) std::atomic<size_t> tail_{0};
    size_t head_cache_{0};
    // Consumer-owned line: head_ + consumer's cached tail.
    alignas(ALIGN) std::atomic<size_t> head_{0};
    size_t tail_cache_{0};
    // Data on its own line too.
    alignas(ALIGN) T buf_[CapacityPow2];
};

struct Tick {
    uint64_t ts_ns;
    double price;
    double volume;
};

}  // namespace aegis
