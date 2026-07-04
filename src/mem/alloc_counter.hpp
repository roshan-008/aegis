#pragma once

#include <atomic>
#include <cstdint>

namespace aegis::mem {

// Project allocation sites call record_allocation(). This deliberately avoids
// replacing global operator new, which would count unrelated libc/runtime work.
class AllocationCounter {
public:
    static void record_allocation() {
#ifndef NDEBUG
        total_.fetch_add(1, std::memory_order_relaxed);
        if (hot_path_) hot_.fetch_add(1, std::memory_order_relaxed);
#endif
    }
    static uint64_t total() { return total_.load(std::memory_order_relaxed); }
    static uint64_t hot_path() { return hot_.load(std::memory_order_relaxed); }
    static void reset_hot_path() { hot_.store(0, std::memory_order_relaxed); }
    static void set_hot_path(bool enabled) { hot_path_ = enabled; }

private:
    inline static std::atomic<uint64_t> total_{0};
    inline static std::atomic<uint64_t> hot_{0};
    inline static thread_local bool hot_path_ = false;
};

class HotPathAllocationScope {
public:
    HotPathAllocationScope() {
        AllocationCounter::reset_hot_path();
        AllocationCounter::set_hot_path(true);
    }
    ~HotPathAllocationScope() { AllocationCounter::set_hot_path(false); }
    HotPathAllocationScope(const HotPathAllocationScope&) = delete;
    HotPathAllocationScope& operator=(const HotPathAllocationScope&) = delete;
};

}  // namespace aegis::mem
