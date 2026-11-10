#!/usr/bin/env bash
set -euo pipefail
# Run against an already configured Linux build; no containers or data are replaced.
BUILD_DIR="${1:-build-rpc}"
if [[ ! -x "${BUILD_DIR}/rpc/kv_cluster_server" ]]; then
  echo "Build with KV_ENABLE_RPC=ON first, then pass the Linux build directory." >&2
  exit 2
fi
ctest --test-dir "${BUILD_DIR}" -L raft --no-tests=error --output-on-failure
