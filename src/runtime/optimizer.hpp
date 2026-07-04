#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <vector>

#include "task_graph.hpp"

namespace aegis::runtime {

struct OptimizationReport {
    size_t dead_nodes_removed = 0;
    size_t transform_pairs_fused = 0;
};

inline OptimizationReport eliminate_dead_nodes(TaskGraph& graph) {
    auto& nodes = graph.mutable_nodes();
    std::vector<bool> needed(nodes.size(), false);
    std::function<void(NodeId)> visit = [&](NodeId id) {
        if (needed[id] || !nodes[id].live) return;
        needed[id] = true;
        for (NodeId d : nodes[id].dependencies) visit(d);
    };
    for (const auto& n : nodes) if (n.required_output && n.live) visit(n.id);
    OptimizationReport report;
    for (auto& n : nodes) {
        if (n.live && !needed[n.id]) {
            n.live = false;
            ++report.dead_nodes_removed;
        }
    }
    return report;
}

inline OptimizationReport fuse_transform_pairs(TaskGraph& graph) {
    auto& nodes = graph.mutable_nodes();
    std::vector<size_t> users(nodes.size(), 0);
    for (const auto& n : nodes)
        if (n.live) for (NodeId d : n.dependencies) if (nodes[d].live) ++users[d];
    OptimizationReport report;
    for (auto& second : nodes) {
        if (!second.live || second.klass != kernels::KernelClass::TRANSFORM ||
            second.dependencies.size() != 1) continue;
        const NodeId first_id = second.dependencies[0];
        auto& first = nodes[first_id];
        if (!first.live || first.required_output || users[first_id] != 1 ||
            first.klass != kernels::KernelClass::TRANSFORM) continue;
        const auto first_fn = first.invoke;
        const auto second_fn = second.invoke;
        second.invoke = [first_fn, second_fn] { first_fn(); second_fn(); };
        second.name = first.name + "+" + second.name;
        second.dependencies = first.dependencies;
        first.live = false;
        ++report.transform_pairs_fused;
    }
    return report;
}

inline OptimizationReport optimize(TaskGraph& graph) {
    auto dead = eliminate_dead_nodes(graph);
    auto fused = fuse_transform_pairs(graph);
    dead.transform_pairs_fused = fused.transform_pairs_fused;
    return dead;
}

}  // namespace aegis::runtime
