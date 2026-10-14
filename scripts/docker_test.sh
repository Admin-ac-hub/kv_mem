#!/usr/bin/env bash
# Build and test this repository entirely inside a container.
#
#   scripts/docker_test.sh build [target...]   configure + build (incremental)
#   scripts/docker_test.sh ctest [args...]     run the CTest suite
#   scripts/docker_test.sh run <cmd...>        run an arbitrary command in /work
#   scripts/docker_test.sh image               (re)build the toolchain image
#   scripts/docker_test.sh shell               interactive shell in /work
#
# The container is linux/arm64, matching the development host, so the distance
# kernel selects NEON. Source is bind-mounted read-only at /src; build output
# lives in the named volume kv-test-build, which survives across invocations and
# keeps object files off the macOS bind mount.

set -euo pipefail

DOCKER="${DOCKER:-/usr/local/bin/docker}"
if ! command -v "${DOCKER}" >/dev/null 2>&1; then
  DOCKER="$(command -v docker || true)"
fi
if [[ -z "${DOCKER}" ]]; then
  echo "docker not found; set DOCKER=/path/to/docker" >&2
  exit 1
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${KV_TEST_IMAGE:-kv-test:ubuntu24}"
BUILD_VOLUME="${KV_TEST_BUILD_VOLUME:-kv-test-build}"
DOCKERFILE="${REPO_ROOT}/docker/Dockerfile.test"
BUILD_TYPE="${KV_TEST_BUILD_TYPE:-Release}"
VECTOR_EXTENSION="${KV_TEST_VECTOR_EXTENSION:-OFF}"

run_in_container() {
  "${DOCKER}" run --rm \
    -v "${REPO_ROOT}":/src:ro \
    -v "${BUILD_VOLUME}":/work \
    -w /work \
    -e CTEST_OUTPUT_ON_FAILURE=1 \
    "${IMAGE}" \
    "$@"
}

ensure_image() {
  if ! "${DOCKER}" image inspect "${IMAGE}" >/dev/null 2>&1; then
    echo "== building toolchain image ${IMAGE} =="
    # The classic builder is used deliberately: on this host buildx fails with
    # "failed to update builder last activity time" because the sandbox denies
    # writes under ~/.docker/buildx/activity. The classic builder does not touch
    # that path and produces an equivalent image.
    DOCKER_BUILDKIT=0 "${DOCKER}" build -f "${DOCKERFILE}" -t "${IMAGE}" "${REPO_ROOT}"
  fi
}

configure_and_build() {
  run_in_container bash -lc "
    set -euo pipefail
    if [[ ! -f /work/CMakeCache.txt ]]; then
      echo '== configure =='
      cmake -S /src -B /work -G Ninja -DCMAKE_BUILD_TYPE=${BUILD_TYPE} -DBUILD_TESTING=ON -DKV_ENABLE_VECTOR_EXTENSION=${VECTOR_EXTENSION}
    else
      echo '== reconfigure (existing cache) =='
      cmake -S /src -B /work -DKV_ENABLE_VECTOR_EXTENSION=${VECTOR_EXTENSION}
    fi
    echo '== build =='
    if [[ \$# -eq 0 ]]; then
      cmake --build /work --parallel
    else
      cmake --build /work --parallel --target \"\$@\"
    fi
  " -- "$@"
}

case "${1:-}" in
  image)
    DOCKER_BUILDKIT=0 "${DOCKER}" build -f "${DOCKERFILE}" -t "${IMAGE}" "${REPO_ROOT}"
    ;;
  build)
    ensure_image
    shift || true
    configure_and_build "$@"
    ;;
  ctest)
    ensure_image
    shift || true
    run_in_container bash -lc "cd /work && ctest --output-on-failure $*"
    ;;
  run)
    ensure_image
    shift
    run_in_container bash -lc "cd /work && $*"
    ;;
  shell)
    ensure_image
    "${DOCKER}" run --rm -it \
      -v "${REPO_ROOT}":/src:ro -v "${BUILD_VOLUME}":/work -w /work \
      "${IMAGE}" /bin/bash
    ;;
  *)
    sed -n '2,16p' "${BASH_SOURCE[0]}"
    exit 2
    ;;
esac
