# 0005 — A header-length guard I added, then reverted

- **Status:** reverted (the fuzz test disproved my own hypothesis)
- **Subsystem:** `rust/src/lib.rs` (`Segment::open`)
- **CS concept:** virtual memory / mmap bounds; defensive programming vs invariants
- **Host of record:** Apple M1, `cargo test` (`open_never_panics_on_malformed_input`)

## Problem
While writing a fuzz test for the parser trust boundary, I read `Segment::open`
and saw `&bytes[0..8]` and `bytes[0..HEADER_BYTES]` — raw slices with no visible
length check. Raw slicing a shorter slice panics in Rust. It *looked* like a
crash-on-malformed-input bug at exactly the place that must never crash.

## Hypothesis
A file under 8 bytes reaches `Segment::open`, `&bytes[0..8]` panics, and the
"Rust owns untrusted input" claim is false. Fix: guard `bytes.len() < HEADER_BYTES`
at the top and return `Err`.

## Measurement
Wrote `open_never_panics_on_malformed_input`: 4000 random-length/random-byte
files plus single-byte-flipped valid segments, asserting `open` never panics.
Then — the key step — I **removed my guard and reran the fuzz test.** It still
passed. My hypothesis was wrong.

Reading one layer down explained why: `Mmap::open` already rejects
`len < HEADER_BYTES` before `Segment::open` ever slices. The invariant was
enforced; my guard was redundant, and worse, it *looked* like it was fixing a
live bug that didn't exist.

## References consulted
- CSAPP ch9 (mmap, file-backed mappings and their length).
- The project's own honesty rule: a fix that changes no behavior is not a fix.

## Alternatives considered
1. Keep the guard as "defense in depth." Rejected — redundant code that masquerades
   as a bug fix is worse than no code; it misleads the next reader about what
   invariant holds where.
2. Remove the guard, keep the fuzz test. **Chosen.** The test is the real asset:
   it *proves* the panic-free property and guards against a future refactor that
   might weaken `Mmap::open`'s check.

## Before / after
Net code change from my investigation: **the fuzz test, and nothing else.** The
guard was added and then removed within the same session.

## Surprise
I was confident I'd found a real panic bug — the slices genuinely have no local
length check. The surprise was that the invariant lived one function away, and
that my "fix" was untestable-as-a-fix because the failing input it supposedly
handled couldn't reach the code. The fuzz test passing *without* the guard is what
turned a confident wrong belief into a correct one.

## Looking back
The right outcome, reached the right way: I let the test adjudicate, not my
reading. What I'd do differently is check the *callers'* invariants before
concluding a slice is unguarded — the bug I "found" was disproved by the function
directly above it. The lasting lesson: distinguish "this line has no local check"
from "this line can receive bad input"; only the second is a bug.

## Open questions
- Are there other raw slices in the codebase whose safety depends on a
  caller-enforced invariant that isn't documented at the slice? A `# Safety` /
  invariant comment audit would be worth a pass. (`Segment::open` now has one.)
