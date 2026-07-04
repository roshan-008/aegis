# 0002 — Rolling kernels: O(n·w) → O(n) sliding; SIMD rejected as the wrong lever

- **Status:** shipped
- **Subsystem:** `src/rolling_fast.hpp` (vs `src/rolling.hpp` oracle), `src/simd.hpp`
- **CS concept:** algorithmic complexity; ILP / loop-carried dependencies; numerical stability
- **Host of record:** Apple M1, Apple clang 21, `-O2 -march=native`, `bench_rolling`

## Problem
The naive rolling kernels recompute the whole window every step — O(n·w). At
w=100 that is ~100× more work than necessary. `bench_rolling` baseline:
rolling_mean 16 M rows/s, rolling_std 4.5 M, rolling_vwap 8 M.

## Hypothesis
Two competing levers:
- **(A) Vectorize** the inner window sum with NEON (SIMD the O(n·w) work).
- **(B) Change the algorithm** to a sliding accumulator: add the entering
  element, subtract the leaving one — O(1) per output, O(n) total.
Hypothesis: (B) dominates, because it removes ~100× of the work rather than
making the same work wider.

## Measurement
Built the three-way table for rolling_mean to test both levers head to head:

| version                    | complexity | rows/s | vs naive |
|----------------------------|-----------|-------:|---------:|
| naive scalar               | O(n·w)    |  15.0 M |   1.0×  |
| naive + NEON (2-wide f64)  | O(n·w)    |  70.9 M |   4.7×  |
| sliding scalar             | O(n)      | 313–375 M | 21–23× |

SIMD bought **4.7×**; the algorithm bought **~23×** and beat the *SIMD'd* naive by
~5×. Hypothesis confirmed with numbers, not intuition.

## References consulted
- Hennessy & Patterson, *Computer Architecture: A Quantitative Approach* — ILP,
  and the multi-accumulator trick to break the FP-add dependency chain.
- Roofline framing in `docs/profiling.md`.

## Alternatives considered
1. **SIMD the naive kernel** — real 4.7×, but it optimizes the wrong complexity
   class. Kept in `simd.hpp` as the *teaching contrast*, not the production path.
2. **SIMD the sliding kernel** — impossible in the useful sense: the sliding
   update is loop-carried (each output needs the previous running sum), which does
   not vectorize. So the scalar sliding version is simultaneously the fastest and
   at its ceiling — SIMD has nothing to add.
3. **Multi-accumulator sliding** — breaks correctness (the running sum is a single
   dependency), so N/A.

## Implementation
`fast::rolling_mean/vwap/std` maintain running accumulators with periodic exact
resync (every 4096 steps) to bound floating-point drift. rolling_std carries a
real numerical-stability decision (logged inline): it shifts data by K ≈ window
mean (variance is translation-invariant) so `Σ(x−K)²` never cancels, and **falls
back to the exact two-pass oracle below w=33** because incremental one-pass
variance cannot match two-pass at tiny variance. A torture test at +1e6 offset
commits the proof: unshifted one-pass diverges by 2.4e-2, shifted matches.

## Before / after
| kernel        | naive | sliding | speedup |
|---------------|------:|--------:|--------:|
| rolling_mean  | 16.0 M | 375 M  |  ~23×   |
| rolling_std   |  4.5 M | 298 M  |  ~70×   |
| rolling_vwap  |  8.0 M | 375 M  |  ~44×   |

rolling_std wins most because naive std is *two* passes per window (mean, then
deviations) and sliding is one. Every sliding kernel is gated against its naive
oracle to 1e-9 (std to 1e-8) before its number is allowed in `BENCHMARKS.md`.

## Why other approaches were rejected
The headline lesson: **a better big-O beats vectorizing a worse one.** SIMD is
kept only as the measured contrast that proves it. Chasing intrinsics on the
sliding path would be effort against a loop-carried dependency that cannot
vectorize — a classic misapplied optimization.

## Surprise
I expected SIMD to dominate — it's the flashy lever. Instead, reducing complexity
from O(n·w) to O(n) produced ~23× while SIMD added only ~4.7×, and the SIMD'd
naive kernel *lost to* the scalar sliding one. The durable lesson: **exhaust
algorithmic improvements before ISA-specific optimization.** The second surprise
was numerical: my first "clever" one-pass windowed variance passed casual tests
but the +1e6 torture test showed it diverging by 2.4e-2 — catastrophic
cancellation is invisible until you deliberately provoke it.

## Looking back
Same decision on the algorithm. The one part I'd want to revisit is the
`STD_SLIDE_MIN = 33` threshold below which rolling_std falls back to the exact
two-pass oracle — it was tuned empirically on this host, not derived. The
assumption "sliding std is fine above some small window" held, but 33 is a magic
number I'd rather justify with an error-vs-window curve. The 4096-step resync
interval is likewise a chosen trade-off (drift vs cost) I never swept.

## Open questions
- Is `STD_SLIDE_MIN = 33` right on other compilers/hardware, or is it fitting M1
  clang's FP behavior? Needs a cross-target error sweep.
- Does the resync interval (4096) sit at the right accuracy/speed point? I have no
  error-vs-interval measurement — only that it's "good enough" here.
- Could the sliding update be reformulated to expose *some* parallelism (e.g.,
  segmented prefix sums across chunks) without losing the O(n)? Open; the halo
  partitioning in `parallel.hpp` is the coarse-grained answer, not a fine one.
