// analyze — full quant analytics over one instrument's rows, computed on the
// Aegis engine. Reads "close,volume" CSV rows on stdin, runs the SIMD rolling
// kernels and a single-pass fan of bulk reductions, times the hot compute, and
// emits a JSON report.
//
//   ./analyze AAPL < aapl.csv
//
// Latency note: for a single instrument (hundreds–thousands of rows) the whole
// analytics pass is a few microseconds. Spawning a thread per reduction costs
// *more* than the work itself (thread launch ~tens of us), so the low-latency
// path is a tight serial sweep. Parallel fan-out (aegis::par / the runtime
// scheduler) only wins once n is large enough to amortize a launch — we cross
// to it above PAR_THRESHOLD. Every number the UI shows comes from here.

#include "../src/column.hpp"
#include "../src/rolling_fast.hpp"
#include "../src/parallel.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using aegis::Column;

static std::string jnum(double v) {
    if (std::isnan(v) || std::isinf(v)) return "null";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}
static std::string jarr(const double* v, size_t n, size_t stride) {
    std::string s = "[";
    bool first = true;
    for (size_t i = 0; i < n; i += stride) {
        if (!first) s += ',';
        s += jnum(v[i]);
        first = false;
    }
    return s + "]";
}

// Everything the analytics pass produces. Filled by run_analytics().
struct Analytics {
    double first, last, pmin, pmax;
    double total_ret, ann_ret, ann_vol, sharpe, mdd;
    double mean_daily, win_rate, best_day, worst_day;
    // volume / bulk-dealing desk metrics
    double avg_vol, med_vol, total_vol, block_thresh;
    long   block_days;
    double dollar_vol, up_vol_frac, obv_last;
    double latest_vwap, latest_mean;
    std::vector<double> rmean, rstd, rvwap;
};

// One serial sweep. This is the hot path we time. rolling_* are the O(n)
// sliding-window kernels; the loop below is the single-pass reduction fan
// (min/max, log-return moments, drawdown, and the volume/accumulation stats).
static Analytics run_analytics(const Column& price, const Column& volume,
                               size_t W, bool parallel, unsigned nthreads) {
    const size_t n = price.size();
    Analytics A{};

    if (parallel) {
        // Large n: fan the rolling kernels across threads, worth the launch.
        A.rmean.assign(n, std::nan(""));
        auto f_mean = std::async(std::launch::async, [&] {
            aegis::par::rolling_mean_into(price, W, nthreads, A.rmean); });
        auto f_std  = std::async(std::launch::async, [&] { return aegis::fast::rolling_std(price, W); });
        A.rvwap = aegis::fast::rolling_vwap(price, volume, W);
        f_mean.get();
        A.rstd = f_std.get();
    } else {
        A.rmean = aegis::fast::rolling_mean(price, W);
        A.rstd  = aegis::fast::rolling_std(price, W);
        A.rvwap = aegis::fast::rolling_vwap(price, volume, W);
    }

    // --- price extremes + return moments in one sweep ---
    const double* p = price.raw();
    const double* v = volume.raw();
    double mn = p[0], mx = p[0];
    double rsum = 0.0, rss = 0.0, best = -1e300, worst = 1e300;
    long up = 0;
    double peak = p[0], mdd = 0.0;
    double tv = 0.0, dv = 0.0, upv = 0.0, downv = 0.0, obv = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double px = p[i], vol = v[i];
        mn = std::min(mn, px); mx = std::max(mx, px);
        peak = std::max(peak, px);
        mdd = std::min(mdd, px / peak - 1.0);
        tv += vol;
        dv += px * vol;
        if (i > 0) {
            double r = std::log(px / p[i - 1]);
            rsum += r; rss += r * r;
            best = std::max(best, r); worst = std::min(worst, r);
            if (r > 0) { ++up; upv += vol; obv += vol; }
            else if (r < 0) { downv += vol; obv -= vol; }
        }
    }
    const size_t rn = n - 1;
    double rmu = rsum / rn;
    double rvar = rss / rn - rmu * rmu;
    double rsd = rvar > 0 ? std::sqrt(rvar) : 0.0;

    A.first = p[0]; A.last = p[n - 1]; A.pmin = mn; A.pmax = mx;
    A.total_ret = (p[n - 1] / p[0] - 1.0) * 100.0;
    A.ann_ret = rmu * 252.0 * 100.0;
    A.ann_vol = rsd * std::sqrt(252.0) * 100.0;
    A.sharpe  = rsd > 0 ? (rmu * 252.0) / (rsd * std::sqrt(252.0)) : 0.0;
    A.mdd = mdd * 100.0;
    A.mean_daily = rmu * 100.0;
    A.win_rate = (double)up / rn;
    A.best_day = best * 100.0;
    A.worst_day = worst * 100.0;

    // --- bulk / block-trade view: median + 90th-pct volume threshold ---
    // Order statistics need selection, not a full sort. nth_element is O(n)
    // average vs O(n log n) — the same move a latency-sensitive desk makes when
    // it only wants a quantile, not the whole ranking.
    std::vector<double> vs(v, v + n);
    const size_t kmed = n / 2, k90 = (size_t)(0.90 * (n - 1));
    std::nth_element(vs.begin(), vs.begin() + kmed, vs.end());
    A.med_vol = vs[kmed];
    std::nth_element(vs.begin(), vs.begin() + k90, vs.end());
    double q90 = vs[k90];
    A.block_thresh = q90;
    long blocks = 0;
    for (size_t i = 0; i < n; ++i) if (v[i] >= q90) ++blocks;
    A.block_days = blocks;
    A.avg_vol = tv / n;
    A.total_vol = tv;
    A.dollar_vol = dv;
    A.up_vol_frac = (upv + downv) > 0 ? upv / (upv + downv) : 0.0;
    A.obv_last = obv;
    A.latest_vwap = A.rvwap[n - 1];
    A.latest_mean = A.rmean[n - 1];
    return A;
}

int main(int argc, char** argv) {
    std::string ticker = argc > 1 ? argv[1] : "TICKER";
    Column price, volume;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string a, b;
        std::getline(ss, a, ',');
        std::getline(ss, b, ',');
        try {
            double c = std::stod(a);
            double vv = b.empty() ? 0.0 : std::stod(b);
            price.push(c);
            volume.push(vv);
        } catch (...) { continue; }  // skip header / bad rows
    }
    const size_t n = price.size();
    if (n < 8) { std::cerr << "need >=8 rows\n"; return 1; }

    constexpr size_t PAR_THRESHOLD = 200000;  // below this, serial wins
    const bool parallel = n >= PAR_THRESHOLD;
    const unsigned nthreads = std::max(2u, std::thread::hardware_concurrency());
    const size_t W = std::min<size_t>(20, n / 2);

    // Best-of-K on the hot compute: the min excludes scheduler/cache noise and
    // is the honest figure for "how fast can the engine turn this around".
    const int K = parallel ? 3 : 50;
    Analytics A;
    double best_ns = 1e300;
    for (int k = 0; k < K; ++k) {
        auto t0 = std::chrono::steady_clock::now();
        A = run_analytics(price, volume, W, parallel, nthreads);
        auto t1 = std::chrono::steady_clock::now();
        double ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        best_ns = std::min(best_ns, ns);
    }

    std::string out = "{";
    out += "\"ticker\":\"" + ticker + "\",";
    out += "\"rows\":" + std::to_string(n) + ",";
    out += "\"window\":" + std::to_string(W) + ",";
    out += "\"threads\":" + std::to_string(parallel ? nthreads : 1u) + ",";
    out += "\"hw_threads\":" + std::to_string(std::thread::hardware_concurrency()) + ",";
    out += "\"parallel\":" + std::string(parallel ? "true" : "false") + ",";
    out += "\"latency_ns\":" + std::to_string((long long)best_ns) + ",";
    out += "\"rows_per_sec\":" + jnum(n / (best_ns / 1e9)) + ",";
    out += "\"stats\":{";
    out += "\"first\":" + jnum(A.first) + ",\"last\":" + jnum(A.last) + ",";
    out += "\"min\":" + jnum(A.pmin) + ",\"max\":" + jnum(A.pmax) + ",";
    out += "\"total_return_pct\":" + jnum(A.total_ret) + ",";
    out += "\"ann_return_pct\":" + jnum(A.ann_ret) + ",";
    out += "\"ann_vol_pct\":" + jnum(A.ann_vol) + ",";
    out += "\"sharpe\":" + jnum(A.sharpe) + ",";
    out += "\"max_drawdown_pct\":" + jnum(A.mdd) + ",";
    out += "\"mean_daily_pct\":" + jnum(A.mean_daily) + ",";
    out += "\"win_rate\":" + jnum(A.win_rate) + ",";
    out += "\"best_day_pct\":" + jnum(A.best_day) + ",";
    out += "\"worst_day_pct\":" + jnum(A.worst_day) + ",";
    out += "\"avg_volume\":" + jnum(A.avg_vol) + ",";
    out += "\"median_volume\":" + jnum(A.med_vol) + ",";
    out += "\"total_volume\":" + jnum(A.total_vol) + ",";
    out += "\"block_threshold\":" + jnum(A.block_thresh) + ",";
    out += "\"block_days\":" + std::to_string(A.block_days) + ",";
    out += "\"dollar_volume\":" + jnum(A.dollar_vol) + ",";
    out += "\"up_volume_frac\":" + jnum(A.up_vol_frac) + ",";
    out += "\"obv_last\":" + jnum(A.obv_last) + ",";
    out += "\"latest_vwap\":" + jnum(A.latest_vwap) + ",";
    out += "\"latest_mean\":" + jnum(A.latest_mean);
    out += "},";
    size_t stride = std::max<size_t>(1, n / 120);
    out += "\"price\":"  + jarr(price.raw(), n, stride) + ",";
    out += "\"mean\":"   + jarr(A.rmean.data(), n, stride) + ",";
    out += "\"vwap\":"   + jarr(A.rvwap.data(), n, stride) + ",";
    out += "\"volume\":" + jarr(volume.raw(), n, stride);
    out += "}";
    std::cout << out << "\n";
    return 0;
}
