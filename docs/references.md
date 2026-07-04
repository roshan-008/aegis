# Aegis under the reading curriculum

Each Aegis subsystem solves a problem that production systems and the standard
references also solve. This maps them — not to claim Aegis *is* jemalloc or
ClickHouse, but to state precisely **which idea is borrowed, which is
deliberately declined, and why**. Every "borrowed" line points at real code;
every "declined" line is a design decision with a stated reason, not an omission.

Reading is connected to implementation, not done cover-to-cover first: the point
is to recognize the class of problem each subsystem belongs to.

---

## Memory — `src/mem/arena.hpp`, `fixed_pool.hpp`

- **References:** CSAPP ch9 (virtual memory, dynamic allocation); jemalloc &
  mimalloc source.
- **Production analog:** per-request arena allocators (region allocation);
  size-class-segregated free lists.
- **Borrowed:** bump/region allocation with reset-per-batch lifetime; a typed
  free-list `FixedPool` (mimalloc's free-list idea, one size class).
- **Deliberately declined:** size classes, thread-local heaps, coalescing,
  fragmentation handling. jemalloc/mimalloc solve the *general multithreaded
  malloc* problem; Aegis sidesteps it because the hot path is single-threaded
  per batch with a region lifetime. **Structure beats a smarter allocator** —
  measured ~1.0 ns/alloc vs malloc's noisy 22–95 ns (`bench_mem`). The debug
  alloc-counter proves 0 hot-path allocations; that is the real invariant, not a
  faster `malloc`.

## Concurrency — `src/ring_buffer.hpp`, `runtime/streaming.hpp`

- **References:** *C++ Concurrency in Action* (Williams) ch5 memory model, ch7
  lock-free, ch8 false sharing.
- **Production analog:** Folly `ProducerConsumerQueue`, the LMAX Disruptor.
- **Borrowed:** acquire/release SPSC ring; cache-line padding + cached indices
  to avoid false sharing (Williams ch8) and needless atomic reloads.
- **Deliberately declined:** MPMC / CAS loops. Ingestion is single-producer, so
  MPMC's compare-exchange retries would be pure cost. The honest result stands:
  on M1 the ring is payload-transfer-bound, not sync-bound (`bench_ring`), so the
  padding is correct best practice whose win is microarchitecture-dependent.

## Scheduling — `runtime/task_graph.hpp`, `optimizer.hpp`, `scheduler.hpp`

- **References:** *Database Internals* (Petrov) execution; *Systems Performance*
  (Gregg) methodology.
- **Production analog:** DuckDB / Velox pipeline execution; morsel-driven
  parallelism; ONNX Runtime graph execution + fusion.
- **Borrowed:** a DAG of classed operators, Kahn topological levels, dead-node
  elimination, and pairwise transform fusion (the "graph rewrite" idea, kept
  deliberately small).
- **Deliberately declined:** work-stealing, adaptive/cost-based scheduling,
  general IR rewrites. Measured, the one-node handoff is a fixed ~3–20 µs
  (`bench_runtime`) — this is a *batch* scheduler, and the numbers say so; a
  per-tick scheduler would be the wrong tool.

## Storage — `storage/*.hpp`, `rust/src/lib.rs`

- **References:** *Database Internals* (WAL, page cache, LSM); CSAPP ch9 (mmap,
  demand paging); *Systems Performance* (page cache behavior).
- **Production analog:** ClickHouse MergeTree parts; LSM write-ahead logs.
- **Borrowed:** append-only sealed segments; WAL with `fsync` + atomic-rename
  commit and torn-tail recovery; per-column CRC; mmap zero-copy reads; header
  min/max **segment skipping** (a poor-man's zone map).
- **Deliberately declined:** LSM compaction/merge, a B-tree/secondary index.
  Aegis is replay-oriented: range scans with segment skipping, not point
  lookups. **Curriculum-driven optimization shipped, in two byte-identical
  steps:** the WAL/segment CRC was a textbook bit-by-bit CRC-32; → compile-time
  256-entry table (816 → 271 ms/query) → **slice-by-8** (zlib/Intel's technique,
  8 bytes/iteration; 271 → 100 ms/query). **8.2× total**, proven equal to the
  bit-by-bit reference by a unit test (`bench_storage`). The zero-copy scan holds
  ~memory bandwidth throughout; only the fixed open cost moved.

## Kernels & SIMD — `src/kernels/core.hpp`, `rolling_fast.hpp`, `simd.hpp`

- **References:** Hennessy & Patterson (*Computer Architecture: A Quantitative
  Approach*) — ILP, roofline; *Effective Modern C++*; Compiler Explorer.
- **Production analog:** Arrow compute kernels; ONNX Runtime / oneDNN kernels.
- **Borrowed:** cache-blocked matmul; multi-accumulator reductions to break the
  FP-add dependency chain (H&P ILP); numerically-stable softmax/layernorm
  (max-subtract / mean-shift); the roofline framing in `docs/profiling.md`.
- **Verified against the compiler (the Compiler Explorer habit):** disassembly
  of `-O2 -march=native` confirms `best::matmul`/`best::dot` emit **FMA**
  (`fmadd`/`fmla`) and the reductions vectorize to **NEON `.2d`** with paired
  `ldp q,q` loads. Because the compiler already emits FMA, **hand-written matmul
  intrinsics are declined** — they would add ISA-specific code for no gain. The
  measured win comes from *blocking* (3.6–4.7×, growing with size), not from
  fighting the vectorizer.

## Networking — `src/net/feed.hpp`, `rust/src/lib.rs` (feed server)

- **References:** Stevens, *UNIX Network Programming*.
- **Production analog:** any low-latency market-data feed handler.
- **Borrowed:** **`TCP_NODELAY` on the write side** (Nagle disabled — the classic
  Stevens latency fix, `set_nodelay(true)` on the server), batched `write_all`
  (≤ ceil(N/1024) syscalls instead of one per tick), `readv` scatter reads, and
  a fixed 32-byte wire frame.
- **Checked and found already correct:** the client is receive-only, so
  client-side Nagle is moot; the write side that matters already disables it. No
  change invented where none was warranted.
- **Deliberately declined:** `epoll`/`kqueue` event loops. Replay is a single
  connection; an event loop adds machinery without benefit at this scale. That
  becomes the right tool only for many concurrent feeds — stated, not pretended.

## Methodology — `bench/harness.hpp`, `BENCHMARKS.md`

- **References:** *Systems Performance* (Gregg) — USE method, avoiding the
  observer effect.
- **Borrowed:** warmup + best-of-N, p50/p99, an anti-DCE checksum, a hot-path
  allocation counter, and a ±10% regression gate. Negative/null results are
  recorded (ring within noise, fusion null at large N, cold-cache mmap deferred)
  rather than hidden — the project's core discipline.

---

### What this map is for

In an interview, "why did you build the allocator this way?" should be
answerable as: *"jemalloc solves general multithreaded malloc; my hot path has a
region lifetime, so I used a bump allocator and measured 1 ns/alloc — here's the
number and here's what I gave up."* Every row above is that answer, backed by a
file and a benchmark.
