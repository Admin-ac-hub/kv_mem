#!/usr/bin/env bash
# Run independently of timed benchmarks. Reconfigure every build's test list.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CHECK_LOG_ROOT="${1:-${PROJECT_ROOT}/build-group-checks-logs}"
mkdir -p "${CHECK_LOG_ROOT}"

for configuration in debug release asan tsan; do
  build_type=Debug
  sanitizers=""
  case "${configuration}" in
    release) build_type=Release ;;
    asan) sanitizers=address,undefined ;;
    tsan) sanitizers=thread ;;
  esac
  build_dir="${PROJECT_ROOT}/build-group-kv-${configuration}"
  {
    cmake -S "${PROJECT_ROOT}" -B "${build_dir}" \
      -DCMAKE_BUILD_TYPE="${build_type}" -DBUILD_TESTING=ON \
      -DKV_SANITIZERS="${sanitizers}"
    cmake --build "${build_dir}" --parallel 6
    ctest --test-dir "${build_dir}" --output-on-failure --parallel 4
  } 2>&1 | tee "${CHECK_LOG_ROOT}/kv-${configuration}.txt"
done
