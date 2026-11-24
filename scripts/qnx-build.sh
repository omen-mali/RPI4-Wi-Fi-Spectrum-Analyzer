#!/usr/bin/env bash
# Cross-compile wifi_spectrum_analyzer for QNX 8.0 / aarch64 inside a
# linux/amd64 container. Prefers Apple's `container` CLI and falls back to Docker.
#
# Usage:
#   scripts/qnx-build.sh            # configure + build into build/qnx
#   scripts/qnx-build.sh clean      # remove build/qnx first
#   scripts/qnx-build.sh shell      # interactive shell with the SDP env loaded
#   scripts/qnx-build.sh rebuild-image
#
# Environment overrides (QNX_SDP_DIR and QNX_CONFIG_DIR can also be set in
# local.env, see local.env.example):
#   QNX_SDP_DIR        host SDP install       (default: ~/qnx800)
#   QNX_CONFIG_DIR     host license/config    (default: ~/.qnx)
#   CONTAINER_RUNTIME  container | docker     (default: auto-detect)
#   QNX_BUILD_IMAGE    image tag              (default: qnx800-build:latest)
#   BUILD_TYPE         CMake build type       (default: RelWithDebInfo)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/local-env.sh
. "$REPO/scripts/local-env.sh"
wsa_load_local_env
SDP="${QNX_SDP_DIR:-$HOME/qnx800}"
QNXCFG="${QNX_CONFIG_DIR:-$HOME/.qnx}"
IMAGE="${QNX_BUILD_IMAGE:-qnx800-build:latest}"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
RUNTIME="${CONTAINER_RUNTIME:-}"
ACTION="${1:-build}"

die() { echo "error: $*" >&2; exit 1; }

[ -f "$SDP/qnxsdp-env.sh" ] || die "QNX SDP not found at $SDP (set QNX_SDP_DIR)"
[ -d "$QNXCFG" ]            || die "QNX license dir not found at $QNXCFG (set QNX_CONFIG_DIR)"

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
    docker)
        PLATFORM=(--platform linux/amd64)
        ;;
    *) die "unsupported CONTAINER_RUNTIME '$RUNTIME'" ;;
esac

build_image() {
    echo ">> Building $IMAGE with $RUNTIME (linux/amd64)"
    "$RUNTIME" build "${PLATFORM[@]}" -t "$IMAGE" \
        -f "$REPO/docker/Containerfile" "$REPO/docker"
}

if [ "$ACTION" = rebuild-image ] || ! "$RUNTIME" image inspect "$IMAGE" >/dev/null 2>&1; then
    build_image
    [ "$ACTION" = rebuild-image ] && exit 0
fi

# The SDP mount isn't written to (qnxsdp-env.sh treats it as read-only).
RUN=("$RUNTIME" run --rm "${PLATFORM[@]}"
     -v "$SDP:/opt/qnx800"
     -v "$QNXCFG:/root/.qnx"
     -v "$REPO:/work"
     -w /work)

# (no ';&' fall-through: macOS ships bash 3.2)
if [ "$ACTION" = clean ]; then
    rm -rf "$REPO/build/qnx"
    ACTION=build
fi

case "$ACTION" in
    shell)
        exec "${RUN[@]}" -it "$IMAGE" bash
        ;;
    build)
        exec "${RUN[@]}" "$IMAGE" bash -c "
            set -e
            cmake -S . -B build/qnx -G Ninja \
                  -DCMAKE_TOOLCHAIN_FILE=cmake/qnx_aarch64.toolchain.cmake \
                  -DCMAKE_BUILD_TYPE=$BUILD_TYPE
            cmake --build build/qnx
            file build/qnx/wifi_spectrum_analyzer"
        ;;
    *) die "unknown action '$ACTION' (build|clean|shell|rebuild-image)" ;;
esac
