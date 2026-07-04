#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "../column.hpp"
#include "rust_bridge.hpp"

namespace aegis::storage {

// Stable C++ mirror of AEGISSEG/v1 metadata. Rust writes and validates the
// serialized 192-byte little-endian header explicitly; it never transmutes
// file bytes into this struct.
struct alignas(64) SegmentHeader {
    char magic[8] = {'A', 'E', 'G', 'I', 'S', 'S', 'E', 'G'};
    uint32_t version = 1;
    uint32_t columns = 3;
    uint64_t n = 0;
    uint64_t offsets[3]{};
    double minimum[3]{};
    double maximum[3]{};
    uint32_t crc[3]{};
    uint32_t header_crc = 0;
    std::array<std::byte, 32> reserved{};
};
static_assert(sizeof(SegmentHeader) == 192);

inline size_t seal_segment(const TickTable& table, const std::string& path) {
    if (table.price.size() != table.volume.size() ||
        table.price.size() != table.ts_ns.size())
        throw std::invalid_argument("segment columns have different lengths");
    rust::append(table, path);  // Rust: WAL + CRC + fsync + atomic rename.
    return sizeof(SegmentHeader) + 3 * table.size() * sizeof(double);
}

}  // namespace aegis::storage
