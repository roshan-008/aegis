#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include "../src/mem/alloc_counter.hpp"

namespace aegis::bench {

struct Result {
    std::string name;
    double throughput = 0.0;
    double p50_ns = 0.0;
    double p99_ns = 0.0;
    uint64_t hot_allocations = 0;
    double checksum = 0.0;
};

class Harness {
public:
    Harness(size_t warmup = 2, size_t reps = 9)
        : warmup_(warmup), reps_(reps) {}

    template <typename Fn>
    Result run(std::string name, size_t work_items, Fn&& fn) const {
        for (size_t i = 0; i < warmup_; ++i) checksum_ += fn();
        std::vector<double> samples;
        samples.reserve(reps_);
        mem::AllocationCounter::reset_hot_path();
        for (size_t i = 0; i < reps_; ++i) {
            const auto begin = std::chrono::steady_clock::now();
            {
                mem::HotPathAllocationScope hot;
                checksum_ += fn();
            }
            const auto end = std::chrono::steady_clock::now();
            samples.push_back(std::chrono::duration<double, std::nano>(end - begin).count());
        }
        std::sort(samples.begin(), samples.end());
        const double med = percentile(samples, .50);
        return {std::move(name), work_items / (med * 1e-9), med,
                percentile(samples, .99), mem::AllocationCounter::hot_path(), checksum_};
    }

    static bool regression_ok(double measured, double ledger,
                              double max_regression = .10) {
        return ledger <= 0.0 || measured >= ledger * (1.0 - max_regression);
    }
    static void assert_zero_hot_allocations(const Result& result) {
        if (result.hot_allocations != 0)
            throw std::runtime_error("hot-path allocation regression in " + result.name);
    }

private:
    static double percentile(const std::vector<double>& v, double p) {
        return v[static_cast<size_t>(std::floor(p * (v.size() - 1)))];
    }
    size_t warmup_;
    size_t reps_;
    inline static volatile double checksum_ = 0.0;
};

// Machine-readable headline numbers for the regression gate. When
// AEGIS_BENCH_JSON names a file, each call appends one JSONL record; several
// bench binaries can share one file. scripts/check_bench.py diffs a run
// against results/baseline-*.jsonl. Convention: metrics ending in "_ns" are
// lower-is-better, everything else higher-is-better.
inline void record_metric(const char* suite, const char* metric, double value) {
    const char* path = std::getenv("AEGIS_BENCH_JSON");
    if (!path || !*path) return;
    FILE* f = std::fopen(path, "ab");
    if (!f) return;
    std::fprintf(f, "{\"suite\":\"%s\",\"metric\":\"%s\",\"value\":%.6g}\n",
                 suite, metric, value);
    std::fclose(f);
}

}  // namespace aegis::bench
