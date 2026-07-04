# results/

Captured evidence. Regenerate with `scripts/bench.sh` (writes a timestamped,
host-labelled run here).

## What's here
- `sample-run-apple-m1.txt` — a committed full benchmark run, verbatim, as
  reference evidence. Absolute numbers are host-specific; the ratios (23×, 8.2×,
  ~90×…) are the portable claim. Host and compiler are in the file header.
- Ad-hoc `benchmarks-<arch>-<timestamp>.txt` runs from `scripts/bench.sh` are
  gitignored so they don't clutter; the sample above is the tracked one.

## What's deliberately NOT here (yet)
- **Flamegraphs**, `perf` reports, `cachegrind` output — these need Linux/`perf`
  or a controlled profiler; they can't be produced cleanly on macOS/arm64, so
  they are deferred to the CI host rather than faked. Tracked as open questions in
  [`../docs/engineering_log/README.md`](../docs/engineering_log/README.md).
- **A per-commit benchmark history chart** — this is early history; a fabricated
  trend line would break the project's one rule (every number is measured). The
  *optimization progression* (real, step-by-step) is in the README and the log.

The absence of a chart is not the absence of evidence — the ledger
([`../BENCHMARKS.md`](../BENCHMARKS.md)) records every number with its method and
caveats, and this directory holds the raw runs behind it.
