#pragma once
// Work-stealing DAG scheduler — the alternative to Scheduler's level barrier.
//
// The level scheduler synchronizes the whole pool at every topological level:
// a level's wall time is its SLOWEST task, so with skewed task durations the
// pool idles behind one straggler per level (sum of per-level maxima). This
// scheduler removes the barrier entirely: a node becomes runnable the moment
// its own dependencies finish, independent of anything else in its "level".
// The bound drops to the critical path (max over chains of the chain sum),
// which for skewed graphs is far smaller — bench_scheduler measures the gap
// and test_runtime proves result-equivalence on randomized DAGs.
//
// Mechanics (the classic Cilk/TBB shape, mutexed deques instead of a
// lock-free Chase-Lev — correctness first, and the deque is not the
// bottleneck at kernel-sized tasks):
//   - one deque per worker: the OWNER pushes/pops at the BACK (LIFO keeps the
//     working set hot in its own cache); THIEVES steal from the FRONT (FIFO
//     takes the oldest, largest-remaining-subtree work, minimizing steals);
//   - a finishing node decrements each dependent's pending count and pushes
//     the ones that hit zero onto the FINISHER's own deque (locality: the
//     data a dependent reads was just written by this worker);
//   - idle workers scan all deques; only after a full failed sweep do they
//     wait on an epoch counter bumped by every push (no missed wakeups: the
//     epoch is checked under the wait predicate).
//
// Exceptions: the first one is captured, the run is cancelled (remaining
// nodes drain without invoking), and submit() rethrows — same contract as
// Scheduler, where future::get() rethrows.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "task_graph.hpp"
#include "trace.hpp"

namespace aegis::runtime {

struct StealingStats {
    uint64_t tasks = 0;
    uint64_t graph_runs = 0;
    uint64_t elapsed_ns = 0;
    uint64_t steals = 0;      // tasks acquired from another worker's deque
    uint64_t local_pops = 0;  // tasks acquired from the owner's deque
};

class StealingScheduler {
public:
    explicit StealingScheduler(size_t workers = std::thread::hardware_concurrency()) {
        if (workers == 0) workers = 1;
        deques_ = std::vector<Deque>(workers);
        threads_.reserve(workers);
        for (size_t i = 0; i < workers; ++i)
            threads_.emplace_back([this, i] { worker(i); });
    }
    ~StealingScheduler() {
        {
            std::lock_guard lock(wake_mu_);
            stop_ = true;
        }
        wake_cv_.notify_all();
        for (auto& t : threads_) t.join();
    }
    StealingScheduler(const StealingScheduler&) = delete;
    StealingScheduler& operator=(const StealingScheduler&) = delete;

    void submit(TaskGraph& graph, TraceRecorder* trace = nullptr) {
        std::lock_guard run_guard(submit_mu_);  // one graph in flight at a time
        const auto begin = std::chrono::steady_clock::now();

        auto& nodes = graph.mutable_nodes();
        pending_ = std::make_unique<std::atomic<size_t>[]>(nodes.size());
        dependents_.assign(nodes.size(), {});
        size_t live = 0;
        for (const auto& n : nodes) {
            if (!n.live) continue;
            ++live;
            size_t deps = 0;
            for (NodeId d : n.dependencies) {
                if (!nodes[d].live) continue;
                ++deps;
                dependents_[d].push_back(n.id);
            }
            pending_[n.id].store(deps, std::memory_order_relaxed);
        }
        graph.levels();  // reuse its cycle check: throws before we start work
        graph_ = &graph;
        trace_ = trace;
        error_ = nullptr;
        cancelled_.store(false, std::memory_order_relaxed);
        remaining_.store(live, std::memory_order_relaxed);
        stats_.tasks += live;

        // Seed the sources round-robin so every worker starts with something.
        size_t lane = 0;
        for (const auto& n : nodes) {
            if (!n.live || pending_[n.id].load(std::memory_order_relaxed) != 0)
                continue;
            push(lane++ % deques_.size(), n.id);
        }
        if (live == 0) {
            ++stats_.graph_runs;
            return;
        }
        {
            std::lock_guard lock(wake_mu_);
            active_ = true;
            ++run_gen_;  // lets a worker distinguish "new run" from "same run"
        }
        wake_cv_.notify_all();

        std::unique_lock done_lock(done_mu_);
        done_cv_.wait(done_lock, [this] {
            return remaining_.load(std::memory_order_acquire) == 0;
        });
        {
            std::lock_guard lock(wake_mu_);
            active_ = false;
        }
        ++stats_.graph_runs;
        stats_.elapsed_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - begin).count();
        if (error_) std::rethrow_exception(error_);
    }

    StealingStats stats() const {
        StealingStats s = stats_;
        s.steals = steals_.load(std::memory_order_relaxed);
        s.local_pops = local_pops_.load(std::memory_order_relaxed);
        return s;
    }
    size_t size() const { return threads_.size(); }

private:
    struct Deque {
        std::mutex mu;
        std::deque<NodeId> q;
    };

    void push(size_t lane, NodeId id) {
        {
            std::lock_guard lock(deques_[lane].mu);
            deques_[lane].q.push_back(id);
        }
        epoch_.fetch_add(1, std::memory_order_release);
        wake_cv_.notify_all();
    }

    bool pop_local(size_t lane, NodeId& out) {
        std::lock_guard lock(deques_[lane].mu);
        if (deques_[lane].q.empty()) return false;
        out = deques_[lane].q.back();  // owner: LIFO, cache-warm end
        deques_[lane].q.pop_back();
        return true;
    }

    bool steal(size_t thief, NodeId& out) {
        const size_t n = deques_.size();
        for (size_t hop = 1; hop < n; ++hop) {  // scan away from own lane
            Deque& victim = deques_[(thief + hop) % n];
            std::lock_guard lock(victim.mu);
            if (victim.q.empty()) continue;
            out = victim.q.front();  // thief: FIFO, oldest/biggest work
            victim.q.pop_front();
            return true;
        }
        return false;
    }

    void run_node(size_t lane, NodeId id) {
        TaskNode& node = graph_->mutable_nodes()[id];
        if (!cancelled_.load(std::memory_order_relaxed)) {
            try {
                if (trace_) {
                    const auto t0 = std::chrono::steady_clock::now();
                    node.invoke();
                    trace_->record(node.id, node.name, node.klass, t0,
                                   std::chrono::steady_clock::now());
                } else {
                    node.invoke();
                }
            } catch (...) {
                std::lock_guard lock(error_mu_);
                if (!error_) error_ = std::current_exception();
                cancelled_.store(true, std::memory_order_relaxed);
            }
        }
        // Even when cancelled, dependents must drain so remaining_ hits zero.
        for (NodeId dep : dependents_[id])
            if (pending_[dep].fetch_sub(1, std::memory_order_acq_rel) == 1)
                push(lane, dep);
        if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            {
                std::lock_guard lock(done_mu_);
                done_cv_.notify_all();  // release the submitter
            }
            wake_cv_.notify_all();  // and any workers parked in the epoch wait
        }
    }

    void worker(size_t lane) {
        uint64_t joined_run = 0;
        for (;;) {
            {
                std::unique_lock lock(wake_mu_);
                wake_cv_.wait(lock, [this, joined_run] {
                    return stop_ || (active_ && run_gen_ != joined_run);
                });
                if (stop_) return;
                joined_run = run_gen_;
            }
            while (remaining_.load(std::memory_order_acquire) != 0) {
                // Capture the epoch BEFORE scanning. A push that lands after
                // the scan started bumps the epoch past `seen`, so the wait
                // predicate below fires instead of sleeping through it. The
                // reverse order is a lost-wakeup deadlock: scan misses the
                // push, then sleeps on an epoch that already includes it.
                const uint64_t seen = epoch_.load(std::memory_order_acquire);
                NodeId id;
                if (pop_local(lane, id)) {
                    local_pops_.fetch_add(1, std::memory_order_relaxed);
                    run_node(lane, id);
                } else if (steal(lane, id)) {
                    steals_.fetch_add(1, std::memory_order_relaxed);
                    run_node(lane, id);
                } else {
                    std::unique_lock lock(wake_mu_);
                    // wait_for: 2ms ceiling prevents permanent stalls if a
                    // notify slips past under heavy instrumentation (ASan).
                    wake_cv_.wait_for(lock, std::chrono::milliseconds(2),
                                      [this, seen] {
                        return stop_ ||
                               epoch_.load(std::memory_order_acquire) != seen ||
                               remaining_.load(std::memory_order_acquire) == 0;
                    });
                    if (stop_) return;
                }
            }
        }
    }

    std::vector<Deque> deques_;
    std::vector<std::thread> threads_;
    std::vector<std::vector<NodeId>> dependents_;
    std::unique_ptr<std::atomic<size_t>[]> pending_;
    TaskGraph* graph_ = nullptr;
    TraceRecorder* trace_ = nullptr;

    std::mutex submit_mu_;
    std::mutex wake_mu_;
    std::condition_variable wake_cv_;
    std::mutex done_mu_;
    std::condition_variable done_cv_;
    std::mutex error_mu_;
    std::exception_ptr error_;
    std::atomic<size_t> remaining_{0};
    std::atomic<uint64_t> epoch_{0};
    std::atomic<bool> cancelled_{false};
    // workers bump these concurrently; tasks/graph_runs/elapsed_ns are only
    // touched by submit(), which is serialized by submit_mu_
    std::atomic<uint64_t> steals_{0};
    std::atomic<uint64_t> local_pops_{0};
    bool active_ = false;
    bool stop_ = false;
    uint64_t run_gen_ = 0;  // guarded by wake_mu_
    StealingStats stats_;
};

}  // namespace aegis::runtime
