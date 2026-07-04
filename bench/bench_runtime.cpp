// Runtime-overhead benchmarks (R2). Answers the three gated hypotheses from
// BENCHMARKS.md: (1) registry std::function dispatch cost vs a direct call,
// (2) one-node TaskGraph/scheduler overhead vs a bare loop as a function of
// batch size (where do futures stop dominating?), (3) transform-pair fusion
// p50 before/after on the analytics graph.
#include <cstdio>
#include <random>
#include <vector>

#include "../examples/pipelines.hpp"
#include "../src/kernels/core.hpp"
#include "../src/kernels/registry.hpp"
#include "../src/runtime/optimizer.hpp"
#include "../src/runtime/scheduler.hpp"
#include "harness.hpp"

using namespace aegis;

static std::vector<double> ramp(size_t n) {
    std::vector<double> v(n);
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> d(0.5, 1.5);
    for (auto& x : v) x = d(rng);
    return v;
}

int main() {
    bench::Harness harness;

    // (1) Registry dispatch vs direct call, tiny workload so the per-call
    // std::function indirection is visible rather than buried in kernel work.
    std::puts("aegis runtime overhead\n");
    std::puts("-- registry dispatch vs direct call (ema, 256-elem col) --");
    const auto in = ramp(256);
    std::vector<double> out(256);
    const auto entry = kernels::find("ema").value();
    kernels::RegistryEntry::Call call{{mat_view(in.data(), 256, 1)},
                                      mat_view(out.data(), 256, 1), 0, 0.1};
    auto rd = harness.run("direct", 1, [&] {
        kernels::best::ema(col_view(in.data(), 256), 0.1, col_view(out.data(), 256));
        return out[255];
    });
    auto rr = harness.run("registry", 1, [&] { entry.best_fn(call); return out[255]; });
    std::printf("  direct   %7.1f ns/call\n  registry %7.1f ns/call\n"
                "  dispatch overhead ~%.1f ns (%.1f%%)\n",
                rd.p50_ns, rr.p50_ns, rr.p50_ns - rd.p50_ns,
                100.0 * (rr.p50_ns - rd.p50_ns) / rd.p50_ns);

    // (2) One-node graph vs direct loop across batch sizes: futures dominate
    // tiny work and amortize as the batch grows.
    std::puts("\n-- one-node graph overhead vs direct call, by batch size --");
    runtime::Scheduler scheduler(2);
    std::printf("  %-9s %12s %12s %10s\n", "batch", "direct ns", "graph ns", "overhead");
    for (size_t n : {size_t{64}, size_t{1024}, size_t{16384}, size_t{262144}}) {
        const auto data = ramp(n);
        std::vector<double> dst(n);
        auto direct = harness.run("d", n, [&] {
            kernels::best::ema(col_view(data.data(), n), 0.1, col_view(dst.data(), n));
            return dst[n - 1];
        });
        runtime::TaskGraph g;
        g.add_node("ema", kernels::KernelClass::TRANSFORM, [&] {
            kernels::best::ema(col_view(data.data(), n), 0.1, col_view(dst.data(), n));
        }, true);
        auto graph = harness.run("g", n, [&] { scheduler.submit(g); return dst[n - 1]; });
        std::printf("  %-9zu %12.0f %12.0f %9.0f ns\n", n, direct.p50_ns,
                    graph.p50_ns, graph.p50_ns - direct.p50_ns);
    }

    // (3) Fusion before/after on the analytics graph (ema -> zscore -> signal).
    std::puts("\n-- transform-pair fusion, analytics graph p50 --");
    const size_t N = 100000;
    const auto price = ramp(N);
    auto build = [&] {
        return examples::AnalyticsPipeline(col_view(price.data(), N));
    };
    auto unfused = build();
    auto before = harness.run("before", N, [&] { scheduler.submit(unfused.graph); return unfused.signal[N - 1]; });
    auto fused = build();
    auto report = runtime::optimize(fused.graph);
    auto after = harness.run("after", N, [&] { scheduler.submit(fused.graph); return fused.signal[N - 1]; });
    std::printf("  before %8.0f ns   after %8.0f ns   fused %zu pair(s), removed %zu dead\n",
                before.p50_ns, after.p50_ns, report.transform_pairs_fused,
                report.dead_nodes_removed);
    return 0;
}
