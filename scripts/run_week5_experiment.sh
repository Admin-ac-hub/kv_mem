#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-week5-bench}"
OPERATIONS="${OPERATIONS:-100000}"
SYNC_OPERATIONS="${SYNC_OPERATIONS:-${OPERATIONS}}"
THREADS="${THREADS:-4}"
SYNC_THREADS="${SYNC_THREADS:-${THREADS}}"
BATCH_SIZE="${BATCH_SIZE:-32}"
VALUE_SIZE="${VALUE_SIZE:-100}"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build "${BUILD_DIR}" --parallel --target week5_bench

RUN_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/mini-lsm-kv-week5.XXXXXX")"
echo "# temporary benchmark data: ${RUN_ROOT}" >&2
echo "# mode,operations,threads,batch_size,elapsed_sec,ops_per_sec,ns_per_op,lock_free,counter"
run_bench() {
  "${BUILD_DIR}/week5_bench" "$@" | tail -n +2
}

run_bench --mode sync --operations "${SYNC_OPERATIONS}" \
  --threads "${SYNC_THREADS}" --batch-size "${BATCH_SIZE}"
run_bench --mode db-put --operations "${OPERATIONS}" \
  --value-size "${VALUE_SIZE}" --path "${RUN_ROOT}/db-put"
run_bench --mode db-batch --operations "${OPERATIONS}" \
  --batch-size 1 --value-size "${VALUE_SIZE}" --path "${RUN_ROOT}/db-batch-1"
run_bench --mode db-batch --operations "${OPERATIONS}" \
  --batch-size "${BATCH_SIZE}" --value-size "${VALUE_SIZE}" \
  --path "${RUN_ROOT}/db-batch-${BATCH_SIZE}"

if command -v perf >/dev/null 2>&1; then
  echo "# perf is available; run perf stat against week5_bench for hardware counters." >&2
else
  echo "# perf is unavailable on this host; macOS users can use scripts/profile_week5.sh." >&2
fi
