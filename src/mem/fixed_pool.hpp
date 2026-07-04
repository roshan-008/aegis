#pragma once

#include <array>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace aegis::mem {

template <typename T, size_t Capacity>
class FixedPool {
public:
    FixedPool() {
        for (size_t i = 0; i < Capacity; ++i)
            next_[i] = i + 1 < Capacity ? i + 1 : npos;
    }
    ~FixedPool() {
        for (size_t i = 0; i < Capacity; ++i)
            if (live_[i]) ptr(i)->~T();
    }

    template <typename... Args>
    T* create(Args&&... args) {
        if (free_ == npos) return nullptr;
        const size_t i = free_;
        free_ = next_[i];
        live_[i] = true;
        ++size_;
        return new (&storage_[i]) T(std::forward<Args>(args)...);
    }
    void destroy(T* p) {
        if (!p) return;
        const auto* b = reinterpret_cast<const std::byte*>(storage_.data());
        const auto* q = reinterpret_cast<const std::byte*>(p);
        const size_t i = static_cast<size_t>(q - b) / sizeof(Storage);
        if (i >= Capacity || !live_[i]) return;
        p->~T();
        live_[i] = false;
        next_[i] = free_;
        free_ = i;
        --size_;
    }
    size_t size() const { return size_; }
    constexpr size_t capacity() const { return Capacity; }

private:
    using Storage = std::aligned_storage_t<sizeof(T), alignof(T)>;
    T* ptr(size_t i) { return reinterpret_cast<T*>(&storage_[i]); }
    static constexpr size_t npos = static_cast<size_t>(-1);
    std::array<Storage, Capacity> storage_{};
    std::array<size_t, Capacity> next_{};
    std::array<bool, Capacity> live_{};
    size_t free_ = 0;
    size_t size_ = 0;
};

}  // namespace aegis::mem
