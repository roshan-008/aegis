#pragma once

#include <chrono>
#include <cstdint>
#include <future>
#include <vector>

#include "task_graph.hpp"
#include "worker_pool.hpp"

namespace aegis::runtime {

struct ScheduleStats {
    uint64_t tasks = 0;
    uint64_t graph_runs = 0;
    uint64_t elapsed_ns = 0;
};

class Scheduler {
public:
    explicit Scheduler(size_t workers = std::thread::hardware_concurrency())
        : pool_(workers) {}

    void submit(TaskGraph& graph) {
        const auto begin = std::chrono::steady_clock::now();
        for (const auto& level : graph.levels()) {
            std::vector<std::future<void>> futures;
            futures.reserve(level.size());
            for (NodeId id : level) {
                ++stats_.tasks;
                futures.push_back(pool_.submit(graph.mutable_nodes()[id].invoke));
            }
            for (auto& future : futures) future.get();
        }
        ++stats_.graph_runs;
        stats_.elapsed_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - begin).count();
    }
    const ScheduleStats& stats() const { return stats_; }

private:
    WorkerPool pool_;
    ScheduleStats stats_;
};

}  // namespace aegis::runtime
