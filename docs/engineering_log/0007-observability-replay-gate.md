# 0007 — Tracing, deterministic replay, and the benchmark gate

- **Status:** shipped (`src/runtime/trace.hpp`, `src/runtime/replay.hpp`, `scripts/check_bench.py`)
- **Subsystem:** runtime observability + reproducibility
- **CS concept:** critical-path analysis, content fingerprinting, regression testing of performance

## Problem
Three claims the project could state but not *check*:
1. "The scheduler is efficient" — no timeline, no idle-time visibility.
2. "Execution is deterministic" — asserted, never verified bit-for-bit.
3. "Every number is measured" — true at publish time, unenforced afterward:
   nothing stopped a later commit from silently halving a kernel.

## Decisions
**Tracing** (`TraceRecorder`): one span per executed node (steady-clock
begin/end + dense worker lane), recorded behind an optional argument to both
schedulers — zero cost when absent, one uncontended mutex acquisition per
node when present (node granularity, not row granularity, so it cannot
distort kernel numbers). Output is standard Chrome trace-event JSON, so
chrome://tracing and Perfetto are the UI; the project ships none.
`critical_path()` re-walks the DAG with *measured* durations: if the result
is close to the wall span, graph shape bounds the run; if far below, the
scheduler or worker count does. That single comparison diagnosed the level
barrier (log 0006).

**Replay** (`RunManifest`): a run records a structural fingerprint of the
graph (names, kernel classes, edges, liveness) and an FNV-1a checksum per
named output buffer, serialized as flat JSON with hex-encoded u64s (JSON
numbers lose integer precision past 2^53 — a real pitfall, not pedantry).
`verify_replay` re-executes and compares, naming the exact output that
diverged. Wall time and worker count are recorded but never compared:
timing may differ, results may not. The storage side was already
reproducible (CRC segments, WAL, paced cursor); this closes the output side.
The test proves sensitivity: a 1e-9 nudge in one input is caught.

**Gate** (`record_metric` + `check_bench.py`): benches append
`{suite, metric, value}` JSONL when `AEGIS_BENCH_JSON` is set;
`results/baseline-apple-m1.jsonl` is the committed reference. Direction is
encoded by convention (`*_ns` lower-is-better, otherwise higher). The gate
is **host-scoped by design**: hard-fail on the baseline host, `--advisory`
on CI whose hardware doesn't match — a hard gate on shared runners would
be noise dressed as rigor. Noisy control metrics (uniform-duration scheduler
speedup) are deliberately not gated; the skewed speedup is gated against
the lowest value observed, not the best.

## Trade-offs
- FNV-1a is a drift detector, not a tamper seal — fine for the job.
- The manifest parser accepts exactly the dialect the writer emits; it is
  not a JSON library and says so.
- Bit-for-bit replay holds for the current kernels because reductions are
  fixed-order per node. A future parallel reduction inside one kernel would
  break it — and the manifest check is exactly what would catch that.
