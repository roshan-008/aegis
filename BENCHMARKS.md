# Benchmark ledger

Every optimization commit appends an entry: what changed / why it should be
faster / measured delta. Numbers are best-of-3, 10M synthetic ticks,
window=100, single thread, `-O2 -march=native`.

Correctness rule: every optimized op is tested against its `naive::`
counterpart to 1e-9 before its number is allowed in this table.

## Row 0 — M1 naive baseline (Apple M1, Apple clang 21)

| Operation         | Rows/sec | Complexity |
|-------------------|---------:|-----------|
| rolling_mean(100) |    16.7M | O(n·w)    |
| rolling_std(100)  |     4.7M | O(n·w)    |
| ema(0.1)          |   195.0M | O(n)      |
| rolling_vwap(100) |     8.5M | O(n·w)    |

- **What changed:** nothing — this is the starting line.
- **Why these numbers:** mean/std/vwap recompute the whole window each step
  (O(n·w)); ema is already O(n), which is why it's ~12–40x faster.
- **Next:** sliding-sum mean/vwap → O(n). Expected ~10x on mean.

## Row 1 — Stage 2.1 sliding windows, O(n·w) → O(n) (Apple M1)

| Operation    | naive (M1) | fast sliding | speedup |
|--------------|-----------:|-------------:|--------:|
| rolling_mean |      16.6M |       374.5M |  22.6×  |
| rolling_std  |       4.5M |       313.6M |  70×    |
| rolling_vwap |       8.4M |       372.3M |  44×    |

- **What changed:** window no longer re-summed each step. Maintain a running
  accumulator: add entering element, subtract leaving one. O(1) per output.
- **Why faster (and why more than ~10×):** the naive kernels are O(n·w) with
  w=100, so ~100× fewer FLOPs; realized speedup exceeds the memory-bandwidth
  ceiling estimate because the naive inner loop also thrashes the same window
  repeatedly. rolling_std wins most (70×) — naive std is *two* passes per
  window (mean, then deviations); sliding does one.
- **Numerical work:** rolling_mean/vwap use periodic exact resync (every 4096
  steps) to bound accumulator drift. rolling_std shifts data by K≈window mean
  (variance is translation-invariant) so var=(Σ(x-K)²-…) never cancels; below
  w=33 it falls back to the exact two-pass oracle because incremental one-pass
  variance can't match two-pass at tiny variance. Torture test (+1e6 offset)
  commits the proof: unshifted one-pass diverges by 2.4e-2, shifted matches.
- **Note:** fast sliding ops (≈374M) now exceed ema (164M) — ema's
  multiply-accumulate is a longer loop-carried dependency chain than a bare add.

## Row 2 — Stage 2.3 ring buffer layout (Apple M1, unpinned)

100M ticks, cap=65536, two threads (producer + consumer), best-of-3.

| Layout                  | M ops/sec |
|-------------------------|----------:|
| baseline (shared line)  |      18.7 |
| +alignas(64)            |      17.0 |
| +cached indices         |      21.4 |

- **What changed:** `head_`/`tail_` moved to separate 64B cache lines; producer
  caches `head_`, consumer caches `tail_`, reloading the atomic only near
  full/empty.
- **Honest result:** the effect is within measurement noise on M1. Throughput
  is **payload-transfer-bound, not sync-bound** — each 24-byte Tick must cross
  from the producer's core to the consumer's core once (~50ns inter-core line
  latency dominates), and enlarging capacity 64× (1024→65536) barely moved the
  number, confirming it isn't full/empty ping-pong.
- **Why the textbook 2–3× isn't visible here:** the clean false-sharing
  demonstration needs (a) thread pinning to fixed cores (no portable macOS
  API) and (b) `perf`/cachegrind cache-miss counts. That measurement is
  deferred to the Linux container pass (Stage 2.5). The optimized layout is
  kept as default regardless — it is correct best practice and *does* help on
  pinned x86; absence of a win on unpinned M1 is a platform fact, not a bug.
- **Lesson for the writeup:** "I measured it and the optimization didn't move
  the needle on this microarchitecture, because the bottleneck was elsewhere"
  is a stronger interview answer than a fabricated speedup.

## Row 3 — Stage 2.4 streaming latency, enqueue→dequeue (Apple M1)

5M ticks through the ring; consumer computes an online O(1) rolling_mean(100)
and records per-tick latency. Two regimes, selected by producer pacing.

| Metric      | full-throttle | paced 8M/s |
|-------------|--------------:|-----------:|
| throughput  |     16.3 M/s  |    7.4 M/s |
| p50         |      244.8 µs |   **166 ns** |
| p99         |      335.9 µs |    142.9 µs |
| p99.9       |      551.7 µs |    937.0 µs |
| max         |      666.3 µs |      1.62 ms |
| empty-poll  |         1.6 % |     93.6 % |

- **The measurement decision, made explicit:** full-throttle measures *queue
  residency* — the producer outruns the consumer, the ring stays full, and
  p50 (244µs) equals cap/throughput = 4096/16.3M = 251µs. That is NOT the
  handoff latency. Pacing the producer below the drain rate empties the ring
  (empty-poll 1.6% → 93.6%) and exposes the real number.
- **True SPSC handoff: p50 = 166 ns.** Consistent with M1 inter-core cache-line
  latency (~50ns) plus pop + rolling-mean update + timestamp. This is the
  "how fast is your lock-free queue" answer.
- **The tail is the scheduler, not the ring:** paced p99.9 = 937µs, max =
  1.6ms are the consumer thread being preempted for a full OS quantum while a
  tick waits. Taming that is the next lever — thread pinning, `SCHED_FIFO` /
  real-time priority, `isolcpus`, busy-wait vs. backoff — none implemented
  (macOS lacks the portable knobs); it's the discussion, not the code.
- **Lesson:** always report the percentile *and* the regime. A single "p50
  latency" with no statement of queue occupancy is unfalsifiable.

## Row 4 — Stage 2.2 SIMD three-way, rolling_mean (Apple M1, NEON)

| Version                    | Complexity | rows/sec | vs naive |
|----------------------------|-----------|---------:|---------:|
| naive scalar               | O(n·w)    |    15.0M |   1.0×   |
| naive + NEON (2-wide f64)  | O(n·w)    |    70.9M |   4.7×   |
| fast sliding scalar        | O(n)      |   313.5M |  20.9×   |

- **What changed:** vectorized the inner window sum with NEON `float64x2_t`,
  two accumulators to break the FP-add dependency chain.
- **The lesson, quantified:** SIMD on the O(n·w) kernel bought 4.7×; switching
  the *algorithm* to O(n) sliding bought 20.9× — and beat the SIMD'd naive by
  4.4×. A better big-O outran vectorization of the worse one.
- **Why the sliding path isn't SIMD'd:** its update is loop-carried (each
  output needs the previous running sum), which does not vectorize. So the
  scalar sliding version is simultaneously the fastest and at its ceiling —
  SIMD has nothing to add. Knowing *where* SIMD can and cannot apply is the
  point.
- **AVX2 backend** (4-wide) is written in `simd.hpp` under `__AVX2__` but is
  untested on this M1; it's for the x86/CI build where the three-way table can
  be regenerated and the two ISAs compared.

## Row 5 — Stage 3.1 parallel scaling, rolling_mean (Apple M1, 8 hw threads)

50M ticks, w=100, halo-partitioned, output preallocated (compute only).

| config       | M rows/sec | speedup |
|--------------|-----------:|--------:|
| 1 (inline)   |      377.2 |   1.00  |
| 2 threads    |      978.0 |   2.59  |
| 4 threads    |      944.5 |   2.50  |
| 8 threads    |     2007.4 |   5.32  |

- **What changed:** partition output indices into T contiguous chunks; each
  thread warms its own running sum over a w-1 halo and slides independently.
  No locks, no atomics, no shared writes.
- **Two benchmark bugs found and fixed before trusting a number** (the real
  lesson of this stage): (1) allocating+NaN-filling the 400MB output *inside*
  the timed loop measured malloc, not the kernel → preallocate, in-place API.
  (2) using a spawned std::thread as the T=1 baseline gave superlinear speedups
  because M1 parks a lone thread on an efficiency core → baseline is now the
  inline main-thread (performance-core) compute.
- **Honest result:** parallelism helps (up to ~2 G rows/s, ~5×), scaling is
  sub-linear and non-monotonic (2 threads ≈ 4 threads) because of M1
  big.LITTLE core heterogeneity with no portable pinning API. A clean scaling
  curve needs homogeneous cores or Linux `pthread_setaffinity_np`; deferred.
- Correctness: `par::rolling_mean` matches naive for T∈{1,2,4,8} at 1e-9.

## Row 6 — Stage 3.2 mmap persistence, load strategies (Apple M1)

10M ticks, 240MB binary file; each path timed file→rolling_mean.

| Strategy               | rows/sec | note |
|------------------------|---------:|------|
| CSV parse + compute    |     3.5M | text parsing dominates |
| binary read + compute  |   116.7M | one bulk fread + owning copy |
| mmap pass 1 + compute  |   239.8M | zero-copy view over the mapping |
| mmap pass 2 + compute  |   285.8M | pages resident |

- **What changed:** `save_table` writes a flat binary (16B header + raw
  columns); `MmapTable` maps it `PROT_READ`/`MAP_PRIVATE` and hands back
  `Column::view` spans that point straight into the mapping — no parse, no
  payload copy, no allocation. Kernels run unchanged on the view.
- **Robust finding (cache-state-independent):** text CSV parsing is ~82× slower
  than any binary path — **parsing, not I/O, is the cost**. That's the durable
  lesson.
- **What is deliberately NOT claimed:** a clean cold-cache read-vs-mmap number.
  macOS won't drop the page cache without `sudo purge`, so every run here is
  page-cache-warm (the numbers even flipped direction between runs). Rather
  than mislabel a warm pass "cold," that comparison is deferred to a controlled
  environment. mmap's demonstrated benefit is zero-copy footprint (no 80MB
  payload allocation) and cheap repeated passes; the first-pass-cold speed
  question is left open honestly.
- Correctness: roundtrip (save → mmap → read back) matches source at endpoints.

## Row 7 — Stage 3.3 expression layer (correctness milestone)

Recursive-descent parser → AST → evaluator over `expr.hpp`. Not a perf row;
a capability + correctness row.

- **What shipped:** a real lexer + precedence-climbing grammar
  (`expr → term → factor`), an AST (Num/Col/Call/Bin/Neg), and a tree-walking
  evaluator that dispatches to the `fast::` kernels. Scalars broadcast; columns
  and functions compose elementwise, so `vwap(price,volume,50)-ema(price,0.1)`
  is a length-n vector. ~250 lines, one file, no SQL, no planner.
- **Gate:** 12 expression tests, each asserted equal to the direct kernel call
  it should compile to (incl. precedence `1+2*3=7`, unary minus, composition).
  All green.

## Row 8 — Tensor kernels (Apple M1, `bench_kernels`)

matmul: naive ijk vs blocked (BS=32) + FMA. Row-wise kernels over 4096×256.

| matmul | naive GF/s | blocked GF/s | speedup |
|--------|-----------:|-------------:|--------:|
| 128×128 |      2.50  |        9.08  |  3.6×   |
| 256×256 |      1.97  |        9.09  |  4.6×   |
| 512×512 |      1.66  |        7.88  |  4.7×   |

| row kernel | Melem/s |
|------------|--------:|
| layernorm  |   401.6 |
| softmax    |   219.4 |
| gelu       |    98.3 |

- **What changed:** cache-blocked matmul with an FMA inner loop vs the textbook
  triple loop. The speedup *grows* with size (3.6× → 4.7×) as the naive version
  falls out of cache — exactly the blocking payoff. Naive GF/s drops with size
  (2.5 → 1.7) for the same reason; blocked holds ~8–9 GF/s until 512.
- **gelu is slowest** (98 Melem/s) because it calls `std::erf` per element (a
  real transcendental), vs softmax's `exp` and layernorm's single `sqrt`/row.
- All three carry the numerical-stability formulation (softmax subtracts the row
  max; layernorm shifts by the row mean) and are gated against the oracle in
  `test_runtime` before these numbers count.

## Row 9 — Memory subsystem (Apple M1, `bench_mem`)

| allocate 4 doubles ×4096 | ns/alloc | vs malloc |
|--------------------------|---------:|----------:|
| arena (bump + reset)     |     1.03 |   ~90× faster |
| malloc/free              |    ~22–95 (variance) |  1× |

| create/destroy Msg ×4096 | ns/op | vs new |
|--------------------------|------:|-------:|
| FixedPool                |  4.86 |  ~6× faster |
| new/delete               | 28.81 |  1×    |

- **What changed:** nothing new — this measures the R1 allocators that already
  existed but were never benchmarked. Arena allocation is `align + add` (~1ns);
  malloc pays for size classes, free-list search, and thread safety. The
  malloc/free column is *noisy* (22–95 ns depending on run) which is itself the
  point — a general allocator's latency is unpredictable, the arena's is not.
- This is the measured backing for the "eliminate malloc from hot paths" cost
  card; the debug-build alloc counter separately proves 0 hot-path allocations.

## Row 10 — Runtime overhead (Apple M1, `bench_runtime`)

- **Registry dispatch:** direct call and `std::function`-dispatched call are
  within noise (~1290 vs ~1250 ns for a 256-elem ema — the delta is inside
  run-to-run variance). **Dispatch is free relative to kernel work** because the
  type erasure lives *outside* the inner loop. That's the design claim, measured.
- **One-node graph overhead vs a bare call, by batch size:**

  | batch  | direct ns | graph ns | overhead |
  |--------|----------:|---------:|---------:|
  | 64     |       292 |    3 708 |  ~3.4 µs |
  | 1 024  |     5 125 |    9 042 |  ~3.9 µs |
  | 16 384 |    79 000 |   86 000 |  ~7 µs   |
  | 262 144|  1 270 292| 1 292 166|  ~22 µs  |

  The future/worker handoff is a **fixed ~3–20 µs** that dominates tiny batches
  (12× at n=64) and amortizes to <2% by n=262k. This is the honest "futures
  dominate small work" curve — the scheduler is for batches, not per-tick.
- **Transform-pair fusion:** on the 3-node analytics graph, before/after p50 are
  within noise (~0.8–1.2 ms) — fusion removes 2 dispatch nodes but the kernel
  work (100k-elem ema/zscore) is ~800 µs and swamps the few-µs node saving.
  Fusion's win shows up at *small* batches (see the overhead curve), not here;
  recording the null at large N is the honest result.

## Row 11 — Storage replay + CRC optimization (Apple M1, `bench_storage`)

4M ticks, 91 MB segment, warm page cache. **Three separated numbers** instead of
one conflated "replay throughput":

| path                        | M rows/sec | note |
|-----------------------------|-----------:|------|
| (1) in-memory sum           |     3762.5 | memory-bandwidth ceiling |
| (2) warm pre-opened scan    |     3748.0 | **99.6% of in-memory** — zero-copy mmap views |
| (3) open + validate + drain |       14.8 | 271 ms/query, CRC-bound |

- **The finding:** the replay *scan* (2) runs at memory bandwidth — the mmap
  zero-copy views cost nothing over an in-memory vector. The apparent slowness of
  end-to-end replay (3) is **entirely open-time CRC validation**, a one-time
  fixed cost per query, not a per-row cost. Separating (2) from (3) is the honest
  version; a single blended number would have hidden which half is expensive.
- **Optimization shipped in two curriculum-motivated steps (measured, each
  byte-identical to the bit-by-bit reference — proven by a unit test):**
  1. bit-by-bit CRC-32 (8 shifts/byte) → compile-time **256-entry table**:
     816 → 271 ms/query (3.0×).
  2. table → **slice-by-8** (zlib/Intel technique, 8 bytes/iteration, eight
     compile-time tables): 271 → **100 ms/query**.

  **Total 816 → 100 ms/query (8.2×)** on the same 91 MB segment, same polynomial
  0xEDB88320, every sealed CRC still valid, all Rust tests + clippy green. The
  scan itself (2) is unchanged at ~memory bandwidth; only the fixed open cost
  moved. This is the honest version of "know your algorithms": a hand-rolled CRC
  is correct but naive, and the standard fast form is a measured 8× with zero
  behavior change.

### Cross-platform reference (same code, different target)

| Operation         | Apple M1 (clang 21) | x86 container (GCC 13, -O2) |
|-------------------|--------------------:|----------------------------:|
| rolling_mean(100) |               16.7M |                       28.2M |
| rolling_std(100)  |                4.7M |                        8.2M |
| ema(0.1)          |              195.0M |                       167.9M |
| rolling_vwap(100) |                8.5M |                       15.2M |

Both columns are real measurements — the M1 column is the baseline every M2
delta is measured against (it's the machine the code is developed on). The
asymmetry is itself interview material: the O(n·w) ops are faster on x86 here,
but ema is *faster* on M1 (195M vs 167.9M). ema is a tight sequential
dependency chain that the M1's FP units chew through, while the window-recompute
loops respond differently to each target's `-march=native` and FP pipeline.
Explaining that gap is a real systems answer, not a footnote.

## R1–R4 v3 hypothesis ledger → measurement status

The rows written as hypotheses before measurement; now mostly closed on the M1
host (Rows 8–11 above). Two remain open and are marked as such.

| Benchmark | Status | Result / gate |
|---|---|---|
| direct call vs registry | **MEASURED** (Row 10) | dispatch within noise — free out-of-loop |
| direct call vs one-node graph | **MEASURED** (Row 10) | fixed ~3–20 µs handoff, amortizes past ~16k |
| scalar vs blocked matmul | **MEASURED** (Row 8) | 3.6–4.7×, grows with size; oracle 1e-10 |
| cursor vs in-memory kernels | **MEASURED** (Row 11) | scan at 99.6% of memory; open is CRC-bound |
| transform pair fusion | **MEASURED** (Row 10) | null at large N; win is small-batch only |
| in-process vs TCP | **OPEN** | wire→feature p50/p99/p99.9 — needs the pinned host pass |
| crash recovery | **OPEN** | `kill -9` at every seal phase — needs the fault-injection harness |

## Hybrid Rust boundary — correctness and syscall structure

- Rust unit tests cover segment roundtrip, CRC corruption rejection, and torn
  WAL-tail removal. The C++ runtime test seals through Rust, opens the Rust-owned
  mmap, range-replays through `ReplayCursor`, and runs the existing kernels.
- The loopback integration test verifies 10 Rust-produced frames through the
  C++ `readv` decoder and bounded ring with exact timestamp/price checks.
- Server write structure changed from **N `send` calls for N ticks** to at most
  **ceil(N/1024) `write_all` batches**. This is a deterministic code-path count,
  not a fabricated latency claim; wire→feature p50/p99 remains a host benchmark.
- Hypothesis: per-segment FFI is below measurement noise for large replay
  batches because no kernel loop crosses FFI. Gate: publish cursor throughput
  before/after on the same warm file and host.
