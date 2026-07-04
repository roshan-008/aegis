#pragma once

#include <cstddef>
#include <cstdint>
#include <thread>

#include "../ring_buffer.hpp"
#include "observability.hpp"

namespace aegis::runtime {

enum class BackpressurePolicy : uint8_t { Drop, Block };

template <typename T, size_t CapacityPow2>
class BoundedChannel {
public:
    explicit BoundedChannel(BackpressurePolicy policy = BackpressurePolicy::Drop,
                            RuntimeStats* stats = nullptr)
        : policy_(policy), stats_(stats) {}

    bool push(const T& value) {
        if (policy_ == BackpressurePolicy::Drop) {
            if (!ring_.try_push(value)) {
                if (stats_) stats_->dropped.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        } else {
            while (!ring_.try_push(value)) {
                if (stats_) stats_->blocked_spins.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
        }
        if (stats_) {
            stats_->accepted.fetch_add(1, std::memory_order_relaxed);
            stats_->ring_depth.store(ring_.approximate_size(), std::memory_order_relaxed);
        }
        return true;
    }
    std::optional<T> pop() {
        auto value = ring_.try_pop();
        if (stats_)
            stats_->ring_depth.store(ring_.approximate_size(), std::memory_order_relaxed);
        return value;
    }
    size_t depth() const { return ring_.approximate_size(); }

private:
    SpscRing<T, CapacityPow2> ring_;
    BackpressurePolicy policy_;
    RuntimeStats* stats_;
};

}  // namespace aegis::runtime
