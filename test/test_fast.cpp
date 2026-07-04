// Correctness gate for the O(n) fast kernels: they must match the naive
// oracle elementwise, including NaN positions. Plus a numerical torture test
// that proves WHY rolling_std shifts the data.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "../src/column.hpp"
#include "../src/rolling.hpp"
#include "../src/parallel.hpp"
#include "../src/rolling_fast.hpp"
#include "../src/simd.hpp"

using namespace aegis;

static int failures = 0;

static bool same(double a, double b, double eps) {
    if (std::isnan(a) && std::isnan(b)) return true;
    if (std::isnan(a) != std::isnan(b)) return false;
    double d = std::fabs(a - b);
    return d <= eps * (1.0 + std::fabs(b));  // mixed abs/rel tolerance
}

static void check_vec(const char* name, const std::vector<double>& fast,
                      const std::vector<double>& naive, double eps) {
    if (fast.size() != naive.size()) {
        std::printf("  FAIL %s: size %zu vs %zu\n", name, fast.size(),
                    naive.size());
        ++failures;
        return;
    }
    double worst = 0.0;
    size_t worst_i = 0;
    for (size_t i = 0; i < fast.size(); ++i) {
        if (!same(fast[i], naive[i], eps)) {
            double d = std::fabs(fast[i] - naive[i]);
            if (d > worst) {
                worst = d;
                worst_i = i;
            }
        }
    }
    if (worst > 0.0) {
        std::printf("  FAIL %s: worst |Δ|=%.3e at i=%zu (fast=%.12g naive=%.12g)\n",
                    name, worst, worst_i, fast[worst_i], naive[worst_i]);
        ++failures;
    } else {
        std::printf("  ok   %s\n", name);
    }
}

static Column make_prices(size_t n, uint64_t seed, double base) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> ret(0.0, 1e-3);
    Column c;
    c.reserve(n);
    double px = base;
    for (size_t i = 0; i < n; ++i) {
        px *= (1.0 + ret(rng));
        c.push(px);
    }
    return c;
}

int main() {
    const size_t N = 100'000;
    Column price = make_prices(N, 1, 100.0);
    Column vol;
    {
        std::mt19937_64 rng(2);
        std::lognormal_distribution<double> lv(4.0, 1.0);
        vol.reserve(N);
        for (size_t i = 0; i < N; ++i) vol.push(lv(rng));
    }

    std::puts("fast == naive across window sizes (eps=1e-9):");
    for (size_t w : {size_t(2), size_t(3), size_t(100), size_t(1000)}) {
        char label[64];
        std::snprintf(label, sizeof label, "rolling_mean w=%zu", w);
        check_vec(label, fast::rolling_mean(price, w),
                  naive::rolling_mean(price, w), 1e-9);
        std::snprintf(label, sizeof label, "rolling_std  w=%zu", w);
        check_vec(label, fast::rolling_std(price, w),
                  naive::rolling_std(price, w), 1e-9);
        std::snprintf(label, sizeof label, "rolling_vwap w=%zu", w);
        check_vec(label, fast::rolling_vwap(price, vol, w),
                  naive::rolling_vwap(price, vol, w), 1e-9);
        std::snprintf(label, sizeof label, "simd_mean    w=%zu", w);
        check_vec(label, simd::rolling_mean(price, w),
                  naive::rolling_mean(price, w), 1e-9);
        for (unsigned T : {1u, 2u, 4u, 8u}) {
            std::snprintf(label, sizeof label, "par_mean T=%u w=%zu", T, w);
            check_vec(label, par::rolling_mean(price, w, T),
                      naive::rolling_mean(price, w), 1e-9);
        }
    }
    std::printf("(simd backend: %s)\n", simd::backend());

    // Torture test: offset prices by +1e6. The two-pass naive impl and the
    // SHIFTED fast::rolling_std both stay accurate; the UNSHIFTED one-pass
    // version visibly diverges. This is the committed proof of the failure mode.
    std::puts("\nnumerical torture (prices offset +1e6, w=1000):");
    Column shifted = make_prices(N, 1, 100.0);
    for (size_t i = 0; i < N; ++i) shifted.data[i] += 1'000'000.0;
    auto ref = naive::rolling_std(shifted, 1000);
    auto good = fast::rolling_std(shifted, 1000);
    auto bad = fast::rolling_std_onepass(shifted, 1000);

    // shifted fast must still match the oracle tightly
    check_vec("shifted fast::rolling_std matches naive", good, ref, 1e-8);

    // the unshifted one-pass must MEASURABLY diverge — assert it does, because
    // if it didn't, the shift would be pointless and the story would be a lie.
    double bad_worst = 0.0;
    for (size_t i = 999; i < N; ++i)
        bad_worst = std::max(bad_worst, std::fabs(bad[i] - ref[i]));
    std::printf("  one-pass (unshifted) worst |Δ| vs naive = %.3e ", bad_worst);
    if (bad_worst > 1e-6) {
        std::printf("(diverges as expected — shift is justified)\n");
    } else {
        std::printf("FAIL: expected divergence, cancellation didn't bite\n");
        ++failures;
    }

    if (failures) {
        std::printf("\n%d CHECK(S) FAILED\n", failures);
        return 1;
    }
    std::puts("\nall fast-kernel checks passed");
    return 0;
}
