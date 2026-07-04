#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../mem/arena.hpp"
#include "reader.hpp"
#include "segment.hpp"

namespace aegis::storage {

struct ReplayRange {
    double from_ts = -std::numeric_limits<double>::infinity();
    double to_ts = std::numeric_limits<double>::infinity();
};

struct ReplayBatch {
    ConstColView price;
    ConstColView volume;
    ConstColView timestamps;
    size_t size() const { return price.rows; }
};

class ReplayCursor {
public:
    ReplayCursor(std::vector<std::string> segments, ReplayRange range,
                 double rate_limit, size_t batch_size = 4096)
        : segments_(std::move(segments)), range_(range), rate_limit_(rate_limit),
          batch_size_(batch_size), pace_start_(std::chrono::steady_clock::now()) {}

    std::optional<ReplayBatch> next(mem::Arena& arena) {
        (void)arena;  // API is arena-ready; mmap batches themselves are zero-copy.
        for (;;) {
            if (!reader_ || position_ >= end_) {
                if (!open_next()) return std::nullopt;
            }
            const size_t n = std::min(batch_size_, end_ - position_);
            ReplayBatch batch{reader_->price().subrows(position_, n),
                              reader_->volume().subrows(position_, n),
                              reader_->timestamps().subrows(position_, n)};
            position_ += n;
            emitted_ += n;
            if (rate_limit_ > 0.0) {
                const auto due = pace_start_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(emitted_ / rate_limit_));
                std::this_thread::sleep_until(due);
            }
            return batch;
        }
    }

private:
    bool open_next() {
        reader_.reset();
        while (segment_index_ < segments_.size()) {
            auto candidate = std::make_unique<SegmentReader>(segments_[segment_index_++]);
            const auto& h = candidate->header();
            if (!h.n || h.maximum[2] < range_.from_ts || h.minimum[2] > range_.to_ts)
                continue;  // header min/max segment skipping
            auto ts = candidate->timestamps();
            const double* first = ts.ptr;
            const double* last = ts.ptr + ts.rows;
            const double* begin = std::lower_bound(first, last, range_.from_ts);
            const double* end = std::upper_bound(begin, last, range_.to_ts);
            position_ = static_cast<size_t>(begin - first);
            end_ = static_cast<size_t>(end - first);
            if (position_ == end_) continue;
            reader_ = std::move(candidate);
            return true;
        }
        return false;
    }
    std::vector<std::string> segments_;
    ReplayRange range_;
    double rate_limit_ = 0.0;
    size_t batch_size_ = 4096;
    size_t segment_index_ = 0;
    size_t position_ = 0;
    size_t end_ = 0;
    size_t emitted_ = 0;
    std::chrono::steady_clock::time_point pace_start_;
    std::unique_ptr<SegmentReader> reader_;
};

class SegmentStore {
public:
    explicit SegmentStore(std::string directory)
        : directory_(std::move(directory)) {
        std::filesystem::create_directories(directory_);
        rust::recover(directory_);
    }

    std::string append(const TickTable& table, std::string name = {}) {
        if (name.empty()) {
            const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            name = "segment-" + std::to_string(stamp) + "-" +
                   std::to_string(next_id_++) + ".aegis";
        }
        const std::string path = directory_ + "/" + name;
        seal_segment(table, path);
        return path;
    }

    ReplayCursor open(ReplayRange range = {}, double rate_limit = 0.0,
                      size_t batch_size = 4096) const {
        std::vector<std::string> paths;
        for (const auto& e : std::filesystem::directory_iterator(directory_))
            if (e.is_regular_file() && e.path().extension() == ".aegis")
                paths.push_back(e.path().string());
        std::sort(paths.begin(), paths.end());
        return ReplayCursor(std::move(paths), range, rate_limit, batch_size);
    }

private:
    std::string directory_;
    uint64_t next_id_ = 0;
};

}  // namespace aegis::storage
