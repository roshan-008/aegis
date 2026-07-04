// Memory-subsystem benchmarks (R1). Arena bump-allocate+reset vs malloc/free;
// FixedPool create/destroy vs new/delete. These are the numbers behind the
// "eliminate malloc from hot paths" cost card.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../src/mem/arena.hpp"
#include "../src/mem/fixed_pool.hpp"
#include "harness.hpp"

using namespace aegis;

struct Msg { double a, b, c; };

int main() {
    bench::Harness harness;
    constexpr size_t K = 4096;  // allocations per batch

    std::puts("aegis memory subsystem\n");

    // Arena: K bump allocations then one reset, vs K malloc + K free.
    mem::Arena arena(K * 64 + 4096);
    auto ra = harness.run("arena", K, [&] {
        double s = 0;
        for (size_t i = 0; i < K; ++i) {
            auto* p = arena.allocate_n<double>(4);
            p[0] = double(i); s += p[0];
        }
        arena.reset();
        return s;
    });
    auto rm = harness.run("malloc", K, [&] {
        double s = 0;
        std::vector<void*> ptrs(K);
        for (size_t i = 0; i < K; ++i) {
            ptrs[i] = std::malloc(4 * sizeof(double));
            static_cast<double*>(ptrs[i])[0] = double(i);
            s += static_cast<double*>(ptrs[i])[0];
        }
        for (void* p : ptrs) std::free(p);
        return s;
    });
    std::printf("-- allocate 4 doubles x%zu --\n", K);
    std::printf("  arena   %8.1f Malloc/s   %6.2f ns/alloc\n",
                ra.throughput / 1e6, ra.p50_ns / K);
    std::printf("  malloc  %8.1f Malloc/s   %6.2f ns/alloc   (%.1fx slower)\n",
                rm.throughput / 1e6, rm.p50_ns / K, rm.p50_ns / ra.p50_ns);

    // Pool: K create/destroy cycles reusing slots, vs new/delete.
    mem::FixedPool<Msg, K> pool;
    auto rp = harness.run("pool", K, [&] {
        double s = 0;
        std::vector<Msg*> live(K);
        for (size_t i = 0; i < K; ++i) { live[i] = pool.create(Msg{double(i), 0, 0}); s += live[i]->a; }
        for (Msg* p : live) pool.destroy(p);
        return s;
    });
    auto rn = harness.run("new", K, [&] {
        double s = 0;
        std::vector<Msg*> live(K);
        for (size_t i = 0; i < K; ++i) { live[i] = new Msg{double(i), 0, 0}; s += live[i]->a; }
        for (Msg* p : live) delete p;
        return s;
    });
    std::printf("\n-- create/destroy Msg x%zu --\n", K);
    std::printf("  pool    %8.1f Mop/s   %6.2f ns/op\n", rp.throughput / 1e6, rp.p50_ns / K);
    std::printf("  new/del %8.1f Mop/s   %6.2f ns/op   (%.1fx slower)\n",
                rn.throughput / 1e6, rn.p50_ns / K, rn.p50_ns / rp.p50_ns);
    return 0;
}
