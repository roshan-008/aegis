#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "../column.hpp"

extern "C" {

struct AegisRustSegmentMeta {
    uint64_t n;
    double minimum[3];
    double maximum[3];
};

void* aegis_rs_segment_open(const char* path, AegisRustSegmentMeta* meta,
                            char* error, size_t error_capacity);
const double* aegis_rs_segment_column(const void* handle, size_t column);
void aegis_rs_segment_close(void* handle);
int aegis_rs_store_append(const char* path, const double* price,
                          const double* volume, const double* timestamps,
                          size_t n, char* error, size_t error_capacity);
int aegis_rs_store_recover(const char* directory, size_t* removed,
                           char* error, size_t error_capacity);
}

namespace aegis::storage::rust {

inline std::runtime_error failure(const char* operation,
                                  const std::array<char, 512>& error) {
    return std::runtime_error(std::string(operation) + ": " + error.data());
}

inline void* open(const std::string& path, AegisRustSegmentMeta& meta) {
    std::array<char, 512> error{};
    void* handle = aegis_rs_segment_open(path.c_str(), &meta, error.data(), error.size());
    if (!handle) throw failure("Rust segment open", error);
    return handle;
}

inline const double* column(const void* handle, size_t index) {
    const double* result = aegis_rs_segment_column(handle, index);
    if (!result) throw std::runtime_error("Rust segment returned a null column");
    return result;
}

inline void close(void* handle) noexcept { aegis_rs_segment_close(handle); }

inline void append(const TickTable& table, const std::string& path) {
    std::array<char, 512> error{};
    if (aegis_rs_store_append(path.c_str(), table.price.raw(), table.volume.raw(),
                              table.ts_ns.raw(), table.size(), error.data(),
                              error.size()) != 0)
        throw failure("Rust segment seal", error);
}

inline size_t recover(const std::string& directory) {
    std::array<char, 512> error{};
    size_t removed = 0;
    if (aegis_rs_store_recover(directory.c_str(), &removed, error.data(),
                               error.size()) != 0)
        throw failure("Rust WAL recovery", error);
    return removed;
}

}  // namespace aegis::storage::rust
