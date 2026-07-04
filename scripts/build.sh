#!/usr/bin/env bash
# Build everything (Rust lib/CLI/server + all C++ targets) and run the test suite.
# The network integration test is made mandatory here (loopback is available on
# any dev machine); set AEGIS_REQUIRE_NETWORK_TEST=0 to relax it.
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD=${BUILD:-build}
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" -j
AEGIS_REQUIRE_NETWORK_TEST=${AEGIS_REQUIRE_NETWORK_TEST:-1} \
  ctest --test-dir "$BUILD" --output-on-failure
