#pragma once

#include <vector>

#include "../src/kernels/core.hpp"
#include "../src/runtime/task_graph.hpp"

namespace aegis::examples {

struct AnalyticsPipeline {
    std::vector<double> ema;
    std::vector<double> zscore;
    std::vector<double> signal;
    runtime::TaskGraph graph;

    explicit AnalyticsPipeline(ConstColView price)
        : ema(price.rows), zscore(price.rows), signal(price.rows) {
        auto e = graph.add_node("ema", kernels::KernelClass::TRANSFORM, [=, this] {
            kernels::best::ema(price, .1, col_view(ema.data(), ema.size()));
        });
        auto z = graph.add_node("zscore", kernels::KernelClass::TRANSFORM, [this] {
            kernels::best::zscore(col_view(static_cast<const double*>(ema.data()), ema.size()),
                                  col_view(zscore.data(), zscore.size()));
        });
        auto s = graph.add_node("signal", kernels::KernelClass::TRANSFORM, [this] {
            for (size_t i = 0; i < signal.size(); ++i) signal[i] = zscore[i] > 1.0 ? 1.0 : 0.0;
        }, true);
        graph.add_edge(e, z); graph.add_edge(z, s);
    }
};

struct MlpPipeline {
    std::vector<double> hidden;
    std::vector<double> normalized;
    std::vector<double> probabilities;
    runtime::TaskGraph graph;

    MlpPipeline(ConstMatView input, ConstMatView weights)
        : hidden(input.rows * weights.cols), normalized(hidden.size()),
          probabilities(hidden.size()) {
        const size_t rows = input.rows, cols = weights.cols;
        auto mm = graph.add_node("matmul", kernels::KernelClass::MATRIX, [=, this] {
            kernels::best::matmul(input, weights, mat_view(hidden.data(), rows, cols));
        });
        auto ln = graph.add_node("layernorm", kernels::KernelClass::NORMALIZATION, [=, this] {
            kernels::best::layernorm(mat_view(static_cast<const double*>(hidden.data()), rows, cols),
                                     mat_view(normalized.data(), rows, cols));
        });
        auto sm = graph.add_node("softmax", kernels::KernelClass::NORMALIZATION, [=, this] {
            kernels::best::softmax(mat_view(static_cast<const double*>(normalized.data()), rows, cols),
                                   mat_view(probabilities.data(), rows, cols));
        }, true);
        graph.add_edge(mm, ln); graph.add_edge(ln, sm);
    }
};

}  // namespace aegis::examples
