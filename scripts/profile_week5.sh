#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-week5-bench}"
OPERATIONS="${OPERATIONS:-1000000}"
THREADS="${THREADS:-4}"
OUTPUT="${OUTPUT:-${ROOT_DIR}/week5-sample.txt}"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF
cmake --build "${BUILD_DIR}" --parallel --target week5_bench

if command -v perf >/dev/null 2>&1; then
  exec perf stat -d "${BUILD_DIR}/week5_bench" --mode sync \
    --operations "${OPERATIONS}" --threads "${THREADS}"
fi

if command -v sample >/dev/null 2>&1; then
  profile_root="$(mktemp -d "${TMPDIR:-/tmp}/mini-lsm-kv-profile.XXXXXX")"
  "${BUILD_DIR}/week5_bench" --mode db-put --operations "${OPERATIONS}" \
    --path "${profile_root}/db" >/dev/null &
  benchmark_pid=$!
  sample "${benchmark_pid}" 2 10 -file "${OUTPUT}" || true
  wait "${benchmark_pid}"
  echo "wrote macOS sample profile to ${OUTPUT}"
  exit 0
fi

echo "neither perf nor macOS sample is available" >&2
exit 1
