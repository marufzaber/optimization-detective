#!/usr/bin/env bash
# run_all.sh — Build and run every correctness test and benchmark.
#
# Requires: any recent clang++ or g++ with -std=c++17 support. No cmake,
# no external benchmark framework, no external libraries.

set -euo pipefail
cd "$(dirname "$0")"

CXX=${CXX:-clang++}
CXXFLAGS=${CXXFLAGS:-"-std=c++17 -O3 -Wall -Wextra"}

TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

mkdir -p results

for dir in src/*/; do
  name=$(basename "$dir")
  echo
  echo "======================================================================"
  echo "  $name"
  echo "======================================================================"

  echo
  echo "-- test --"
  $CXX $CXXFLAGS -o "$TMPDIR/test" "$dir/test.cc"
  "$TMPDIR/test"

  echo
  echo "-- benchmark --"
  $CXX $CXXFLAGS -o "$TMPDIR/bench" "$dir/bench.cc"
  "$TMPDIR/bench" | tee "results/${name}_bench.txt"
done

echo
echo "All benchmarks complete. Raw output saved under results/."
