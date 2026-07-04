#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <queue>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../kernels/registry.hpp"

namespace aegis::runtime {

using NodeId = size_t;

struct TaskNode {
    NodeId id = 0;
    std::string name;
    kernels::KernelClass klass = kernels::KernelClass::TRANSFORM;
    std::function<void()> invoke;
    std::vector<NodeId> dependencies;
    bool required_output = false;
    bool live = true;
};

class TaskGraph {
public:
    NodeId add_node(std::string name, kernels::KernelClass klass,
                    std::function<void()> invoke, bool required = false) {
        const NodeId id = nodes_.size();
        nodes_.push_back({id, std::move(name), klass, std::move(invoke), {},
                          required, true});
        return id;
    }
    void add_edge(NodeId from, NodeId to) {
        check(from); check(to);
        nodes_[to].dependencies.push_back(from);
    }
    void mark_output(NodeId id) { check(id); nodes_[id].required_output = true; }

    const std::vector<TaskNode>& nodes() const { return nodes_; }
    std::vector<TaskNode>& mutable_nodes() { return nodes_; }

    std::vector<std::vector<NodeId>> levels() const {
        std::vector<size_t> indegree(nodes_.size(), 0);
        std::vector<std::vector<NodeId>> outgoing(nodes_.size());
        size_t live_count = 0;
        for (const auto& n : nodes_) {
            if (!n.live) continue;
            ++live_count;
            for (NodeId d : n.dependencies) {
                if (d >= nodes_.size()) throw std::runtime_error("bad graph edge");
                if (!nodes_[d].live) continue;
                ++indegree[n.id];
                outgoing[d].push_back(n.id);
            }
        }
        std::vector<NodeId> ready;
        for (const auto& n : nodes_)
            if (n.live && indegree[n.id] == 0) ready.push_back(n.id);
        std::vector<std::vector<NodeId>> result;
        size_t visited = 0;
        while (!ready.empty()) {
            result.push_back(ready);
            std::vector<NodeId> next;
            for (NodeId id : ready) {
                ++visited;
                for (NodeId out : outgoing[id])
                    if (--indegree[out] == 0) next.push_back(out);
            }
            ready = std::move(next);
        }
        if (visited != live_count) throw std::runtime_error("task graph contains cycle");
        return result;
    }

private:
    void check(NodeId id) const {
        if (id >= nodes_.size()) throw std::out_of_range("task node id");
    }
    std::vector<TaskNode> nodes_;
};

}  // namespace aegis::runtime
