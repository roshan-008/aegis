// Level-barrier vs work-stealing scheduler on the workload shape that
// separates them: independent chains with SKEWED per-node durations.
//
// The level scheduler joins the whole pool at every topological level, so its
// wall time is the sum over levels of the SLOWEST node in each level. The
// stealing scheduler starts a node the moment its own chain is ready, so its
// bound is the heaviest single chain (the critical path). With uniform
// durations the two coincide — that's the control. With skew they diverge,
// and the gap is the price of the barrier, not scheduler overhead.
//
// Both schedulers run the identical pre-generated graph; a checksum over all
// node outputs must match between them or the numbers don't count (the same
// oracle discipline as every kernel bench).
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "../src/runtime/scheduler.hpp"
#include "../src/runtime/trace.hpp"
#include "../src/runtime/work_steal.hpp"
#include "harness.hpp"

using namespace aegis;
using Clock = std::chrono::steady_clock;

static void busy_spin_us(uint32_t us) {
    const auto until = Clock::now() + std::chrono::microseconds(us);
    while (Clock::now() < until) {
    }
}

struct ChainGraph {
    runtime::TaskGraph graph;
    std::vector<double> out;  // one slot per node; checksum oracle

    // `chains` independent chains of `length` nodes; durations[i] per node.
    ChainGraph(size_t chains, size_t length,
               const std::vector<uint32_t>& durations)
        : out(chains * length, 0.0) {
        for (size_t c = 0; c < chains; ++c) {
            runtime::NodeId prev = 0;
            for (size_t l = 0; l < length; ++l) {
                const size_t slot = c * length + l;
                const uint32_t us = durations[slot];
                const auto id = graph.add_node(
                    "c" + std::to_string(c) + "n" + std::to_string(l),
                    kernels::KernelClass::TRANSFORM,
                    [this, slot, us, c, l] {
                        busy_spin_us(us);
                        out[slot] = (l == 0 ? 1.0 : out[slot - 1]) +
                                    static_cast<double>(c + 1);
                    },
                    /*required=*/l + 1 == length);
                if (l > 0) graph.add_edge(prev, id);
                prev = id;
            }
        }
    }
    double checksum() const {
        double s = 0.0;
        for (double v : out) s += v;
        return s;
    }
};

template <typename Submit>
static double run_case(const char* label, size_t chains, size_t length,
                       const std::vector<uint32_t>& durations, Submit&& submit,
                       double* out_checksum) {
    ChainGraph cg(chains, length, durations);
    const auto begin = Clock::now();
    submit(cg.graph);
    const double ms =
        std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    *out_checksum = cg.checksum();
    std::printf("  %-14s %8.2f ms\n", label, ms);
    return ms;
}

int main() {
    const size_t kChains = 8, kLength = 64, kWorkers = 8;
    std::printf("scheduler comparison: %zu chains x %zu nodes, %zu workers\n",
                kChains, kLength, kWorkers);

    runtime::Scheduler level(kWorkers);
    runtime::StealingScheduler stealing(kWorkers);

    std::mt19937_64 rng(7);
    auto durations = [&](bool skewed) {
        std::vector<uint32_t> d(kChains * kLength);
        for (auto& v : d)
            v = skewed ? (rng() % 8 == 0 ? 400u : 20u) : 50u;
        return d;
    };

    for (bool skewed : {false, true}) {
        std::printf("\n-- %s durations --\n",
                    skewed ? "skewed (1-in-8 nodes 20x heavier)" : "uniform");
        const auto d = durations(skewed);
        double sum_level_max = 0.0, max_chain_sum = 0.0;
        for (size_t l = 0; l < kLength; ++l) {
            uint32_t mx = 0;
            for (size_t c = 0; c < kChains; ++c)
                mx = std::max(mx, d[c * kLength + l]);
            sum_level_max += mx;
        }
        for (size_t c = 0; c < kChains; ++c) {
            double s = 0.0;
            for (size_t l = 0; l < kLength; ++l) s += d[c * kLength + l];
            max_chain_sum = std::max(max_chain_sum, s);
        }
        std::printf("  model: barrier bound %.2f ms, critical path %.2f ms\n",
                    sum_level_max / 1e3, max_chain_sum / 1e3);

        double cks_level = 0.0, cks_steal = 0.0;
        const double ms_level = run_case(
            "level-barrier", kChains, kLength, d,
            [&](runtime::TaskGraph& g) { level.submit(g); }, &cks_level);
        const auto steals_before = stealing.stats().steals;
        const double ms_steal = run_case(
            "work-stealing", kChains, kLength, d,
            [&](runtime::TaskGraph& g) { stealing.submit(g); }, &cks_steal);
        if (cks_level != cks_steal) {
            std::printf("  ORACLE MISMATCH %.17g vs %.17g\n", cks_level, cks_steal);
            return 1;
        }
        std::printf("  speedup %.2fx  (steals this case: %llu)  oracle ok\n",
                    ms_level / ms_steal,
                    static_cast<unsigned long long>(stealing.stats().steals -
                                                    steals_before));
        bench::record_metric("scheduler",
                             skewed ? "steal_speedup_skewed" : "steal_speedup_uniform",
                             ms_level / ms_steal);
    }

    // One traced skewed run through each scheduler: busy/span/workers is the
    // pool efficiency; the measured critical path should track the model.
    std::printf("\n-- traced skewed run --\n");
    const auto d = durations(true);
    for (int which = 0; which < 2; ++which) {
        ChainGraph cg(kChains, kLength, d);
        runtime::TraceRecorder trace;
        if (which == 0)
            level.submit(cg.graph, &trace);
        else
            stealing.submit(cg.graph, &trace);
        const auto cp = trace.critical_path(cg.graph);
        std::printf(
            "  %-14s span %7.2f ms  busy %7.2f ms  efficiency %4.1f%%  "
            "critical path %.2f ms (%zu nodes)\n",
            which == 0 ? "level-barrier" : "work-stealing", trace.span_ns() / 1e6,
            trace.busy_ns() / 1e6,
            100.0 * trace.busy_ns() /
                (double(trace.span_ns()) * double(kWorkers)),
            cp.total_ns / 1e6, cp.nodes.size());
    }
    return 0;
}
