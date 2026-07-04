// Tensor-kernel benchmarks (R1 MATRIX/NORMALIZATION/TRANSFORM classes).
// matmul: naive ijk vs blocked+FMA. softmax/layernorm/gelu: throughput.
// Uses bench::Harness so warmup/reps/p50/p99/anti-DCE are shared with the
// rest of the suite (this also exercises the harness cost card).
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "../src/kernels/core.hpp"
#include "harness.hpp"

using namespace aegis;

static std::vector<double> random_matrix(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    std::vector<double> m(n);
    for (auto& x : m) x = dist(rng);
    return m;
}

int main() {
    bench::Harness harness;

    std::puts("aegis tensor kernels\n");
    std::puts("-- matmul: naive ijk vs blocked+FMA (GFLOP/s) --");
    for (size_t d : {128u, 256u, 512u}) {
        const auto A = random_matrix(d * d, 1), B = random_matrix(d * d, 2);
        std::vector<double> C(d * d);
        const auto a = mat_view(A.data(), d, d), b = mat_view(B.data(), d, d);
        const auto c = mat_view(C.data(), d, d);
        const double flops = 2.0 * d * d * d;  // one madd = 2 FLOPs
        auto rn = harness.run("naive", static_cast<size_t>(flops), [&] {
            kernels::naive::matmul(a, b, c); return C[0];
        });
        auto rb = harness.run("blocked", static_cast<size_t>(flops), [&] {
            kernels::best::matmul(a, b, c); return C[0];
        });
        std::printf("  %4zux%-4zu  naive %6.2f GF/s   blocked %6.2f GF/s   %4.2fx\n",
                    d, d, rn.throughput / 1e9, rb.throughput / 1e9,
                    rb.throughput / rn.throughput);
    }

    std::puts("\n-- row-wise kernels over a 4096x256 matrix (Melem/s) --");
    const size_t R = 4096, Cc = 256;
    const auto X = random_matrix(R * Cc, 3);
    std::vector<double> Y(R * Cc);
    const auto x = mat_view(X.data(), R, Cc);
    const auto y = mat_view(Y.data(), R, Cc);
    const size_t elems = R * Cc;
    struct RowKernel { const char* name; void (*fn)(ConstMatView, MatView); };
    const RowKernel row_kernels[] = {
        {"softmax", kernels::best::softmax},
        {"gelu", kernels::best::gelu},
        {"layernorm", [](ConstMatView a, MatView o) { kernels::best::layernorm(a, o); }},
    };
    for (const RowKernel& k : row_kernels) {
        auto r = harness.run(k.name, elems, [&] { k.fn(x, y); return Y[0]; });
        std::printf("  %-10s %7.1f Melem/s   p50 %8.0f ns\n", k.name,
                    r.throughput / 1e6, r.p50_ns);
    }
    return 0;
}
