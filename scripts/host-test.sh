#!/usr/bin/env bash
# Build and run the host unit tests (CTest).
#
# Usage:
#   scripts/host-test.sh [gcc|clang] [asan|tsan|none]     (defaults: gcc none)
#
# On Linux it runs natively and needs cmake, ninja and the chosen compiler.
# On macOS it re-runs itself inside the linux/amd64 build image
# (docker/Containerfile), without the QNX SDP: the host build needs
# clock_nanosleep, which macOS lacks. Each compiler/sanitizer pair gets its
# own build directory, build/host-<cc>-<san>. tsan requires native x86-64
# Linux (see below).
#
# Environment overrides (macOS only):
#   CONTAINER_RUNTIME  container | docker     (default: auto-detect)
#   QNX_BUILD_IMAGE    image tag              (default: qnx800-build:latest)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CC_NAME="${1:-gcc}"
SAN="${2:-none}"

die() { echo "error: $*" >&2; exit 1; }

case "$CC_NAME" in gcc|clang) ;; *) die "compiler must be gcc or clang (got '$CC_NAME')" ;; esac
case "$SAN" in asan|tsan|none) ;; *) die "sanitizer must be asan, tsan or none (got '$SAN')" ;; esac

if [ "$(uname -s)" = Darwin ]; then
    # ThreadSanitizer aborts at startup under Rosetta's x86-64 emulation
    # ("memory layout is incompatible, even though ASLR is disabled"), even
    # for an empty program. It runs in the hosted CI (native x86-64 Linux).
    if [ "$SAN" = tsan ]; then
        die "tsan can't run in the linux/amd64 container on macOS (Rosetta); it runs in the hosted CI's sanitizers job"
    fi
    IMAGE="${QNX_BUILD_IMAGE:-qnx800-build:latest}"
    RUNTIME="${CONTAINER_RUNTIME:-}"
    if [ -z "$RUNTIME" ]; then
        if command -v container >/dev/null 2>&1; then
            RUNTIME=container
        elif command -v docker >/dev/null 2>&1; then
            RUNTIME=docker
        else
            die "neither Apple 'container' nor 'docker' found"
        fi
    fi
    case "$RUNTIME" in
        container)
            PLATFORM=(--arch amd64)
            container system status >/dev/null 2>&1 || container system start
            ;;
        docker) PLATFORM=(--platform linux/amd64) ;;
        *) die "unsupported CONTAINER_RUNTIME '$RUNTIME'" ;;
    esac
    # Rebuild the image if it's missing or predates the host compilers and
    # clang's sanitizer runtimes. The probe is single-quoted so it expands
    # inside the container, not here.
    # shellcheck disable=SC2016
    if ! "$RUNTIME" run --rm "${PLATFORM[@]}" "$IMAGE" sh -c \
            'command -v gcc && command -v clang &&
             test -e "$(clang -print-resource-dir)/lib/linux/libclang_rt.tsan-x86_64.a"' \
            >/dev/null 2>&1; then
        echo ">> Building $IMAGE with $RUNTIME (linux/amd64)"
        "$RUNTIME" build "${PLATFORM[@]}" -t "$IMAGE" \
            -f "$REPO/docker/Containerfile" "$REPO/docker"
    fi
    exec "$RUNTIME" run --rm "${PLATFORM[@]}" -v "$REPO:/work" -w /work \
        "$IMAGE" scripts/host-test.sh "$CC_NAME" "$SAN"
fi

BUILD="build/host-$CC_NAME-$SAN"
cd "$REPO"
cmake -S . -B "$BUILD" -G Ninja \
      -DWIFI_HOST_TESTS=ON \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_COMPILER="$CC_NAME" \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      -DWSA_SANITIZE="$SAN"
cmake --build "$BUILD"
ctest --test-dir "$BUILD" --output-on-failure
