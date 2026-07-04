# 0001 — CRC-32: bit-by-bit → table → slice-by-8

- **Status:** shipped
- **Subsystem:** `rust/src/lib.rs` (`crc32`), validated on `open` and `seal`
- **CS concept:** error-detecting codes; table-driven finite-state computation
- **Host of record:** Apple M1, cargo 1.96 release, `bench_storage`, 91 MB segment

## Problem
`bench_storage` first measured full-query replay at **~5 M rows/s (816 ms)** while
the in-memory scan ran at ~3.7 G rows/s — a 0.1% retention that looked broken. It
wasn't a benchmark artifact: `Segment::open` CRC-validates every column over the
whole segment, and the CRC was the cost.

## Hypothesis
The CRC-32 was the slowest possible form — a bit-by-bit loop (8 shifts per byte),
which runs near ~100 MB/s. Over 91 MB that predicts ~0.8 s, matching the 816 ms.

## Measurement
Separated the three costs in `bench_storage` so the finding couldn't hide behind a
blended number:
- (2) warm pre-opened scan: **99.6% of in-memory** → the mmap views cost nothing.
- (3) open+validate+drain: **816 ms/query** → the CRC is the entire fixed cost.
Confirming the scan is *not* the bottleneck was what justified touching the CRC.

## References consulted
- *Database Internals* (Petrov) — checksums on pages/segments.
- zlib `crc32.c`; Intel, "Fast CRC Computation Using PCLMULQDQ" — the table and
  slice-by-N lineage.
- "know your algorithms": a correct hand-rolled CRC and the fast standard form
  produce identical output; the difference is purely how many bytes per iteration.

## Alternatives considered
1. **256-entry table** (1 byte/iter) — simplest fast form. Chosen as step 1.
2. **Slice-by-8** (8 bytes/iter, eight tables) — zlib/Intel's technique. Chosen
   as step 2.
3. **Slice-by-16 / PCLMULQDQ (hardware CRC)** — rejected: x86-specific
   (`_mm_crc32`) or wide-table; arm64 has its own `crc32` instructions but wiring
   ISA intrinsics for a one-time-per-query cost is unjustified gold-plating.
4. **Validate once and trust thereafter** — rejected: the safety posture is
   "Rust validates untrusted input on every open"; caching trust weakens it. The
   cost is now low enough that keeping the check is cheap.

## Implementation
Two byte-identical steps, both with compile-time (`const fn`) tables so there is
no runtime init:
1. bit-by-bit → 256-entry table.
2. table → slice-by-8: consume 8 bytes/iteration via eight tables, then a
   byte-wise remainder loop for the tail.

Correctness is *proven*, not asserted: `crc32_matches_bitwise_reference` compares
the shipped `crc32` against the bit-by-bit definition for every length 0..64
(exercising the <8-byte remainder path). Same polynomial 0xEDB88320, so every
previously sealed segment's stored CRC stays valid — **on-disk compatibility
preserved**.

## Before / after
| CRC-32 form           | open+drain / query |
|-----------------------|-------------------:|
| bit-by-bit (original) |             816 ms |
| 256-entry table       |             271 ms |
| slice-by-8            |         **100 ms** |

**8.2× total**, zero behavior change. The scan (2) is unchanged at ~memory
bandwidth; only the fixed open cost moved. All Rust tests + `clippy -D warnings`
green.

## Why other approaches were rejected
Hardware/PCLMULQDQ CRC would beat slice-by-8, but it is ISA-specific complexity
for a cost that is one-time per query and already amortized across a whole
segment's batches. The measured 8.2× with *no* portability cost and *no* on-disk
change is the right stopping point; slice-by-16 / hardware CRC is noted as a
future lever, not pretended to be done.

## Surprise
Two things. First, I assumed the replay bottleneck would be the *scan* (mmap
faults, kernel work) — it was the opposite: the scan is at memory bandwidth and
100% of the cost is a CRC I hadn't thought about. Separating the three numbers is
what exposed that; a blended "replay throughput" would have sent me optimizing the
wrong half. Second, I expected the table version to be "the" fix — slice-by-8 then
went another 2.7× beyond it, a reminder that the standard fast form of a classic
algorithm is often much faster than the *obvious* fast form.

## Looking back
Same decision today. The assumption that "Rust re-validates untrusted input on
every open" is worth keeping *held* — but only because the cost dropped enough to
make it cheap; at 816 ms it would have been worth questioning the policy itself.
The assumption that a `const fn` table is free at runtime held (no init cost). If
replay ever opened the *same* segment thousands of times per query, the right move
would flip to validate-once-and-cache — the decision is cost-dependent, not
absolute.

## Open questions
- arm64 has a hardware `crc32` instruction and x86 has `_mm_crc32` / PCLMULQDQ.
  Do they beat slice-by-8 here enough to justify ISA-specific code? **I can't
  answer without measuring on Linux/x86 + wiring the arm64 intrinsic** — listed
  as a candidate, not claimed.
- Is open-time CRC even memory-bound now, or still compute-bound? Needs a
  bandwidth measurement (`perf stat` cache/mem counters — Linux).
