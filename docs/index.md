# Engineering index

A map from CS concept → where in Aegis it's implemented, measured, and reasoned
about. Built for revision: pick a topic, follow it to real code, a real number,
and a decision record explaining the trade-off. Every row points at something
that exists in the repo — no placeholders.

| CS concept | Implemented in | Measured in | Reasoned about in |
|------------|----------------|-------------|-------------------|
| **Memory management** — region/bump allocation, free lists | `src/mem/arena.hpp`, `fixed_pool.hpp` | `bench_mem` (Row 9) | `cost_cards.md`; `references.md` (jemalloc/mimalloc) |
| **Cache locality / blocking** | `kernels/core.hpp` `best::matmul` | `bench_kernels` (Row 8) | log [0002], [0003]; `references.md` (H&P) |
| **Algorithmic complexity** — O(n·w)→O(n) | `src/rolling_fast.hpp` | `bench_rolling` (Row 1) | log [0002] |
| **ILP / loop-carried dependencies** | `kernels/core.hpp` `best::sum/dot`; sliding update | disassembly in log [0003] | log [0002], [0003] |
| **SIMD / vectorization (NEON, AVX2)** | `src/simd.hpp` | `bench_rolling` (Row 4) | log [0002] ("SIMD rejected as wrong lever") |
| **Compiler codegen / instruction selection** | verified, not written | asm excerpt in log [0003] | log [0003] (intrinsics declined) |
| **Numerical stability** — cancellation, shifted variance | `rolling_fast.hpp` `rolling_std`; `softmax`/`layernorm` | `test_fast` torture test | log [0002] |
| **Error-detecting codes** — CRC-32, table & slice-by-8 | `rust/src/lib.rs` `crc32` | `bench_storage` (Row 11) | log [0001] |
| **Virtual memory / mmap / page cache** | `mmap_table.hpp`; `rust/src/lib.rs` `Mmap` | `bench_mmap` (Row 6), `bench_storage` | log [0005]; `references.md` (CSAPP, DB Internals) |
| **Concurrency — lock-free SPSC, memory ordering** | `src/ring_buffer.hpp`, `runtime/streaming.hpp` | `bench_ring` (Row 2), `bench_streaming` (Row 3) | log [0004]; `references.md` (Williams, Folly) |
| **False sharing / cache coherence** | `ring_buffer.hpp` (`alignas(64)`, cached indices) | `bench_ring` (null result) | log [0004] |
| **Operating systems — scheduling, threads** | `runtime/scheduler.hpp`, `worker_pool.hpp` | `bench_runtime`, `bench_parallel` (Row 5) | `cost_cards.md`; `references.md` (Velox) |
| **Graph algorithms — DAG, topo sort, DCE, fusion** | `runtime/task_graph.hpp`, `optimizer.hpp` | `bench_runtime` (Row 10) | `cost_cards.md` |
| **Databases — WAL, segments, recovery, zone maps** | `rust/src/lib.rs`; `storage/*.hpp` | `bench_storage` | `references.md` (DB Internals) |
| **Networking — TCP, Nagle, batching, readv** | `src/net/feed.hpp`; `rust` feed server | (percentiles OPEN) | log [0003]; `references.md` (Stevens) |
| **Parsing / trust boundaries / fuzzing** | `rust/src/lib.rs` `Segment::open`; `src/expr.hpp` | `test_expr`, fuzz test | log [0005] |
| **Parallelism — halo partitioning** | `src/parallel.hpp` | `bench_parallel` (Row 5) | `BENCHMARKS.md` Row 5 |
| **Benchmark methodology — p50/p99, anti-DCE, regression gate** | `bench/harness.hpp` | all `bench_*` | `references.md` (Gregg) |
| **Memory safety verification — ASan/UBSan/fuzz** | — | CI `sanitizers` job | `REPORT.md` quality gates |

Log entries: [engineering_log/](engineering_log/README.md). Ledger:
[BENCHMARKS.md](../BENCHMARKS.md). Subsystem↔production map:
[references.md](references.md).
