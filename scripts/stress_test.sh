#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
STRESS_BIN="${BUILD_DIR}/kv_stress"
STRESS_DB_PATH="${STRESS_DB_PATH:-${ROOT_DIR}/stress_runs/stress_db}"

EPOCHS="${EPOCHS:-10}"
THREADS="${THREADS:-4}"
OPS_PER_THREAD="${OPS_PER_THREAD:-2000}"
KEYS_PER_THREAD="${KEYS_PER_THREAD:-1000}"
VALUE_SIZE="${VALUE_SIZE:-100}"
DELETE_PERCENT="${DELETE_PERCENT:-20}"
BATCH_SIZE="${BATCH_SIZE:-1}"
REOPEN_EVERY_EPOCHS="${REOPEN_EVERY_EPOCHS:-2}"
COMPACT_EVERY_EPOCHS="${COMPACT_EVERY_EPOCHS:-3}"
SEED="${SEED:-1}"
MEMTABLE_LIMIT="${MEMTABLE_LIMIT:-128}"
L0_LIMIT="${L0_LIMIT:-2}"

if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
  cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}"
  cmake --build "${BUILD_DIR}" --target kv_stress
fi

if [[ ! -x "${STRESS_BIN}" ]]; then
  echo "kv_stress binary not found: ${STRESS_BIN}" >&2
  exit 1
fi

"${STRESS_BIN}" \
  --path "${STRESS_DB_PATH}" \
  --epochs "${EPOCHS}" \
  --threads "${THREADS}" \
  --ops-per-thread "${OPS_PER_THREAD}" \
  --keys-per-thread "${KEYS_PER_THREAD}" \
  --value-size "${VALUE_SIZE}" \
  --delete-percent "${DELETE_PERCENT}" \
  --batch-size "${BATCH_SIZE}" \
  --reopen-every-epochs "${REOPEN_EVERY_EPOCHS}" \
  --compact-every-epochs "${COMPACT_EVERY_EPOCHS}" \
  --seed "${SEED}" \
  --memtable-limit "${MEMTABLE_LIMIT}" \
  --l0-limit "${L0_LIMIT}" \
  "$@"
