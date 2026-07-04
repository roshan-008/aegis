# Aegis — Full Project Report (A → Z)

*Single-process, hybrid Rust + C++20 low-latency execution runtime for streaming
tick analytics. This document is the end-to-end account: what it is, why each
decision was made, everything that was built, how it is tested, and every
benchmark number with the honest caveats attached.*

Host of record for all measurements below: **Apple M1, Apple clang 21, `-O2
-march=native`, unpinned threads.** Where a number is x86, it says so.

---

## A. One-paragraph summary

Aegis is a modular **execution engine** for low-latency dataflow workloads: it
executes directed acyclic graphs of classed kernels over memory-resident data
with predictable latency. Streaming financial analytics (rolling
mean/std/VWAP/EMA) and tensor inference (matmul → layernorm → softmax) are two
*workloads* on one substrate — a memory model, a scheduler, and a kernel
registry, bound together by an `ExecutionContext`. It is built as an engine,
not a script: an aligned arena allocator, a classed
kernel registry, a checked task-graph scheduler with dead-code elimination and
fusion, a Rust-owned storage/WAL/mmap replay layer, and a Rust TCP feed server
that hands validated batches to a C++ `readv`/arena/ring client. Analytics is
one workload on this runtime; a small MLP (matmul → layernorm → softmax) is
another, proving the scheduler and kernels are workload-agnostic. The through-line
of the whole project is **measurement honesty** — every optimization is gated
against a naive oracle to 1e-9, and negative or unmeasurable results are recorded
as such rather than dressed up.

---

## B. Why hybrid Rust + C++

The split is deliberate and defensible:

```
Rust  → files / CRC / WAL / mmap ownership / replay server / CLI
              │  (one FFI call per segment: a validated batch pointer)
C++20 → arena / Span2D / SPSC ring / TaskGraph / SIMD kernels / matmul
```

- **Rust owns everything that can be malformed or that has a lifetime that can go
  wrong**: parsing files off disk, CRC validation, WAL torn-tail recovery,
  ownership of the mmap, and the network server accepting bytes from outside.
- **C++ owns everything where explicit memory layout, intrinsics, and per-tick
  data movement matter**: the arena, the lock-free ring, the SIMD kernels.
- **There is no FFI call inside any kernel loop.** FFI happens once per *segment*
  (a batch handoff), never once per *tick*. That keeps the boundary cost out of
  the hot path by construction.

This is the honest version of "why not pure C++": the untrusted-input and
resource-lifetime code is exactly where memory-safety bugs live, so it lives in
the language that makes those bugs compile errors.

---

## C. The four subsystems (R1–R4)

### R1 — Memory + kernels (the foundation)

- **Arena allocator** (`src/mem/arena.hpp`): bump-pointer, 64-byte aligned,
  pre-faulted, `reset()` per batch, tracks `used()` and `high_water()`. The design
  intent is *zero allocations per tick* — you allocate an arena once and reset it,
  you never `malloc` in the loop.
- **Fixed pool** (`src/mem/fixed_pool.hpp`): fixed-capacity typed object pool with
  `create`/`destroy` and free-list reuse; returns `nullptr` at capacity instead of
  growing (bounded by design).
- **Allocation counter** (`src/mem/alloc_counter.hpp`): a hook to assert the hot
  path stays allocation-free.
- **`Span2D<double>` / views** (`src/span.hpp`): non-owning row-major matrix and
  column views (`mat_view`, `col_view`) so kernels operate on borrowed memory.
- **`ExecutionContext`** (`src/runtime/context.hpp`): the engine seam. A wiring
  harness of non-owning pointers — arena + scheduler + telemetry + kernel
  registry — passed by reference to subsystems instead of globals/singletons.
  `ctx.execute(graph)` is the one entry point that runs a kernel DAG and records
  the run into telemetry; `ctx.kernel(name)` does registry lookup. This is what
  makes Aegis an *engine* (one substrate, workloads as plugins) rather than a
  pile of subsystems. Exercised in `test_runtime`.
- **Kernel registry** (`src/kernels/registry.hpp`, `core.hpp`): **12 classed
  kernels** — `rolling_mean`, `rolling_std`, `rolling_vwap`, `ema`, `sum`, `max`,
  `dot`, `zscore`, `matmul`, `softmax`, `layernorm`, `gelu`. Each is tagged with a
  `KernelClass` (WINDOW / REDUCTION / TRANSFORM / NORMALIZATION / MATRIX) and
  carries both a `naive` oracle and a `best` implementation. `softmax`/`layernorm`
  use the numerically-stable formulation (subtract the max / shift by the mean)
  and are checked against a NumPy-equivalent oracle.

### R2 — Scheduling + streaming

- **TaskGraph** (`src/runtime/task_graph.hpp`): an explicit checked DAG of kernel
  nodes with edges; nodes carry their `KernelClass` and a callable.
- **Optimizer** (`src/runtime/optimizer.hpp`): runs **dead-node elimination**
  (a node whose output nobody consumes and that isn't marked as an output is
  dropped) and **transform-pair fusion** (two adjacent elementwise TRANSFORM nodes
  collapse into one scheduled task). Returns a report (`dead_nodes_removed`,
  `transform_pairs_fused`) so the effect is inspectable, not magic.
- **Scheduler + worker pool** (`src/runtime/scheduler.hpp`,
  `worker_pool.hpp`): Kahn topological levelization, persistent worker threads,
  submit-and-join.
- **Bounded SPSC channel** (`src/runtime/streaming.hpp`): lock-free ring with
  explicit **backpressure policy** — `Drop` (increment a dropped counter, return
  false) or `Block`. Runtime counters (`RuntimeStats`) track drops.
- **Observability** (`src/runtime/observability.hpp`): latency histograms /
  counters.

### R3 — Storage as replay (Rust-owned)

- **Segment format `AEGISSEG/v1`** parsed in Rust (`rust/src/lib.rs`): explicit
  bounds and **CRC** checks on every segment; a corrupt segment is rejected, not
  trusted.
- **WAL + crash recovery**: sealing is `fsync` + atomic `rename`; an uncommitted
  `.partial` never becomes visible. `recover_store` removes torn WAL tails and
  reports how many it dropped.
- **Zero-copy C++ replay** (`src/storage/cursor.hpp`, `reader.hpp`,
  `rust_bridge.hpp`): the C++ `ReplayCursor` opens the Rust-owned mmap and streams
  **range-filtered, batched** rows into an arena — the kernels run unchanged on
  views into the mapping. One FFI call per segment.

### R4 — Network ingestion

- **Rust replay server** (`rust/src/bin/feed_server.rs`, `std::net`, no external
  crates): streams a store over TCP using batched `write_all` (≤ `ceil(N/1024)`
  writes for N ticks, instead of one `send` per tick).
- **C++ client** (`src/net/feed.hpp`, `rust_server.hpp`): `readv` decoder →
  arena → bounded SPSC ring. Frame layout is a fixed **32-byte `WireTick`**
  (`static_assert(sizeof(net::WireTick) == 32)`), with copy accounting.

### Example workloads (`examples/pipelines.hpp`)

- **`AnalyticsPipeline`**: builds a graph over a price column and produces a
  signal — the "market features" workload.
- **`MlpPipeline`**: matmul → layernorm → softmax on the *same* scheduler/graph
  API — proving the runtime is not analytics-specific.

---

## D. How it is tested (5 suites, all green)

```
ctest --test-dir build --output-on-failure
1  rolling ......... naive kernel correctness
2  fast ............ sliding-window kernels vs naive oracle to 1e-9 (+ torture)
3  expressions ..... parser/AST/evaluator vs direct kernel calls (12 cases)
4  runtime_r1_r4 ... R1 arena/pool/registry + R2 DAG/opt/sched + R3 store/cursor
                     + R4 bounded channel + live Rust-TCP→C++ integration
5  rust_unit ....... segment roundtrip, CRC-corruption rejection, torn-tail recovery
```

`runtime_r1_r4` is not a smoke test. In one run it: asserts 64B arena alignment
and high-water; exercises pool reuse to capacity; asserts the registry has
exactly 12 kernels and that `softmax` rows sum to 1.0 and `matmul` matches the
hand-computed oracle; builds a 3-node DAG and asserts the optimizer removes 1
dead node and fuses 1 transform pair and the scheduler runs nodes in topological
order; asserts the bounded channel drops the 3rd push into a cap-2 ring and bumps
`stats.dropped`; runs both example pipelines on the scheduler; seals a 10-row
table through the Rust WAL, opens the mmap cursor with a `[1020,1060]` range +
batch size 3, and asserts exactly 5 rows with the right price sum; then stands up
the Rust TCP server on a loopback port and verifies 10 frames arrive through the
C++ `readv` decoder with exact timestamp/price checks.

**The network path is real, not skipped.** Forcing it:

```
$ AEGIS_REQUIRE_NETWORK_TEST=1 ./build/test_runtime
Rust TCP -> C++ ring integration passed
```

**Warnings:** the C++ builds clean under `-Wall -Wextra` (enforced in
`CMakeLists.txt`); Rust is checked with `cargo clippy -- -D warnings`.

**Quality gates beyond "tests pass" (the maturity pass):**
- **Sanitizers.** The whole suite — including `test_runtime` with its Rust FFI,
  worker threads, mmap, and TCP path — runs clean under **ASan + UBSan** (no
  memory errors, no undefined behavior). LeakSanitizer is unavailable on
  macOS/arm64, so leak-checking is a Linux-CI follow-up, stated not faked.
- **Fuzzing at the trust boundary.** `open_never_panics_on_malformed_input`
  throws 4000 random-length/random-byte files plus single-byte-flipped valid
  segments at the Rust `Segment::open` parser and asserts it never panics (Err
  is fine, a crash is not). This is the test behind the "Rust owns untrusted
  input" claim. Writing it also *corrected* a mis-diagnosis: an unguarded
  `bytes[0..8]` slice *looked* like a panic bug, but the fuzz test proved
  `Mmap::open`'s length check already covers it — so the redundant guard was
  removed rather than shipped as a fake fix.
- **Static analysis.** A curated `.clang-tidy` (bugprone/performance/portability/
  misc, with intentional-choice checks disabled and *why* documented) runs with
  no real findings. macOS needs `-isysroot $(xcrun --show-sdk-path)` to parse the
  stdlib; that command is recorded in the config.
- **Flaky test fixed.** `segment_roundtrip_and_recovery` failed ~5% of runs:
  parallel test threads collided on a temp dir keyed by a coarse-granularity
  clock. Fixed with a process-wide atomic counter; **0 failures in 100+ restress
  runs** (was ~2/30).

---

## E. Benchmarks — every number, with the caveat attached

Rules of this ledger: best-of-3, 10M synthetic ticks, window=100, single thread
unless stated, and **every optimized op is asserted equal to its `naive::`
counterpart to 1e-9 before its number is allowed here.**

### E.1 Rolling kernels — the headline algorithmic win

| Operation           | naive O(n·w) | +NEON SIMD O(n·w) | fast sliding O(n) |
|---------------------|-------------:|------------------:|------------------:|
| rolling_mean(100)   |      16.0 M  |          73.0 M   |      **375.1 M**  |
| rolling_std(100)    |       4.5 M  |             —     |      **298.4 M**  |
| rolling_vwap(100)   |       8.0 M  |             —     |      **375.0 M**  |
| ema(0.1)            |     185.3 M  |             —     |    (already O(n)) |

*(rows/sec)*

- **What changed:** the window is no longer re-summed every step. Maintain a
  running accumulator — add the entering element, subtract the leaving one — for
  O(1) per output. With w=100 that's ~100× fewer FLOPs.
- **The lesson, quantified:** SIMD on the O(n·w) kernel bought **4.6×** (16→73M).
  Switching the *algorithm* to O(n) sliding bought **23×** — and beat the SIMD'd
  naive by 5×. **A better big-O outran vectorization of the worse one.**
- **Why the sliding path isn't SIMD'd:** its update is loop-carried (each output
  needs the previous running sum), which doesn't vectorize. So the scalar sliding
  version is simultaneously the fastest *and* at its ceiling — knowing where SIMD
  cannot apply is the point.
- **Numerical stability (the real depth here):** `rolling_std` shifts the data by
  K ≈ the window mean (variance is translation-invariant) so
  `var = (Σ(x−K)² − …)` never suffers catastrophic cancellation. Below w=33 it
  falls back to the exact two-pass oracle, because incremental one-pass variance
  genuinely cannot match two-pass at tiny variance. A torture test with a +1e6
  offset commits the proof: the unshifted one-pass diverges by 2.4e-2, the shifted
  form matches. `rolling_mean`/`vwap` do a periodic exact resync (every 4096 steps)
  to bound floating-point accumulator drift.

### E.2 SPSC ring buffer — an honest null result

100M ticks, cap=65536, producer + consumer threads:

| Layout                  | M ops/sec |
|-------------------------|----------:|
| baseline (shared line)  |     16.0  |
| +alignas(64)            |     15.5  |
| +cached indices         |     18.8  |

- The false-sharing / cached-index optimizations are **within measurement noise**
  on M1. The throughput is **payload-transfer-bound, not sync-bound**: each Tick
  must cross from the producer core to the consumer core once (~50ns inter-core
  line latency dominates), and enlarging capacity 64× barely moved the number,
  confirming it isn't a full/empty ping-pong problem.
- The textbook 2–3× needs thread pinning (no portable macOS API) and cache-miss
  counters (`perf`, Linux-only). **"I measured it and it didn't move the needle
  because the bottleneck was elsewhere" is the finding** — the optimized layout is
  kept because it's correct best practice and helps on pinned x86.

### E.3 Streaming latency — always report the regime

5M ticks through the ring, consumer computes an online rolling_mean(100):

| Metric      | full-throttle | paced (below drain rate) |
|-------------|--------------:|-------------------------:|
| throughput  |     16.4 M/s  |                    ~7 M/s |
| p50         |      246.9 µs |                **~166 ns** |
| p99         |      306.0 µs |                    ~143 µs |
| p99.9       |      475.3 µs |                    ~937 µs |
| max         |      540.7 µs |                    ~1.6 ms |
| empty-poll  |        0.3 %  |                    ~93 %  |

- **Full-throttle measures queue residency, not handoff.** The producer outruns
  the consumer, the ring stays full, and p50 (247µs) ≈ cap/throughput. That is
  NOT the latency of the queue.
- **True SPSC handoff: p50 ≈ 166 ns** — pace the producer below the drain rate,
  the ring empties (empty-poll → 93%), and you see the real inter-core handoff
  cost. That's the "how fast is your lock-free queue" answer.
- **The tail is the OS scheduler, not the ring:** p99.9/max in the ms range are
  the consumer thread being preempted for a full quantum. Taming it needs
  `SCHED_FIFO`/pinning/`isolcpus` — the discussion, not code, because macOS lacks
  the portable knobs.

### E.4 Parallel scaling — halo partitioning

50M ticks, w=100, output preallocated (compute only), 8 hw threads:

| Config      | M rows/sec | speedup |
|-------------|-----------:|--------:|
| 1 (inline)  |      599.9 |   1.00  |
| 2 threads   |     1176.6 |   1.96  |
| 4 threads   |     1713.8 |   2.86  |
| 8 threads   |     2077.2 |   3.46  |

- **What changed:** partition output into contiguous chunks; each thread warms
  its running sum over a w−1 *halo* and slides independently. No locks, no atomics,
  no shared writes.
- **Two benchmark bugs found and fixed before trusting a number** — the real
  lesson of this stage: (1) allocating + NaN-filling the 400MB output *inside* the
  timed loop measured `malloc`, not the kernel → moved to an in-place preallocated
  API; (2) using a spawned `std::thread` as the T=1 baseline gave *superlinear*
  speedups because M1 parks a lone thread on an efficiency core → baseline is now
  the inline performance-core compute.
- Scaling is sub-linear and noisy because of M1 big.LITTLE core heterogeneity
  with no portable pinning API. Clean curve needs homogeneous cores / Linux
  affinity — deferred honestly.

### E.5 mmap persistence — parsing dominates, not I/O

10M ticks, 240 MB binary file, each path timed file → rolling_mean:

| Strategy                | rows/sec | note |
|-------------------------|---------:|------|
| CSV parse + compute     |    3.5 M | text parsing dominates |
| binary read + compute   |  113.5 M | one bulk `fread` + owning copy |
| mmap pass 1 + compute   |  242.2 M | zero-copy view over the mapping |
| mmap pass 2 + compute   |  340.0 M | pages resident |

- **Robust, cache-state-independent finding:** text CSV parsing is **~96× slower**
  than any binary path. Parsing, not I/O, is the cost. That's the durable lesson.
- **What is deliberately NOT claimed:** a clean cold-cache read-vs-mmap number.
  macOS won't drop the page cache without `sudo purge`, so every run here is
  page-cache-warm (the numbers even flipped direction between runs). Rather than
  mislabel a warm pass "cold," that comparison is deferred. mmap's demonstrated
  benefit is zero-copy footprint (no ~80 MB payload allocation) and cheap repeated
  passes.

### E.6 Cross-platform reference (same code, two targets)

| Operation         | Apple M1 (clang 21) | x86 container (GCC 13, -O2) |
|-------------------|--------------------:|----------------------------:|
| rolling_mean(100) |               16.7M |                       28.2M |
| rolling_std(100)  |                4.7M |                        8.2M |
| ema(0.1)          |              195.0M |                       167.9M |
| rolling_vwap(100) |                8.5M |                       15.2M |

Both columns are real measurements. The asymmetry is itself material: the O(n·w)
ops are faster on x86, but **ema is faster on M1** (195M vs 168M) — ema is a tight
sequential dependency chain the M1 FP units chew through, while the
window-recompute loops respond differently to each target's FP pipeline.

### E.7 v3 R1–R4 hypothesis ledger (written *before* measurement)

Some runtime-level micro-benchmarks (registry dispatch overhead, one-node-graph
future overhead, blocked-matmul cache reuse, cursor-vs-in-memory replay,
in-process-vs-TCP copy cost) are recorded in `BENCHMARKS.md` as **hypotheses with
gates**, not invented numbers. They become measured rows only after running on a
named host with the command recorded. This is intentional: the correctness and
syscall *structure* is proven (tests + the deterministic write-batch count); the
latency claims are gated rather than fabricated.

---

### E.8 Every subsystem now benchmarked (Rows 8–11)

The gap this pass closed: several subsystems were tested but never measured. Now
each has a number, via `bench::Harness` (warmup/reps/p50/p99/anti-DCE):

- **Tensor kernels** (`bench_kernels`): blocked+FMA matmul **3.6–4.7×** over naive
  ijk, the speedup *growing* with size as naive falls out of cache. Row kernels:
  layernorm 402, softmax 219, gelu 98 Melem/s (gelu slowest — per-element `erf`).
- **Memory** (`bench_mem`): arena **~1.0 ns/alloc** vs malloc's noisy 22–95 ns;
  FixedPool **4.9 ns/op** vs new/delete 28.8 (~6×). The malloc variance is the
  point — a general allocator's latency is unpredictable, the arena's is not.
- **Runtime overhead** (`bench_runtime`): registry dispatch is within noise of a
  direct call (type erasure is out-of-loop → free); the one-node graph handoff is
  a fixed **~3–20 µs** that dominates tiny batches and amortizes to <2% by 262k —
  the honest "scheduler is for batches, not per-tick" curve.
- **Storage** (`bench_storage`): the zero-copy replay scan holds **99.6% of
  in-memory** throughput; the only fixed cost is open-time CRC. **Optimization
  shipped in two byte-identical steps:** bit-by-bit → 256-entry table → slice-by-8
  (zlib/Intel), cutting open+drain **816 → 271 → 100 ms/query (8.2× total)**,
  proven equal to the reference CRC by a unit test.

## F. Repository map

```
src/
  column.hpp            columnar TickTable (owning + view mode)
  rolling.hpp           naive O(n·w) oracles (correctness reference)
  rolling_fast.hpp      sliding O(n) kernels (mean/std/vwap) + resync/shift logic
  simd.hpp              NEON (primary) + AVX2 (x86, provided) window sum
  parallel.hpp          halo-partitioned multithreaded rolling_mean
  ring_buffer.hpp       templated SPSC ring (padding + cached indices)
  mmap_table.hpp        binary save + zero-copy mmap load
  span.hpp              Span2D / mat_view / col_view
  expr.hpp              lexer → precedence grammar → AST → evaluator
  mem/       arena.hpp  fixed_pool.hpp  alloc_counter.hpp
  kernels/   core.hpp (12 kernels)  registry.hpp (classed dispatch)
  runtime/   task_graph.hpp  optimizer.hpp  scheduler.hpp  worker_pool.hpp
             streaming.hpp  observability.hpp
  storage/   segment.hpp  reader.hpp  cursor.hpp  rust_bridge.hpp
  net/       feed.hpp  rust_server.hpp
rust/
  src/lib.rs            storage / CRC / WAL / mmap / recovery (710 lines)
  src/bin/aegis.rs      CLI: stats / inspect / recover
  src/bin/feed_server.rs replay server (std::net)
examples/pipelines.hpp  AnalyticsPipeline + MlpPipeline
test/   test_rolling  test_fast  test_expr  test_runtime  (+ Rust #[test]s)
bench/  bench_rolling  bench_ring  bench_streaming  bench_parallel  bench_mmap
docs/   profiling.md  cost_cards.md  copy_map.md  references.md
        engineering_log/  (decision records: problem→measurement→trade-off)
```

---

## G. How to build, test, and run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j                       # Rust lib+CLI+server, then all C++
ctest --test-dir build --output-on-failure   # 5 suites, all green
AEGIS_REQUIRE_NETWORK_TEST=1 ./build/test_runtime   # force the TCP path

# benchmarks
for b in rolling ring streaming parallel mmap; do ./build/bench_$b; done

# CLI (Rust)
./build/aegis stats                  # runtime capabilities + kernel count
./build/aegis inspect STORE_DIR      # per-segment row counts + ts ranges
./build/aegis recover STORE_DIR      # drop torn WAL tails, report count
```

Requires CMake, a C++20 compiler, and Rust/Cargo (≥1.79) on 64-bit macOS or
Linux. **No third-party Rust crates.**

---

## H. The through-line: measurement honesty

This is the project's actual thesis, and it shows up in the code and docs, not
just the prose:

1. **Every optimized kernel is gated against a naive oracle to 1e-9** before its
   benchmark number is allowed to exist.
2. **Negative results are recorded as negative** — the ring optimization within
   noise (E.2) is written up as "the bottleneck was elsewhere," not deleted.
3. **Unmeasurable claims are deferred, not faked** — cold-cache mmap (E.5) and the
   runtime latency micro-benchmarks (E.7) are marked as hypotheses with explicit
   gates instead of invented numbers.
4. **Benchmark bugs are hunted before trusting a number** — the two parallel-scaling
   artifacts (E.4) were found and fixed, and the story of finding them is kept.
5. **The regime is always reported alongside the percentile** (E.3) — a p50 with no
   statement of queue occupancy is unfalsifiable.

---

## I. Known limitations / deferred work (stated plainly)

- **Thread pinning** is unavailable on macOS with a portable API, so the ring
  (E.2), streaming tail (E.3), and parallel-scaling (E.4) numbers all carry a
  big.LITTLE / no-affinity caveat. A clean version of each needs a Linux host with
  `pthread_setaffinity_np` / `isolcpus` / `SCHED_FIFO`.
- **Cold-cache mmap** is not measured (needs `sudo purge` or a controlled host).
- **AVX2** kernels are written but untested (this host is arm64/NEON); they're for
  the x86/CI pass where the two ISAs can be compared.
- **Runtime-level latency micro-benchmarks** (E.7) are gated hypotheses awaiting a
  named-host run; correctness and syscall structure are already proven.

---

## J. Bottom line

All four subsystems (R1–R4) are implemented, the build is warning-clean under
`-Wall -Wextra` and `clippy -D warnings`, all five test suites pass (including a
live Rust-TCP→C++ integration path), and every performance claim is either a
measured number gated against an oracle or an explicitly-deferred hypothesis.
Nothing in v3 is left to implement.
