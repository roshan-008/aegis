# Engineering log

A decision record, not a changelog. A changelog says *what* changed; this says
*why* — the hypothesis, the measurement, the references consulted, the
alternatives rejected, and the trade-off accepted. Each entry is a decision that
actually happened in this repository, with real before/after numbers. **Declined
optimizations are logged too** — deciding *not* to change something after
investigating it is an engineering decision, and often a more revealing one.

Rule: an entry may only cite a number that was measured on a named host with a
command that exists in the repo. No aspirational entries.

## Entries

| # | Date | Decision | CS concept | Outcome |
|---|------|----------|-----------|---------|
| [0001](0001-crc32-slice-by-8.md) | 2026-07-04 | CRC-32: bit-by-bit → table → slice-by-8 | error detection | **shipped**, 816 → 100 ms/query (8.2×), byte-identical |
| [0002](0002-sliding-window-kernels.md) | 2026-07-04 | Rolling kernels: O(n·w) → O(n) sliding; SIMD considered | algorithms / comp. arch | **shipped**, up to 70×; SIMD *rejected* |
| [0003](0003-declined-optimizations.md) | 2026-07-04 | matmul intrinsics; client-side Nagle | compiler codegen / networking | **declined** — evidence said no |
| [0004](0004-ring-false-sharing-null.md) | 2026-07-04 | Ring: cache-line padding + cached indices | concurrency / false sharing | **kept, null result** — payload-bound, not sync-bound |
| [0005](0005-reverted-crc-length-guard.md) | 2026-07-04 | Added a header-length guard to `Segment::open` | virtual memory / trust boundary | **reverted** — the fuzz test disproved the hypothesis |

See [`../index.md`](../index.md) for the CS-concept → where-explored map.

## Template

```markdown
# NNNN — <decision title>

- **Status:** shipped / declined / reverted / open
- **Date / Subsystem / CS concept / Host of record**

## Problem            observation and how it surfaced
## Hypothesis         expected cause + fix, before measuring
## Measurement        the number that confirmed/refuted it (command + result)
## References         books AND real production code (name the file/idea, honestly)
## Alternatives       each option, chosen or not, with the reason
## Implementation     what shipped (or why nothing did)
## Before / after     measured delta on the named host

## Surprise           what I expected vs what actually happened
## Looking back        would I decide the same today? which assumptions held?
## Open questions      what I still can't answer, and what I'd need to (often: a Linux host + perf)
```

The last three sections are the point of a *notebook* over a *log*: they record
judgment and its limits, not just outcomes. "What surprised me" and "questions I
couldn't answer" are the sections that read as engineering maturity — an
interviewer trusts someone who knows the edge of their own knowledge.

## Open candidates (not yet done — listed so they aren't mistaken for done)

- **Networking latency percentiles** — wire→feature p50/p99/p99.9 on a pinned
  host (the in-process-vs-TCP hypothesis in BENCHMARKS.md, still OPEN).
- **Crash recovery under fault injection** — `kill -9` at every seal phase; the
  WAL is designed for it and unit-tested, but the fault harness isn't built.
- **Linux/x86 pass** — thread pinning, `perf`/cachegrind cache-miss counts, and
  the AVX2 kernel path, all of which macOS/arm64 can't measure cleanly.
