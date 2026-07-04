#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>

#include "alloc_counter.hpp"

namespace aegis::mem {

class Arena {
public:
    explicit Arena(size_t bytes, bool prefault = true)
        : capacity_(round_up(bytes, kAlignment)),
          base_(static_cast<std::byte*>(
              ::operator new(capacity_, std::align_val_t{kAlignment}))) {
        AllocationCounter::record_allocation();
        if (prefault) {
            constexpr size_t page = 4096;
            for (size_t i = 0; i < capacity_; i += page) base_[i] = std::byte{0};
            if (capacity_) base_[capacity_ - 1] = std::byte{0};
        }
    }
    ~Arena() { ::operator delete(base_, std::align_val_t{kAlignment}); }
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    void* allocate(size_t bytes, size_t alignment = kAlignment) {
        if (alignment == 0 || (alignment & (alignment - 1)) != 0)
            throw std::invalid_argument("arena alignment must be a power of two");
        const size_t aligned = round_up(offset_, alignment);
        if (bytes > capacity_ - std::min(aligned, capacity_))
            throw std::bad_alloc();
        offset_ = aligned + bytes;
        high_water_ = std::max(high_water_, offset_);
        return base_ + aligned;
    }

    template <typename T>
    T* allocate_n(size_t n) {
        return static_cast<T*>(allocate(sizeof(T) * n,
                                        std::max(alignof(T), kAlignment)));
    }

    void reset() noexcept { offset_ = 0; }
    size_t used() const noexcept { return offset_; }
    size_t capacity() const noexcept { return capacity_; }
    size_t high_water() const noexcept { return high_water_; }

    static constexpr size_t kAlignment = 64;

private:
    static size_t round_up(size_t n, size_t alignment) {
        return (n + alignment - 1) & ~(alignment - 1);
    }
    size_t capacity_ = 0;
    std::byte* base_ = nullptr;
    size_t offset_ = 0;
    size_t high_water_ = 0;
};

}  // namespace aegis::mem
