// Storage-replay benchmark (R3). Three numbers that separate concerns instead
// of conflating them:
//   (1) in-memory sum          — the memory-bandwidth ceiling.
//   (2) warm pre-opened scan    — segment already open+validated; pure cost of
//                                 batching mmap views through a kernel.
//   (3) open + validate + drain — realistic per-query cost, INCLUDING Rust's
//                                 whole-segment CRC check on open.
// Reporting (2) and (3) apart is the honest version: it shows replay-the-scan
// stays near memory speed, while open-time validation is a separate fixed cost.
#include <cstdio>
#include <filesystem>
#include <vector>

#include "../src/kernels/core.hpp"
#include "../src/mem/arena.hpp"
#include "../src/storage/cursor.hpp"
#include "../src/storage/reader.hpp"
#include "harness.hpp"

using namespace aegis;

int main() {
    bench::Harness harness;
    const size_t N = 4'000'000;
    const size_t BATCH = 65536;

    TickTable table;
    table.price.data.reserve(N);
    for (size_t i = 0; i < N; ++i) {
        table.price.push(100.0 + (i % 997) * 0.01);
        table.volume.push(1.0 + (i % 13));
        table.ts_ns.push(static_cast<double>(i));
    }

    const auto dir = std::filesystem::temp_directory_path() / "aegis-bench-storage";
    std::filesystem::remove_all(dir);
    storage::SegmentStore store(dir.string());
    const std::string path = store.append(table, "bench.aegis");

    mem::Arena arena(1 << 20);

    // (1) In-memory baseline.
    auto in_mem = harness.run("in-memory", N, [&] {
        return kernels::best::sum(col_view(static_cast<const double*>(table.price.data.data()), N));
    });

    // (2) Warm scan: open+validate ONCE, then batch the mmap views repeatedly.
    storage::SegmentReader reader(path);
    auto warm = harness.run("warm-scan", N, [&] {
        const auto price = reader.price();
        double s = 0.0;
        for (size_t off = 0; off < N; off += BATCH)
            s += kernels::best::sum(price.subrows(off, std::min(BATCH, N - off)));
        return s;
    });

    // (3) End-to-end: reopen the store (mmap + full-segment CRC) and drain.
    auto e2e = harness.run("open+drain", N, [&] {
        auto cursor = store.open({}, 0.0, BATCH);
        double s = 0.0;
        while (auto batch = cursor.next(arena)) { s += kernels::best::sum(batch->price); arena.reset(); }
        return s;
    });

    std::puts("aegis storage replay (warm page cache)\n");
    std::printf("  (1) in-memory sum        %8.1f M rows/sec\n", in_mem.throughput / 1e6);
    std::printf("  (2) warm pre-opened scan %8.1f M rows/sec   (%.1f%% of in-memory)\n",
                warm.throughput / 1e6, 100.0 * warm.throughput / in_mem.throughput);
    std::printf("  (3) open+validate+drain  %8.1f M rows/sec   (%.2f ms/query, CRC-bound)\n",
                e2e.throughput / 1e6, e2e.p50_ns / 1e6);
    std::printf("\n  open-time CRC over the whole %zu MB segment is the fixed cost\n"
                "  in (3); the scan itself (2) stays near memory bandwidth.\n",
                (N * 3 * sizeof(double)) >> 20);

    std::filesystem::remove_all(dir);
    return 0;
}
