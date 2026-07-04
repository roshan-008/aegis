# Profiling & roofline — where the cycles go

This is the evidence pass for M2. It has two parts: (1) an analytical roofline
derived from the **measured** throughput numbers in BENCHMARKS.md, which is
enough to classify each kernel as compute-, latency-, or bandwidth-bound; and
(2) the exact hardware-counter commands to confirm it. The counter run is done
on Linux/x86 in a container (perf + cachegrind) and on macOS via Instruments —
those tools are not available in the environment where the code was built, so
the counter tables below are marked PENDING and are reproduced with the listed
commands.

## Machine

Apple M1 (arm64), Apple clang 21, `-O2 -march=native`. Peak DRAM bandwidth
≈ 68 GB/s; core ≈ 3.2 GHz.

## Roofline from measured throughput

For 10M ticks, window w=100. "Bytes/elem" counts DRAM traffic: the streamed
read of the price column (8 B/elem) plus the output write (8 B/elem); the
trailing edge `c[i-w]` is ~800 B behind the leading edge (100 doubles) and
stays resident in L1 (32 KB), so it is not DRAM traffic.

| kernel               | rows/s | ns/elem | ~GB/s DRAM | % of 68 GB/s | bound by |
|----------------------|-------:|--------:|-----------:|-------------:|----------|
| naive scalar mean    |  15.0M |   66.7  |    ~0.2    |    0.3%      | compute (100 dependent adds/output) |
| naive+NEON mean      |  70.9M |   14.1  |    ~1.1    |    1.6%      | compute (vectorized, 2 accumulators) |
| fast sliding mean    | 313.5M |    3.19 |    ~5.0    |    7.4%      | **latency** (loop-carried add + per-output work) |

**Reading the table:**
- The naive kernels are nowhere near memory bandwidth — they are compute-bound
  on the O(n·w) add work. That is why SIMD helps them (4.7×): more adds per
  cycle. Classic compute-bound regime.
- The sliding kernel moves only 5 GB/s — **7% of peak bandwidth**, so it is NOT
  bandwidth-bound either. At 3.19 ns/elem (~10 cycles) the bottleneck is the
  latency of the dependent running-sum update plus per-output work (a load, a
  store, and — see below — a division). This is why SIMD cannot help it: adding
  lanes does not shorten a dependency chain.

## An optimization the roofline predicted — CONFIRMED

`fast::rolling_mean` emitted `sum / w` — a floating-point **division per
output** (~10–15 cycle latency, low throughput). Not on the loop-carried chain,
but at ~10 cycles/elem it still partially gated. Fix: precompute `invw = 1.0/w`,
multiply.

**Measured: 313.5M → 343.8M rows/s (+9.7%), tests still green.** Exactly the
"small but real" the latency-bound roofline predicted — and a clean example of
measurement-driven optimization: the roofline said the kernel wasn't
bandwidth-bound, pointed at per-output latency work, and removing one divide
delivered the predicted single-digit-percent gain. A bandwidth-bound kernel
would have shown ~0% from this change.

## Hardware-counter pass (reproducible commands)

### Linux / x86 container — PENDING
```
# IPC, cache behavior
perf stat -e cycles,instructions,cache-references,cache-misses,\
L1-dcache-loads,L1-dcache-load-misses ./bench_rolling 10000000

# per-line cache simulation
valgrind --tool=cachegrind ./bench_rolling 2000000
cg_annotate cachegrind.out.<pid>

# flamegraph
perf record -g ./bench_rolling 10000000
perf script | stackcollapse-perf.pl | flamegraph.pl > docs/flame_rolling.svg
```
Expected confirmation: naive shows high instructions/elem and low miss rate
(compute-bound); sliding shows ~10× fewer instructions/elem and still-low miss
rate (latency-bound, not bandwidth-bound).

### macOS / arm64 — PENDING
```
xcrun xctrace record --template 'Time Profiler' --launch -- ./bench_rolling
xcrun xctrace record --template 'CPU Counters'  --launch -- ./bench_rolling
# or a quick statistical sample:
sample bench_rolling 5 -f /tmp/aegis.sample && open /tmp/aegis.sample
```

## What the evidence already establishes

Even before the counter run, the throughput-derived roofline is enough to
defend the two headline claims:
1. **SIMD helped the naive kernel because it was compute-bound** (0.3% of BW).
2. **SIMD cannot help the sliding kernel because it is latency-bound, not
   throughput-bound** (7% of BW, ~10 cycles/elem on a dependency chain).
The counter pass upgrades "derived from throughput" to "measured in cycles and
misses"; it is not expected to change the conclusions.
