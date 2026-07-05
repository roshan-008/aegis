// Financial pipeline benchmark. This is the quant-facing sibling of the AI
// operator demos: replay a sealed market-data segment at maximum speed, execute
// a DAG of feature kernels, then report throughput and tail batch latency.
//
// Pipeline:
//   MarketTick replay -> Decode/Normalize -> VWAP, rolling mean/std, EMA
//   -> Signal generation -> Risk checks -> Output checksum
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "../src/kernels/core.hpp"
#include "../src/mem/arena.hpp"
#include "../src/runtime/scheduler.hpp"
#include "../src/storage/cursor.hpp"
#include "harness.hpp"

using namespace aegis;
using Clock = std::chrono::steady_clock;

struct RunResult {
    size_t rows = 0;
    double wall_ns = 0.0;
    double p50_ns = 0.0;
    double p99_ns = 0.0;
    double p999_ns = 0.0;
    double checksum = 0.0;
};

static double percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    return sorted[static_cast<size_t>(p * static_cast<double>(sorted.size() - 1))];
}

static TickTable make_feed(size_t n) {
    TickTable table;
    table.reserve(n);
    std::mt19937_64 rng(42);
    std::normal_distribution<double> ret(0.0, 8e-5);
    std::uniform_real_distribution<double> vol_jitter(0.0, 8.0);
    double px = 100.0;
    for (size_t i = 0; i < n; ++i) {
        px *= 1.0 + ret(rng);
        const double auction_wave = static_cast<double>((i / 2000) % 17);
        table.price.push(px);
        table.volume.push(25.0 + auction_wave + vol_jitter(rng));
        table.ts_ns.push(static_cast<double>(i) * 1000.0);
    }
    return table;
}

static void decode_normalize(ConstColView price, ConstColView volume, size_t n,
                             double* px, double* vol, double* returns) {
    for (size_t i = 0; i < n; ++i) {
        px[i] = price(i);
        vol[i] = volume(i);
        returns[i] = i == 0 ? 0.0 : std::log(px[i] / px[i - 1]);
    }
}

static void rolling_mean_into(const double* x, size_t n, size_t w, double* out) {
    const double nan = std::nan("");
    std::fill(out, out + n, nan);
    if (w == 0 || n < w) return;
    const double inv = 1.0 / static_cast<double>(w);
    double sum = 0.0;
    for (size_t i = 0; i < w; ++i) sum += x[i];
    out[w - 1] = sum * inv;
    for (size_t i = w; i < n; ++i) {
        sum += x[i] - x[i - w];
        out[i] = sum * inv;
    }
}

static void rolling_vwap_into(const double* price, const double* volume, size_t n,
                              size_t w, double* out) {
    const double nan = std::nan("");
    std::fill(out, out + n, nan);
    if (w == 0 || n < w) return;
    double pv = 0.0;
    double v = 0.0;
    for (size_t i = 0; i < w; ++i) {
        pv += price[i] * volume[i];
        v += volume[i];
    }
    out[w - 1] = v > 0.0 ? pv / v : nan;
    for (size_t i = w; i < n; ++i) {
        pv += price[i] * volume[i] - price[i - w] * volume[i - w];
        v += volume[i] - volume[i - w];
        out[i] = v > 0.0 ? pv / v : nan;
    }
}

static void rolling_std_into(const double* x, size_t n, size_t w, double* out) {
    const double nan = std::nan("");
    std::fill(out, out + n, nan);
    if (w == 0 || n < w) return;
    if (w == 1) {
        std::fill(out, out + n, 0.0);
        return;
    }
    const double inv = 1.0 / static_cast<double>(w);
    double k = x[0];
    double sum = 0.0;
    double sumsq = 0.0;
    for (size_t i = 0; i < w; ++i) {
        const double d = x[i] - k;
        sum += d;
        sumsq += d * d;
    }
    auto emit = [&] {
        const double mean = sum * inv;
        const double var = sumsq * inv - mean * mean;
        return var > 0.0 ? std::sqrt(var) : 0.0;
    };
    out[w - 1] = emit();
    for (size_t i = w; i < n; ++i) {
        const double in = x[i] - k;
        const double old = x[i - w] - k;
        sum += in - old;
        sumsq += in * in - old * old;
        out[i] = emit();
    }
}

static void generate_signal(const double* price, const double* returns,
                            const double* mean, const double* stdev,
                            const double* vwap, const double* ema, size_t n,
                            size_t warmup, double* signal) {
    for (size_t i = 0; i < n; ++i) {
        double desired = 0.0;
        if (i + 1 >= warmup && stdev[i] > 1e-12 && std::isfinite(vwap[i])) {
            const double fair = 0.5 * (mean[i] + vwap[i]);
            const double z = (price[i] - fair) / stdev[i];
            const double trend = ema[i] >= mean[i] ? 1.0 : -1.0;
            desired = std::clamp(-0.35 * z + 2000.0 * returns[i] + 0.05 * trend,
                                 -1.0, 1.0);
        }
        signal[i] = desired;
    }
}

static double risk_checks(const double* signal, size_t n, double* exposure) {
    double position = 0.0;
    double checksum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        position = std::clamp(0.98 * position + 0.02 * signal[i], -1.0, 1.0);
        exposure[i] = position;
        checksum += exposure[i] * static_cast<double>((i % 17) + 1);
    }
    return checksum;
}

static RunResult run_pipeline(const storage::SegmentStore& store, size_t batch_size,
                              size_t window, size_t workers) {
    runtime::Scheduler scheduler(workers);
    mem::Arena arena(1 << 20);
    std::vector<double> px(batch_size), vol(batch_size), returns(batch_size);
    std::vector<double> rmean(batch_size), rstd(batch_size), rvwap(batch_size);
    std::vector<double> ema(batch_size), signal(batch_size), exposure(batch_size);

    storage::ReplayBatch current{};
    size_t current_n = 0;
    double batch_checksum = 0.0;

    runtime::TaskGraph graph;
    const auto decode = graph.add_node("MarketTick.decode_normalize",
                                       kernels::KernelClass::TRANSFORM, [&] {
        decode_normalize(current.price, current.volume, current_n, px.data(),
                         vol.data(), returns.data());
    });
    const auto mean = graph.add_node("rolling_mean",
                                     kernels::KernelClass::WINDOW, [&] {
        rolling_mean_into(px.data(), current_n, window, rmean.data());
    });
    const auto stdev = graph.add_node("rolling_std",
                                      kernels::KernelClass::WINDOW, [&] {
        rolling_std_into(px.data(), current_n, window, rstd.data());
    });
    const auto vwap = graph.add_node("rolling_vwap",
                                     kernels::KernelClass::WINDOW, [&] {
        rolling_vwap_into(px.data(), vol.data(), current_n, window, rvwap.data());
    });
    const auto ema_node = graph.add_node("ema",
                                         kernels::KernelClass::TRANSFORM, [&] {
        kernels::best::ema(col_view(static_cast<const double*>(px.data()), current_n), 0.08,
                           col_view(ema.data(), current_n));
    });
    const auto signal_node = graph.add_node("signal_generation",
                                            kernels::KernelClass::TRANSFORM, [&] {
        generate_signal(px.data(), returns.data(), rmean.data(), rstd.data(),
                        rvwap.data(), ema.data(), current_n, window,
                        signal.data());
    });
    const auto risk_node = graph.add_node("risk_checks",
                                          kernels::KernelClass::REDUCTION, [&] {
        batch_checksum = risk_checks(signal.data(), current_n, exposure.data());
    }, true);
    graph.add_edge(decode, mean);
    graph.add_edge(decode, stdev);
    graph.add_edge(decode, vwap);
    graph.add_edge(decode, ema_node);
    graph.add_edge(mean, signal_node);
    graph.add_edge(stdev, signal_node);
    graph.add_edge(vwap, signal_node);
    graph.add_edge(ema_node, signal_node);
    graph.add_edge(signal_node, risk_node);

    auto cursor = store.open({}, 0.0, batch_size);
    std::vector<double> batch_samples;
    batch_samples.reserve(1024);
    RunResult result;
    const auto wall_begin = Clock::now();
    while (auto batch = cursor.next(arena)) {
        current = *batch;
        current_n = batch->size();
        const auto t0 = Clock::now();
        scheduler.submit(graph);
        const auto t1 = Clock::now();
        batch_samples.push_back(
            std::chrono::duration<double, std::nano>(t1 - t0).count());
        result.rows += current_n;
        result.checksum += batch_checksum;
        arena.reset();
    }
    result.wall_ns = std::chrono::duration<double, std::nano>(
                         Clock::now() - wall_begin)
                         .count();
    std::sort(batch_samples.begin(), batch_samples.end());
    result.p50_ns = percentile(batch_samples, 0.50);
    result.p99_ns = percentile(batch_samples, 0.99);
    result.p999_ns = percentile(batch_samples, 0.999);
    return result;
}

int main(int argc, char** argv) {
    const size_t n = argc > 1 ? std::stoull(argv[1]) : 1'000'000;
    const size_t batch_size = argc > 2 ? std::stoull(argv[2]) : 65536;
    const size_t window = argc > 3 ? std::stoull(argv[3]) : 128;
    const size_t workers = argc > 4 ? std::stoull(argv[4]) : 4;
    if (n == 0 || batch_size == 0 || window == 0 || workers == 0 ||
        batch_size < window) {
        std::fprintf(stderr,
                     "usage: bench_financial_pipeline [rows] [batch>=window] "
                     "[window] [workers]\n");
        return 2;
    }

    const auto dir = std::filesystem::temp_directory_path() /
                     ("aegis-financial-pipeline-" + std::to_string(n) + "-" +
                      std::to_string(batch_size));
    std::filesystem::remove_all(dir);
    storage::SegmentStore store(dir.string());
    const auto table = make_feed(n);
    store.append(table, "market-session.aegis");

    const auto result = run_pipeline(store, batch_size, window, workers);
    const double rows_per_sec = result.rows / (result.wall_ns * 1e-9);
    std::puts("aegis financial pipeline replay\n");
    std::printf("  rows=%zu  batch=%zu  window=%zu  workers=%zu\n", n, batch_size,
                window, workers);
    std::puts("  graph: MarketTick -> Decode/Normalize -> "
              "{VWAP, RollingMean, RollingStd, EMA} -> Signal -> Risk");
    std::printf("\n  throughput       %8.1f M ticks/sec\n", rows_per_sec / 1e6);
    std::printf("  batch p50        %8.1f us\n", result.p50_ns / 1000.0);
    std::printf("  batch p99        %8.1f us\n", result.p99_ns / 1000.0);
    std::printf("  batch p99.9      %8.1f us\n", result.p999_ns / 1000.0);
    std::printf("  risk checksum    %.6f\n", result.checksum);

    bench::record_metric("financial_pipeline", "throughput_rows_per_sec",
                         rows_per_sec);
    bench::record_metric("financial_pipeline", "batch_p99_ns", result.p99_ns);

    std::filesystem::remove_all(dir);
    return 0;
}
