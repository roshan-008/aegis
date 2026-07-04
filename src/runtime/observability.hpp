#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace aegis::runtime {

class LatencyHistogram {
public:
    void observe(uint64_t ns) {
        size_t bucket = 0;
        uint64_t ceiling = 64;
        while (bucket + 1 < buckets_.size() && ns > ceiling) {
            ++bucket;
            ceiling <<= 1;
        }
        buckets_[bucket].fetch_add(1, std::memory_order_relaxed);
        count_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t percentile(double p) const {
        if (count() == 0) return 0;  // no observations: 64 would be a lie
        const uint64_t target = static_cast<uint64_t>(count() * p);
        uint64_t seen = 0, ceiling = 64;
        for (const auto& b : buckets_) {
            seen += b.load(std::memory_order_relaxed);
            if (seen >= target) return ceiling;
            ceiling <<= 1;
        }
        return ceiling;
    }
    uint64_t count() const { return count_.load(std::memory_order_relaxed); }

private:
    std::array<std::atomic<uint64_t>, 24> buckets_{};
    std::atomic<uint64_t> count_{0};
};

struct RuntimeStats {
    std::atomic<uint64_t> accepted{0};
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint64_t> blocked_spins{0};
    std::atomic<uint64_t> ring_depth{0};
    LatencyHistogram wire_to_feature;
};

}  // namespace aegis::runtime
