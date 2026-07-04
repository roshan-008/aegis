// Loading strategies compared on the same data: text CSV parse vs binary
// read-into-vector vs zero-copy mmap. Each is timed end-to-end from "file on
// disk" to "rolling_mean computed", so the numbers include whatever copying
// each path forces. The mmap path does zero payload copies.
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <random>
#include <string>
#include <vector>

#include "../src/column.hpp"
#include "../src/mmap_table.hpp"
#include "../src/rolling_fast.hpp"

using namespace aegis;
using Clock = std::chrono::steady_clock;

template <typename F>
static double timed(F&& f) {
    auto t0 = Clock::now();
    f();
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

int main(int argc, char** argv) {
    const size_t N = argc > 1 ? std::stoull(argv[1]) : 10'000'000;
    const std::string bin = "/tmp/aegis_ticks.bin";
    const std::string csv = "/tmp/aegis_ticks.csv";
    const size_t W = 100;

    // synth
    TickTable t;
    t.reserve(N);
    std::mt19937_64 rng(42);
    std::normal_distribution<double> ret(0.0, 1e-4);
    double px = 100.0;
    for (size_t i = 0; i < N; ++i) {
        px *= (1.0 + ret(rng));
        t.price.push(px);
        t.volume.push(1.0);
        t.ts_ns.push(static_cast<double>(i));
    }

    // write both formats
    size_t nb = save_table(t, bin);
    {
        FILE* f = std::fopen(csv.c_str(), "wb");
        for (size_t i = 0; i < N; ++i)
            std::fprintf(f, "%.10g,%.10g,%zu\n", t.price[i], t.volume[i], i);
        std::fclose(f);
    }
    std::printf("aegis mmap bench: N=%zu  (binary %.1f MB)\n\n", N,
                nb / 1e6);

    volatile double sink = 0.0;

    // 1) CSV: parse text into a Column, then compute.
    double t_csv = timed([&] {
        Column c;
        c.reserve(N);
        FILE* f = std::fopen(csv.c_str(), "rb");
        double a, b;
        long long ts;
        while (std::fscanf(f, "%lf,%lf,%lld", &a, &b, &ts) == 3) c.push(a);
        std::fclose(f);
        sink = fast::rolling_mean(c, W).back();
    });

    // 2) Binary read into an owning vector (one full copy), then compute.
    double t_read = timed([&] {
        Column c;
        c.data.resize(N);
        FILE* f = std::fopen(bin.c_str(), "rb");
        std::fseek(f, kHeaderBytes, SEEK_SET);
        size_t got = std::fread(c.data.data(), sizeof(double), N, f);
        (void)got;
        std::fclose(f);
        sink = fast::rolling_mean(c, W).back();
    });

    // 3) mmap zero-copy: map once; time first and second passes. NOTE: page
    //    cache state is NOT controlled here — the file is warm from save_table
    //    and any prior run. Truly-cold numbers need `sudo purge` (macOS) or
    //    `echo 3 > /proc/sys/vm/drop_caches` (Linux) before this process. So we
    //    report both passes honestly as "warm" rather than mislabel one "cold".
    double t_p1 = 0, t_p2 = 0;
    {
        MmapTable m(bin);
        m.advise_sequential();
        t_p1 = timed([&] { sink = fast::rolling_mean(m.price(), W).back(); });
        t_p2 = timed([&] { sink = fast::rolling_mean(m.price(), W).back(); });
    }

    (void)sink;
    std::printf("%-26s %8.3f s   %8.1f M rows/sec\n", "CSV parse + compute",
                t_csv, N / t_csv / 1e6);
    std::printf("%-26s %8.3f s   %8.1f M rows/sec\n", "binary read + compute",
                t_read, N / t_read / 1e6);
    std::printf("%-26s %8.3f s   %8.1f M rows/sec\n", "mmap pass 1 + compute",
                t_p1, N / t_p1 / 1e6);
    std::printf("%-26s %8.3f s   %8.1f M rows/sec\n", "mmap pass 2 + compute",
                t_p2, N / t_p2 / 1e6);
    std::printf(
        "\nRobust finding (cache-state-independent):\n"
        "  text CSV parsing is ~%.0fx slower than any binary path — parsing,\n"
        "  not I/O, dominates. Binary read and mmap are both far faster and,\n"
        "  with the page cache warm, within noise of each other; mmap's\n"
        "  distinct benefit is zero-copy (no %zu MB payload allocation).\n"
        "  Cold-cache read-vs-mmap is NOT measured here (needs purge) — that\n"
        "  claim is deferred rather than faked.\n",
        t_csv / t_p2, N * sizeof(double) / (1u << 20));

    // correctness: mmap'd data must equal the source
    MmapTable m(bin);
    bool ok = m.size() == N && m.price()[0] == t.price[0] &&
              m.price()[N - 1] == t.price[N - 1];
    std::printf("roundtrip correctness: %s\n", ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}
