#!/usr/bin/env bash
# bootstrap.sh — Clone the upstream libraries this repo references.
#
# Not strictly needed to run the benchmarks (each src/ subdirectory extracts
# the relevant original code with attribution into its own original.h), but
# useful for anyone who wants to browse the surrounding upstream context.

set -euo pipefail
cd "$(dirname "$0")"
mkdir -p third_party

clone_shallow() {
  local url=$1 dir=$2
  if [[ -d "third_party/$dir/.git" ]]; then
    echo "third_party/$dir already present — skipping."
  else
    git clone --depth=1 "$url" "third_party/$dir"
  fi
}

clone_shallow https://github.com/abseil/abseil-cpp.git abseil-cpp
clone_shallow https://github.com/fmtlib/fmt.git         fmt
clone_shallow https://github.com/gabime/spdlog.git      spdlog
clone_shallow https://github.com/nlohmann/json.git      nlohmann-json

echo
echo "Done. See README.md for how to build and run the benchmarks."
