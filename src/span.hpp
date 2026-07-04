#pragma once

#include <cassert>
#include <cstddef>
#include <type_traits>

namespace aegis {

template <typename T>
struct Span2D {
    T* ptr = nullptr;
    size_t rows = 0;
    size_t cols = 0;
    size_t row_stride = 0;  // elements, never bytes

    constexpr T& operator()(size_t r, size_t c = 0) const {
        assert(r < rows && c < cols);
        return ptr[r * row_stride + c];
    }
    constexpr bool empty() const { return rows == 0 || cols == 0; }
    constexpr size_t size() const { return rows * cols; }
    constexpr Span2D<T> subrows(size_t first, size_t count) const {
        assert(first + count <= rows);
        return {ptr + first * row_stride, count, cols, row_stride};
    }
    constexpr Span2D<T> column(size_t c) const {
        assert(c < cols);
        return {ptr + c, rows, 1, row_stride};
    }
};

using ColView = Span2D<double>;
using MatView = Span2D<double>;
using ConstColView = Span2D<const double>;
using ConstMatView = Span2D<const double>;

inline ColView col_view(double* p, size_t n) { return {p, n, 1, 1}; }
inline ConstColView col_view(const double* p, size_t n) { return {p, n, 1, 1}; }
inline MatView mat_view(double* p, size_t rows, size_t cols,
                        size_t stride = 0) {
    return {p, rows, cols, stride ? stride : cols};
}
inline ConstMatView mat_view(const double* p, size_t rows, size_t cols,
                             size_t stride = 0) {
    return {p, rows, cols, stride ? stride : cols};
}

static_assert(std::is_trivially_copyable_v<ColView>);

}  // namespace aegis
