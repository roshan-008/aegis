# 0004 — Ring buffer cache-line padding + cached indices: a null result

- **Status:** kept (correct best practice) — but a measured **null result**, logged honestly
- **Subsystem:** `src/ring_buffer.hpp`
- **CS concept:** concurrency — false sharing, MESI cache coherence, memory ordering
- **Host of record:** Apple M1 (unpinned), `bench_ring`, 100M ticks, cap 65536

## Problem
An SPSC ring where the producer's `tail_` and the consumer's `head_` share a
cache line causes **false sharing**: each side's write invalidates the other's
line, forcing coherence traffic. Textbook fix: put them on separate 64-byte lines
and cache the peer index so the atomic is reloaded only near full/empty.

## Hypothesis
Padding + cached indices should buy the classic 2–3× that concurrency texts show
for false-sharing removal.

## Measurement
`bench_ring`, three layouts, best-of-3:

| layout                  | M ops/sec |
|-------------------------|----------:|
| baseline (shared line)  |     16.0  |
| +alignas(64)            |     15.5  |
| +cached indices         |     18.8  |

The movement is **within run-to-run noise.** Enlarging capacity 64× (1024 →
65536) barely moved the number either — which is the diagnostic: if it were
full/empty ping-pong, more slots would help.

## References consulted
- *C++ Concurrency in Action* (Williams), ch8 (false sharing), ch5 (memory model).
- Production analog: Folly `ProducerConsumerQueue`, the LMAX Disruptor — both pad
  aggressively, and are right to.

## Alternatives considered
Reverting the padding (simpler struct) vs keeping it. Kept it: on a *pinned* x86
core pair the padding does help, and it is unambiguously correct best practice.
The absence of a win here is a property of the host, not a defect.

## Before / after
No significant change on M1. Kept for correctness, not for a number I can claim.

## Surprise
I expected the canonical 2–3×. Instead the throughput is **payload-transfer-
bound, not sync-bound**: every 24-byte tick must cross from the producer core to
the consumer core once, and that inter-core line transfer (~tens of ns) dominates
the handshake the padding protects. The optimization is aimed at a bottleneck
that isn't the bottleneck here.

## Looking back
Right call to keep the padding, right call to *not* claim a speedup. The honest
sentence — "I measured it and it didn't move the needle on this microarchitecture
because the bottleneck was elsewhere" — is a stronger answer than a fabricated
number, and it's the one I'd give an interviewer. What I'd change: I should have
predicted the payload-bound regime from a back-of-envelope (24 B × 100 M =
2.4 GB crossing cores) *before* running, not after.

## Open questions
- On two **pinned** physical cores (Linux `pthread_setaffinity_np`), does the
  2–3× appear? I can't test it — macOS has no portable pinning API. Listed as a
  Linux-CI candidate.
- What does `perf c2c` (cache-to-cache transfer profiling) say the actual
  false-sharing rate is? Unmeasurable on this host.
