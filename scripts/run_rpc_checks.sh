#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
RPC_SOURCE="$(cd "${ROOT_DIR}/../miniRPC" && pwd -P)"
CONTAINER=minirpc-linux

if ! docker info >/dev/null 2>&1; then
  echo 'Docker daemon is unavailable. Start Docker and rerun.' >&2
  exit 1
fi
MOUNT_SOURCE="$(docker inspect "${CONTAINER}" --format '{{range .Mounts}}{{if eq .Destination "/work"}}{{.Source}}{{end}}{{end}}')"
if [[ "${MOUNT_SOURCE}" != "${RPC_SOURCE}" ]]; then
  echo "Expected ${CONTAINER} to mount ${RPC_SOURCE} at /work; check container configuration." >&2
  exit 1
fi
docker start "${CONTAINER}" >/dev/null
docker exec "${CONTAINER}" test -f /work/build/CMakeCache.txt
docker exec "${CONTAINER}" grep -q 'CMAKE_CXX_COMPILER:FILEPATH=/usr/' /work/build/CMakeCache.txt
RUN_DIR="$(docker exec "${CONTAINER}" mktemp -d /tmp/kv-rpc-check.XXXXXX)"
printf 'Linux build directory: %s:%s\n' "${CONTAINER}" "${RUN_DIR}"

docker exec "${CONTAINER}" cmake --build /work/build --parallel 4
docker exec "${CONTAINER}" cmake --install /work/build --prefix "${RUN_DIR}/minirpc"
COPYFILE_DISABLE=1 tar --no-xattrs --exclude='._*' --exclude='test/test_dbs' \
  -C "${ROOT_DIR}" -cf - CMakeLists.txt cmake include src bench stress test rpc | \
  docker exec -i "${CONTAINER}" tar -xf - -C "${RUN_DIR}"
docker exec "${CONTAINER}" cmake -S "${RUN_DIR}" -B "${RUN_DIR}/build" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Debug}" -DBUILD_TESTING=ON -DKV_ENABLE_RPC=ON \
  -DKV_SANITIZERS="${KV_SANITIZERS:-}" -DCMAKE_PREFIX_PATH="${RUN_DIR}/minirpc"
docker exec "${CONTAINER}" cmake --build "${RUN_DIR}/build" --parallel 4
docker exec "${CONTAINER}" ctest --test-dir "${RUN_DIR}/build" --output-on-failure
printf '\nRun in two terminals (paths are inside the existing container):\n'
printf 'docker exec -it %s %s/build/rpc/kv_server %s/data 127.0.0.1 19000\n' "${CONTAINER}" "${RUN_DIR}" "${RUN_DIR}"
printf 'docker exec %s %s/build/rpc/kv_client 127.0.0.1 19000 put hello world\n' "${CONTAINER}" "${RUN_DIR}"
printf 'docker exec %s %s/build/rpc/kv_client 127.0.0.1 19000 get hello\n' "${CONTAINER}" "${RUN_DIR}"
