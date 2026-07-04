#pragma once
// Zero-copy persistence for a TickTable via mmap.
//
// On-disk format (little-endian, host layout): a 16-byte header then three
// raw double columns back to back.
//   [0..8)   magic "AEGIS\0\0\0"
//   [8..16)  uint64 n
//   [16     .. 16+8n)   price[n]
//   [16+8n  .. 16+16n)  volume[n]
//   [16+16n .. 16+24n)  ts_ns[n]
//
// load() mmaps the file read-only and hands back Column::view spans that point
// straight into the mapping — NO parse, NO copy, NO allocation of the payload.
// The kernels run directly on page-cache-backed memory; the OS faults pages in
// on demand. Concepts this exercises: page cache, demand paging, madvise,
// and the difference between "loading" data and mapping it.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "column.hpp"

namespace aegis {

inline constexpr char kMagic[8] = {'A', 'E', 'G', 'I', 'S', 0, 0, 0};
inline constexpr size_t kHeaderBytes = 16;

// Serialize a TickTable to `path`. Returns bytes written.
inline size_t save_table(const TickTable& t, const std::string& path) {
    const uint64_t n = t.size();
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("save_table: cannot open " + path);
    std::fwrite(kMagic, 1, 8, f);
    std::fwrite(&n, sizeof n, 1, f);
    std::fwrite(t.price.raw(), sizeof(double), n, f);
    std::fwrite(t.volume.raw(), sizeof(double), n, f);
    std::fwrite(t.ts_ns.raw(), sizeof(double), n, f);
    std::fclose(f);
    return kHeaderBytes + 3 * n * sizeof(double);
}

// A read-only mmap of a saved table. Columns are zero-copy views into the map.
class MmapTable {
public:
    explicit MmapTable(const std::string& path) {
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0) throw std::runtime_error("MmapTable: open failed " + path);
        struct stat st;
        if (::fstat(fd_, &st) != 0) throw std::runtime_error("MmapTable: fstat");
        bytes_ = static_cast<size_t>(st.st_size);
        base_ = ::mmap(nullptr, bytes_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (base_ == MAP_FAILED) throw std::runtime_error("MmapTable: mmap");

        const char* p = static_cast<const char*>(base_);
        if (bytes_ < kHeaderBytes || std::memcmp(p, kMagic, 8) != 0)
            throw std::runtime_error("MmapTable: bad magic");
        std::memcpy(&n_, p + 8, sizeof n_);
        const size_t need = kHeaderBytes + 3 * n_ * sizeof(double);
        if (bytes_ < need) throw std::runtime_error("MmapTable: truncated file");

        const double* d = reinterpret_cast<const double*>(p + kHeaderBytes);
        price_ = Column::view(d, n_);
        volume_ = Column::view(d + n_, n_);
        ts_ = Column::view(d + 2 * n_, n_);
    }

    ~MmapTable() {
        if (base_ && base_ != MAP_FAILED) ::munmap(base_, bytes_);
        if (fd_ >= 0) ::close(fd_);
    }
    MmapTable(const MmapTable&) = delete;
    MmapTable& operator=(const MmapTable&) = delete;

    // Hint the kernel we'll stream these pages sequentially.
    void advise_sequential() const {
        ::madvise(base_, bytes_, MADV_SEQUENTIAL | MADV_WILLNEED);
    }

    size_t size() const { return n_; }
    const Column& price() const { return price_; }
    const Column& volume() const { return volume_; }
    const Column& ts() const { return ts_; }

private:
    int fd_ = -1;
    void* base_ = nullptr;
    size_t bytes_ = 0;
    uint64_t n_ = 0;
    Column price_, volume_, ts_;
};

}  // namespace aegis
