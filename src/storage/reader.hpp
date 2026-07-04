#pragma once

#include <cstddef>
#include <string>

#include "../span.hpp"
#include "rust_bridge.hpp"
#include "segment.hpp"

namespace aegis::storage {

// C++ owns only this RAII facade. Rust owns mmap lifetime, file-format parsing,
// bounds checks, CRC validation, and sorted-timestamp validation.
class SegmentReader {
public:
    explicit SegmentReader(const std::string& path, bool verify_crc = true) {
        (void)verify_crc;  // Rust always verifies; unsafe opt-outs are not exposed.
        AegisRustSegmentMeta meta{};
        handle_ = rust::open(path, meta);
        header_.n = meta.n;
        for (size_t c = 0; c < 3; ++c) {
            header_.minimum[c] = meta.minimum[c];
            header_.maximum[c] = meta.maximum[c];
            columns_[c] = rust::column(handle_, c);
        }
    }
    ~SegmentReader() { rust::close(handle_); }
    SegmentReader(const SegmentReader&) = delete;
    SegmentReader& operator=(const SegmentReader&) = delete;
    SegmentReader(SegmentReader&&) = delete;
    SegmentReader& operator=(SegmentReader&&) = delete;

    const SegmentHeader& header() const { return header_; }
    size_t size() const { return static_cast<size_t>(header_.n); }
    ConstColView column(size_t c) const {
        if (c >= 3) throw std::out_of_range("segment column");
        return col_view(columns_[c], size());
    }
    ConstColView price() const { return column(0); }
    ConstColView volume() const { return column(1); }
    ConstColView timestamps() const { return column(2); }

private:
    void* handle_ = nullptr;
    SegmentHeader header_{};
    const double* columns_[3]{};
};

}  // namespace aegis::storage
