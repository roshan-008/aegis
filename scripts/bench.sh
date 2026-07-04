#!/usr/bin/env bash
# Reproduce the full benchmark suite and capture a timestamped, labelled run into
# results/. "Can I reproduce the results?" — yes: run this. Absolute numbers are
# host-dependent (the ledger in BENCHMARKS.md names its host); the *ratios* are
# what carry across machines.
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD=${BUILD:-build}
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" -j >/dev/null

OUT="results/benchmarks-$(uname -m)-$(date +%Y%m%d-%H%M%S).txt"
mkdir -p results
{
  echo "# Aegis benchmark run"
  echo "host: $(uname -srm)"
  echo "compiler: $(${CXX:-c++} --version | head -1)"
  echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo
  for b in bench_rolling bench_kernels bench_mem bench_runtime bench_storage \
           bench_ring bench_streaming bench_parallel bench_mmap; do
    echo "===== $b ====="
    "./$BUILD/$b" || echo "(exited $?)"
    echo
  done
} | tee "$OUT"
echo "saved -> $OUT"
