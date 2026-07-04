#pragma once

#include <array>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "../column.hpp"
#include "../rolling.hpp"
#include "../rolling_fast.hpp"
#include "core.hpp"

namespace aegis::kernels {

enum class KernelClass : uint8_t {
    WINDOW,
    REDUCTION,
    TRANSFORM,
    NORMALIZATION,
    MATRIX
};

struct RegistryEntry {
    std::string_view name;
    KernelClass klass;
    struct Call {
        std::vector<ConstMatView> inputs;
        MatView output;
        size_t window = 0;
        double scalar = 0.0;
    };
    using Fn = std::function<void(Call&)>;
    Fn naive_fn;
    Fn best_fn;
    double oracle_tol;
};

inline Column as_column(ConstMatView x) {
    if (x.cols != 1 || x.row_stride != 1)
        throw std::invalid_argument("window kernel requires contiguous column");
    return Column::view(x.ptr, x.rows);
}
inline void copy_column(const std::vector<double>& v, MatView out) {
    if (out.cols != 1 || out.rows != v.size())
        throw std::invalid_argument("window output shape mismatch");
    std::copy(v.begin(), v.end(), out.ptr);
}

inline const std::array<RegistryEntry, 12>& registry() {
    using C = RegistryEntry::Call;
    static const std::array<RegistryEntry, 12> entries{{
        {"rolling_mean", KernelClass::WINDOW,
         [](C& c) { copy_column(aegis::naive::rolling_mean(as_column(c.inputs.at(0)), c.window), c.output); },
         [](C& c) { copy_column(aegis::fast::rolling_mean(as_column(c.inputs.at(0)), c.window), c.output); }, 1e-9},
        {"rolling_std", KernelClass::WINDOW,
         [](C& c) { copy_column(aegis::naive::rolling_std(as_column(c.inputs.at(0)), c.window), c.output); },
         [](C& c) { copy_column(aegis::fast::rolling_std(as_column(c.inputs.at(0)), c.window), c.output); }, 1e-8},
        {"rolling_vwap", KernelClass::WINDOW,
         [](C& c) { copy_column(aegis::naive::rolling_vwap(as_column(c.inputs.at(0)), as_column(c.inputs.at(1)), c.window), c.output); },
         [](C& c) { copy_column(aegis::fast::rolling_vwap(as_column(c.inputs.at(0)), as_column(c.inputs.at(1)), c.window), c.output); }, 1e-9},
        {"sum", KernelClass::REDUCTION,
         [](C& c) { c.output(0, 0) = naive::sum(c.inputs.at(0).column(0)); },
         [](C& c) { c.output(0, 0) = best::sum(c.inputs.at(0).column(0)); }, 1e-12},
        {"max", KernelClass::REDUCTION,
         [](C& c) { c.output(0, 0) = naive::max(c.inputs.at(0).column(0)); },
         [](C& c) { c.output(0, 0) = best::max(c.inputs.at(0).column(0)); }, 0.0},
        {"dot", KernelClass::REDUCTION,
         [](C& c) { c.output(0, 0) = naive::dot(c.inputs.at(0).column(0), c.inputs.at(1).column(0)); },
         [](C& c) { c.output(0, 0) = best::dot(c.inputs.at(0).column(0), c.inputs.at(1).column(0)); }, 1e-11},
        {"ema", KernelClass::TRANSFORM,
         [](C& c) { naive::ema(c.inputs.at(0).column(0), c.scalar, c.output.column(0)); },
         [](C& c) { best::ema(c.inputs.at(0).column(0), c.scalar, c.output.column(0)); }, 1e-12},
        {"zscore", KernelClass::TRANSFORM,
         [](C& c) { naive::zscore(c.inputs.at(0).column(0), c.output.column(0)); },
         [](C& c) { best::zscore(c.inputs.at(0).column(0), c.output.column(0)); }, 1e-10},
        {"gelu", KernelClass::TRANSFORM,
         [](C& c) { naive::gelu(c.inputs.at(0), c.output); },
         [](C& c) { best::gelu(c.inputs.at(0), c.output); }, 1e-12},
        {"softmax", KernelClass::NORMALIZATION,
         [](C& c) { naive::softmax(c.inputs.at(0), c.output); },
         [](C& c) { best::softmax(c.inputs.at(0), c.output); }, 1e-12},
        {"layernorm", KernelClass::NORMALIZATION,
         [](C& c) { naive::layernorm(c.inputs.at(0), c.output, c.scalar > 0 ? c.scalar : 1e-5); },
         [](C& c) { best::layernorm(c.inputs.at(0), c.output, c.scalar > 0 ? c.scalar : 1e-5); }, 1e-10},
        {"matmul", KernelClass::MATRIX,
         [](C& c) { naive::matmul(c.inputs.at(0), c.inputs.at(1), c.output); },
         [](C& c) { best::matmul(c.inputs.at(0), c.inputs.at(1), c.output); }, 1e-10},
    }};
    return entries;
}

inline std::optional<RegistryEntry> find(std::string_view name) {
    for (const auto& e : registry()) if (e.name == name) return e;
    return std::nullopt;
}

inline std::string_view class_name(KernelClass c) {
    switch (c) {
        case KernelClass::WINDOW: return "WINDOW";
        case KernelClass::REDUCTION: return "REDUCTION";
        case KernelClass::TRANSFORM: return "TRANSFORM";
        case KernelClass::NORMALIZATION: return "NORMALIZATION";
        case KernelClass::MATRIX: return "MATRIX";
    }
    return "UNKNOWN";
}

}  // namespace aegis::kernels
