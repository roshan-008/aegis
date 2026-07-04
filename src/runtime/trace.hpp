#pragma once
// Execution tracing for the DAG scheduler — the observability layer.
//
// Records one span per executed node (begin/end on a steady clock, plus which
// worker ran it) and can emit the standard Chrome trace-event JSON, so a run
// opens directly in chrome://tracing or https://ui.perfetto.dev with zero
// custom UI: worker lanes, per-kernel spans, idle gaps between levels.
//
// It also answers the question a timeline alone doesn't: what bounded the
// run? critical_path() walks the executed DAG with the *measured* durations
// and returns the longest dependency chain — if its total is close to the
// wall span, the graph shape is the bottleneck; if it's far below, the
// scheduler (or worker count) is.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "task_graph.hpp"

namespace aegis::runtime {

struct TraceEvent {
    NodeId node = 0;
    std::string name;
    kernels::KernelClass klass = kernels::KernelClass::TRANSFORM;
    size_t worker = 0;      // dense per-recorder worker index, not a TID hash
    uint64_t begin_ns = 0;  // relative to the recorder's epoch
    uint64_t end_ns = 0;
    uint64_t duration_ns() const { return end_ns - begin_ns; }
};

class TraceRecorder {
public:
    TraceRecorder() : epoch_(std::chrono::steady_clock::now()) {}

    // Called by the scheduler around each node body. Thread-safe; the mutex
    // is uncontended relative to kernel work (one lock per node, not per row).
    void record(NodeId node, std::string_view name, kernels::KernelClass klass,
                std::chrono::steady_clock::time_point begin,
                std::chrono::steady_clock::time_point end) {
        const auto rel = [this](std::chrono::steady_clock::time_point t) {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t - epoch_)
                    .count());
        };
        std::lock_guard lock(mu_);
        events_.push_back({node, std::string(name), klass, worker_index_locked(),
                           rel(begin), rel(end)});
    }

    std::vector<TraceEvent> events() const {
        std::lock_guard lock(mu_);
        return events_;
    }
    size_t worker_count() const {
        std::lock_guard lock(mu_);
        return workers_.size();
    }

    // Wall span (first begin to last end) and total busy time across workers.
    // busy/span/workers is scheduler efficiency; 1.0 means no idle gaps.
    uint64_t span_ns() const {
        std::lock_guard lock(mu_);
        if (events_.empty()) return 0;
        uint64_t lo = events_.front().begin_ns, hi = events_.front().end_ns;
        for (const auto& e : events_) {
            lo = std::min(lo, e.begin_ns);
            hi = std::max(hi, e.end_ns);
        }
        return hi - lo;
    }
    uint64_t busy_ns() const {
        std::lock_guard lock(mu_);
        uint64_t total = 0;
        for (const auto& e : events_) total += e.duration_ns();
        return total;
    }

    struct CriticalPath {
        uint64_t total_ns = 0;
        std::vector<std::string> nodes;  // dependency order, source first
    };

    // Longest dependency chain through the nodes that actually executed,
    // weighted by measured durations. Nodes optimized away (dead, or fused
    // into a successor) carry no event and contribute no weight.
    CriticalPath critical_path(const TaskGraph& graph) const {
        std::unordered_map<NodeId, uint64_t> duration;
        {
            std::lock_guard lock(mu_);
            for (const auto& e : events_) duration[e.node] += e.duration_ns();
        }
        const auto& nodes = graph.nodes();
        std::vector<uint64_t> best(nodes.size(), 0);
        std::vector<NodeId> prev(nodes.size(), SIZE_MAX);
        CriticalPath result;
        NodeId tail = SIZE_MAX;
        for (const auto& level : graph.levels()) {  // topological order
            for (NodeId id : level) {
                uint64_t incoming = 0;
                for (NodeId d : nodes[id].dependencies) {
                    if (!nodes[d].live) continue;
                    if (best[d] > incoming) {
                        incoming = best[d];
                        prev[id] = d;
                    }
                }
                auto it = duration.find(id);
                best[id] = incoming + (it == duration.end() ? 0 : it->second);
                if (best[id] > result.total_ns) {
                    result.total_ns = best[id];
                    tail = id;
                }
            }
        }
        for (NodeId id = tail; id != SIZE_MAX; id = prev[id])
            result.nodes.push_back(nodes[id].name);
        std::reverse(result.nodes.begin(), result.nodes.end());
        return result;
    }

    // Chrome trace-event JSON: complete ("X") events, microsecond timestamps,
    // one tid lane per worker. Opens in chrome://tracing and Perfetto as-is.
    void write_chrome_trace(const std::string& path) const {
        const auto snapshot = events();
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) throw std::runtime_error("trace: cannot open " + path);
        std::fputs("{\"traceEvents\":[", f);
        bool first = true;
        for (const auto& e : snapshot) {
            std::fprintf(
                f, "%s{\"name\":\"%s\",\"cat\":\"%.*s\",\"ph\":\"X\",\"ts\":%.3f,"
                   "\"dur\":%.3f,\"pid\":1,\"tid\":%zu}",
                first ? "" : ",", escape(e.name).c_str(),
                static_cast<int>(kernels::class_name(e.klass).size()),
                kernels::class_name(e.klass).data(), e.begin_ns / 1e3,
                e.duration_ns() / 1e3, e.worker);
            first = false;
        }
        const bool ok = std::fputs("]}\n", f) >= 0;
        if (std::fclose(f) != 0 || !ok)
            throw std::runtime_error("trace: short write to " + path);
    }

private:
    // Dense worker index for this recorder; requires mu_ held.
    size_t worker_index_locked() {
        const auto id = std::this_thread::get_id();
        auto it = workers_.find(id);
        if (it != workers_.end()) return it->second;
        const size_t index = workers_.size();
        workers_.emplace(id, index);
        return index;
    }
    static std::string escape(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            if (c == '"' || c == '\\') out.push_back('\\');
            if (static_cast<unsigned char>(c) >= 0x20) out.push_back(c);
        }
        return out;
    }

    std::chrono::steady_clock::time_point epoch_;
    mutable std::mutex mu_;
    std::vector<TraceEvent> events_;
    std::unordered_map<std::thread::id, size_t> workers_;
};

}  // namespace aegis::runtime
