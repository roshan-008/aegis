#!/usr/bin/env python3
"""Benchmark regression gate.

Compares a fresh bench run (JSONL emitted by bench binaries when
AEGIS_BENCH_JSON is set) against a committed baseline and fails on
regressions beyond a tolerance.

    AEGIS_BENCH_JSON=current.jsonl ./build/bench_mem
    AEGIS_BENCH_JSON=current.jsonl ./build/bench_scheduler
    python3 scripts/check_bench.py results/baseline-apple-m1.jsonl current.jsonl

Each record: {"suite": "mem", "metric": "arena_alloc_ns", "value": 1.58}
Direction convention: metrics ending in "_ns" are lower-is-better; all
others (throughput, speedups) are higher-is-better.

Modes:
  default      exit 1 on any regression — the same-host gate
  --advisory   print verdicts, always exit 0 — for CI hardware that does
               not match the baseline host (absolute numbers shift, so a
               hard gate there would be noise, not signal)
  --update     rewrite the baseline from the current run (deduplicated,
               last value per metric wins)
"""

import argparse
import json
import sys


def load(path):
    metrics = {}
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            metrics[(rec["suite"], rec["metric"])] = float(rec["value"])
    return metrics


def lower_is_better(metric):
    return metric.endswith("_ns")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("baseline")
    ap.add_argument("current")
    ap.add_argument("--tolerance", type=float, default=0.25,
                    help="allowed relative regression (default 0.25)")
    ap.add_argument("--advisory", action="store_true",
                    help="report but always exit 0 (cross-hardware CI)")
    ap.add_argument("--update", action="store_true",
                    help="rewrite baseline from current and exit")
    args = ap.parse_args()

    current = load(args.current)
    if args.update:
        with open(args.baseline, "w", encoding="utf-8") as fh:
            for (suite, metric), value in sorted(current.items()):
                fh.write(json.dumps({"suite": suite, "metric": metric,
                                     "value": value}) + "\n")
        print(f"baseline {args.baseline} rewritten with {len(current)} metrics")
        return 0

    baseline = load(args.baseline)
    regressions, improved, missing = [], 0, []
    for key, base in sorted(baseline.items()):
        suite, metric = key
        if key not in current:
            missing.append(f"{suite}/{metric}")
            continue
        cur = current[key]
        if lower_is_better(metric):
            ratio = cur / base if base else float("inf")
            regressed = cur > base * (1.0 + args.tolerance)
        else:
            ratio = base / cur if cur else float("inf")
            regressed = cur < base * (1.0 - args.tolerance)
        verdict = "REGRESSED" if regressed else "ok"
        print(f"  {verdict:9s} {suite}/{metric}: baseline {base:g} -> {cur:g}")
        if regressed:
            regressions.append(f"{suite}/{metric} ({ratio:.2f}x worse)")
        elif ratio < 1.0:
            improved += 1

    for name in missing:
        print(f"  MISSING   {name}: baseline metric absent from current run")

    print(f"\n{len(baseline) - len(missing) - len(regressions)} ok, "
          f"{improved} improved, {len(regressions)} regressed, "
          f"{len(missing)} missing (tolerance {args.tolerance:.0%})")
    if regressions or missing:
        for r in regressions:
            print(f"  regression: {r}", file=sys.stderr)
        if args.advisory:
            print("advisory mode: not failing the build")
            return 0
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
