# Cost cards

## Arena allocator

GOAL eliminate malloc from hot paths.  
MECHANISM pre-faulted 64-byte-aligned slab; allocation is align+add; reset is one store.  
TRADEOFF reset-only lifetime and fixed capacity.  
ALTERNATIVE `pmr::monotonic_buffer_resource`—valid, but rejected for explicit alignment, hooks, and inspectability.  
BENCHMARK streaming hot-path project allocations/tick = 0 after warmup (gate in harness).

## Fixed pool

GOAL give messages bounded, reusable ownership.  
MECHANISM in-place objects and an index free-list avoid heap traffic.  
TRADEOFF compile-time capacity.  
ALTERNATIVE general heap—rejected for allocator latency and unbounded footprint.  
BENCHMARK `bench_mem`: create/destroy 4.86 ns/op vs new/delete 28.81 ns (~6× faster).

## Span views

GOAL pass storage-independent contiguous batches without copies.  
MECHANISM pointer+shape+element-stride is trivially copied and aliases arena/vector/mmap memory.  
TRADEOFF lifetime is external and R1–R3 expose only double aliases.  
ALTERNATIVE owning generic tensors—rejected until another scalar workload justifies the test surface.  
BENCHMARK pending: direct pointer vs view loop.

## Kernel registry

GOAL discover and dispatch kernels by structural class.  
MECHANISM one metadata table selects permanent naive or best callable paths.  
TRADEOFF type-erased dispatch uses `std::function` outside the inner loop.  
ALTERNATIVE industry-specific registries—rejected because the runtime does not care about quant vs inference.  
BENCHMARK `bench_runtime`: registry vs direct call within noise (~1.25 vs 1.29 µs/call) — dispatch is out-of-loop, so free relative to kernel work.

## Benchmark harness

GOAL make throughput and tail regressions reproducible.  
MECHANISM warmup, repeated timings, p50/p99, checksum anti-DCE, allocation hook, and 10% ledger gate.  
TRADEOFF repetition lengthens CI.  
ALTERNATIVE best-of-one—rejected because it hides distribution and tails.  
BENCHMARK harness overhead is reported per invocation; perf counters remain Linux follow-up.

## Task graph and scheduler

GOAL share one execution API across batch and streaming work.  
MECHANISM Kahn levels expose ready work to persistent workers; one node is the direct case.  
TRADEOFF graph validation, futures, and a barrier per level.  
ALTERNATIVE separate batch/stream runtimes—rejected because semantics and instrumentation would diverge.  
BENCHMARK `bench_runtime`: one-node graph adds a fixed ~3–20 µs future/worker handoff — 12× at batch 64, <2% by batch 262k. The scheduler is for batches, not per-tick.

## Bounded ring

GOAL make overload and memory use explicit.  
MECHANISM fixed power-of-two slots; full means drop+count or block+count.  
TRADEOFF chosen loss or producer stall.  
ALTERNATIVE unbounded queue—rejected because it converts overload into memory growth and latency.  
BENCHMARK existing three-layout ring table plus e2e drop counters.

## mmap replay

GOAL replay datasets larger than RAM at page-cache speed.  
MECHANISM Rust owns mmap and validates the whole format once; C++ receives three page-cache-backed pointers; sequential advice enables readahead.  
TRADEOFF cold page faults, OS-controlled eviction, and one per-segment FFI open/close.  
ALTERNATIVE `read()` into owned buffers—rejected for payload copy and double caching.  
BENCHMARK `bench_storage`: warm pre-opened scan holds 99.6% of in-memory throughput (3748 vs 3762 M rows/s) — the mmap views cost nothing; open-time CRC is the only fixed cost.

## WAL and segments

GOAL expose only complete, verified replay data after a crash.  
MECHANISM safe Rust file APIs perform intent, partial write, CRCs, fsync, atomic rename, directory fsync and commit; checked parsing removes pointer-arithmetic hazards.  
TRADEOFF two WAL fsyncs per seal and append-only updates.  
ALTERNATIVE in-place mutable files—rejected because torn state is ambiguous.  
BENCHMARK `bench_storage`: whole-segment CRC on open is the dominant per-query cost; CRC-32 bit-by-bit → table → slice-by-8 cut open+drain 816→271→100 ms/query (8.2× total, byte-identical). kill-9 recovery remains a required platform test.

## TCP ingestion

GOAL turn framed wire ticks into bounded runtime work with known copies.  
MECHANISM Rust batches 1,024 replay frames per 32 KiB write; C++ `readv` fills arena memory, casts frames in place, and copies accepted ticks into SPSC slots.  
TRADEOFF little-endian fixed frames, one partial-frame carry copy, and two languages in the build.  
ALTERNATIVE per-frame send/parse/allocation—rejected for syscall volume, branches, and allocator traffic.  
BENCHMARK pending: wire→feature percentiles and in-process delta.

## Graph optimization

GOAL remove useless work and intermediate dispatch.  
MECHANISM backward liveness from outputs plus safe exclusive TRANSFORM-pair fusion.  
TRADEOFF fusion is deliberately pairwise and does not rewrite arbitrary kernels.  
ALTERNATIVE general compiler IR—rejected as unjustified complexity for R4.  
BENCHMARK pending: analytics fused before/after.
