#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
BENCH_BIN="${BUILD_DIR}/kv_bench"
BENCH_ROOT="${ROOT_DIR}/bench_runs"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}"
cmake --build "${BUILD_DIR}"

rm -rf "${BENCH_ROOT}"
mkdir -p "${BENCH_ROOT}"

run_case() {
  local name="$1"
  shift
  local db_path="${BENCH_ROOT}/${name}_db"

  echo
  echo "== ${name} =="
  "${BENCH_BIN}" --path "${db_path}" "$@"
}

run_case write_100k --write 100000 --value-size 100
run_case read_sequential_100k --write 100000 --value-size 100
"${BENCH_BIN}" --path "${BENCH_ROOT}/read_sequential_100k_db" --read 100000

run_case read_random_100k --write 100000 --value-size 100
"${BENCH_BIN}" --path "${BENCH_ROOT}/read_random_100k_db" --read-random 100000 --seed 1

run_case scan_100k --write 100000 --value-size 100
"${BENCH_BIN}" --path "${BENCH_ROOT}/scan_100k_db" --scan 100000

run_case mixed_100k --mixed 100000 --value-size 100
