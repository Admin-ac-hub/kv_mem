#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-week6-bench}"
RUN_ROOT="${RUN_ROOT:-${ROOT_DIR}/bench_runs/week6}"
VECTOR_COUNT="${VECTOR_COUNT:-1000000}"
DIMENSION="${DIMENSION:-768}"
QUERY_COUNT="${QUERY_COUNT:-1000}"
# Exact recall costs O(recall queries * corpus size * dimension). Set this to
# QUERY_COUNT for a complete 1,000-query recall measurement.
RECALL_QUERY_COUNT="${RECALL_QUERY_COUNT:-10}"
MIXED_OPERATIONS="${MIXED_OPERATIONS:-1000000}"
WRITE_BATCH_SIZE="${WRITE_BATCH_SIZE:-256}"
QUERY_BATCH_SIZE="${QUERY_BATCH_SIZE:-1}"
TOP_K="${TOP_K:-10}"
EF_SEARCH="${EF_SEARCH:-128}"
MEMTABLE_LIMIT="${MEMTABLE_LIMIT:-1024}"
L0_LIMIT="${L0_LIMIT:-4}"
L1_BYTES="${L1_BYTES:-67108864}"
BLOCK_CACHE="${BLOCK_CACHE:-64}"
HNSW_M="${HNSW_M:-16}"
EF_CONSTRUCTION="${EF_CONSTRUCTION:-200}"
REQUIRE_MAINTENANCE="${REQUIRE_MAINTENANCE:-0}"

# Resolve tools explicitly so a minimal launchd/CI PATH also works on Homebrew.
resolve_tool() {
  local name="$1" override="$2" candidate
  if [[ -n "${override}" ]]; then
    command -v "${override}" || { echo "tool not found: ${override}" >&2; return 1; }
    return
  fi
  if command -v "${name}" >/dev/null 2>&1; then
    command -v "${name}"
    return
  fi
  for candidate in "/opt/homebrew/bin/${name}" "/usr/local/bin/${name}"; do
    if [[ -x "${candidate}" ]]; then
      echo "${candidate}"
      return
    fi
  done
  echo "${name} not found; set CMAKE_BIN/NINJA_BIN or add it to PATH" >&2
  return 1
}
CMAKE_BIN="$(resolve_tool cmake "${CMAKE_BIN:-}")"
NINJA_BIN="$(resolve_tool ninja "${NINJA_BIN:-}")"

case "${RUN_ROOT}" in
  "${ROOT_DIR}"/bench_runs/*) ;;
  *)
    echo "RUN_ROOT must be under ${ROOT_DIR}/bench_runs" >&2
    exit 2
    ;;
esac

"${CMAKE_BIN}" -S "${ROOT_DIR}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_MAKE_PROGRAM="${NINJA_BIN}" \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
"${CMAKE_BIN}" --build "${BUILD_DIR}" --parallel --target vector_bench

# Preserve previous evidence. Use a fresh run directory for each comparison.
if [[ -e "${RUN_ROOT}" ]]; then
  echo "RUN_ROOT already exists; choose a new directory: ${RUN_ROOT}" >&2
  exit 2
fi
mkdir -p "${RUN_ROOT}"
{
  uname -a
  "${CMAKE_BIN}" --version
  "${NINJA_BIN}" --version
  git -C "${ROOT_DIR}" rev-parse HEAD
  git -C "${ROOT_DIR}" status --short
  cat "${BUILD_DIR}/CMakeCache.txt"
} > "${RUN_ROOT}/environment.txt"

COMMON_ARGS=(
  --vectors "${VECTOR_COUNT}"
  --dimension "${DIMENSION}"
  --queries "${QUERY_COUNT}"
  --recall-queries "${RECALL_QUERY_COUNT}"
  --mixed-operations "${MIXED_OPERATIONS}"
  --write-batch "${WRITE_BATCH_SIZE}"
  --query-batch "${QUERY_BATCH_SIZE}"
  --top-k "${TOP_K}"
  --ef-search "${EF_SEARCH}"
  --memtable-limit "${MEMTABLE_LIMIT}"
  --l0-limit "${L0_LIMIT}"
  --l1-bytes "${L1_BYTES}"
  --block-cache "${BLOCK_CACHE}"
  --hnsw-m "${HNSW_M}"
  --ef-construction "${EF_CONSTRUCTION}"
  --reset
)
if [[ "${REQUIRE_MAINTENANCE}" == 1 ]]; then
  COMMON_ARGS+=(--require-maintenance)
fi

run_case() {
  local mode="$1"
  echo "== ${mode} =="
  "${BUILD_DIR}/vector_bench" --mode "${mode}" \
    --path "${RUN_ROOT}/${mode}" "${COMMON_ARGS[@]}" \
    | tee "${RUN_ROOT}/${mode}.txt"
}

run_case write
run_case query
run_case mixed

echo "results: ${RUN_ROOT}" >&2
