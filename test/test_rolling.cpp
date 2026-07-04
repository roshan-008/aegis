// Correctness tests. When you write optimized versions, test them AGAINST
// the naive ones here — the naive impls become your oracle.
#include <cassert>
#include <cmath>
#include <cstdio>

#include "../src/column.hpp"
#include "../src/ring_buffer.hpp"
#include "../src/rolling.hpp"

using namespace aegis;

static bool close(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) < eps;
}

int main() {
    Column p;
    for (double v : {1.0, 2.0, 3.0, 4.0, 5.0}) p.push(v);

    auto m = naive::rolling_mean(p, 3);
    assert(std::isnan(m[0]) && std::isnan(m[1]));
    assert(close(m[2], 2.0) && close(m[3], 3.0) && close(m[4], 4.0));

    auto s = naive::rolling_std(p, 3);
    // pop std of {1,2,3} = sqrt(2/3)
    assert(close(s[2], std::sqrt(2.0 / 3.0)));

    auto e = naive::ema(p, 0.5);
    assert(close(e[0], 1.0) && close(e[1], 1.5) && close(e[2], 2.25));

    Column vol;
    for (double v : {1.0, 1.0, 2.0, 1.0, 1.0}) vol.push(v);
    auto vw = naive::rolling_vwap(p, vol, 2);
    // window {2,3} with vols {1,2}: (2+6)/3
    assert(close(vw[2], 8.0 / 3.0));

    // Ring buffer smoke test (single-threaded semantics).
    SpscRing<Tick, 8> ring;
    for (uint64_t i = 0; i < 8; ++i)
        assert(ring.try_push({i, 1.0 * i, 1.0}));
    assert(!ring.try_push({99, 0, 0}));  // full
    for (uint64_t i = 0; i < 8; ++i) {
        auto t = ring.try_pop();
        assert(t && t->ts_ns == i);
    }
    assert(!ring.try_pop());  // empty

    std::puts("all tests passed");
    return 0;
}
