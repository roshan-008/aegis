#pragma once
// Contiguous columnar storage. Deliberately minimal: the point of columnar
// layout is that hot loops iterate over a single dense array of doubles.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace aegis {

struct Column {
    std::vector<double> data;
    // View mode: when ext_ != nullptr the column is a NON-OWNING view over
    // external memory (e.g. an mmap'd file) — zero-copy. Owning and view modes
    // share the same read interface so every kernel works on either unchanged.
    const double* ext_ = nullptr;
    size_t ext_n_ = 0;

    void reserve(size_t n) { data.reserve(n); }
    void push(double v) { data.push_back(v); }
    size_t size() const { return ext_ ? ext_n_ : data.size(); }
    const double* raw() const { return ext_ ? ext_ : data.data(); }
    double operator[](size_t i) const { return raw()[i]; }

    // Build a zero-copy view over [p, p+n). No allocation, no copy.
    static Column view(const double* p, size_t n) {
        Column c;
        c.ext_ = p;
        c.ext_n_ = n;
        return c;
    }
};

// A tick table: parallel columns, same length. Struct-of-arrays, not
// array-of-structs — this is the whole thesis of the project.
struct TickTable {
    Column price;
    Column volume;
    Column ts_ns;  // nanosecond timestamps

    size_t size() const { return price.size(); }
    void reserve(size_t n) {
        price.reserve(n);
        volume.reserve(n);
        ts_ns.reserve(n);
    }
};

}  // namespace aegis
