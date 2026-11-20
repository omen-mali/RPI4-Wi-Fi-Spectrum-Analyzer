#!/usr/bin/env bash
# Copy the cross-built analyzer (and libwpactrl, if linked dynamically) to
# the Pi, and install a launcher that sets LD_LIBRARY_PATH.
#
# The QNX 8.0.5 Quick Start image does not include libwpactrl, so a
# dynamically linked build needs libwpactrl-2.11.so.1 next to it (~/lib).
#
# Usage:
#   scripts/deploy.sh [user@host]      (default: qnxuser@qnx-wifi-analyzer.local)
# Then on the Pi:
#   sudo ~/bin/wifi-analyzer [-i bcm0] [-c CA]
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDP="${QNX_SDP_DIR:-$HOME/qnx800}"
TARGET="${1:-qnxuser@qnx-wifi-analyzer.local}"
BUILD="$REPO/build/qnx"
BIN="$BUILD/wifi_spectrum_analyzer"

die() { echo "error: $*" >&2; exit 1; }

# One multiplexed SSH connection (same ControlPath as measure-timing.sh), so a
# password is asked once and reused by a measurement run started right after.
CM=(-o ControlMaster=auto -o "ControlPath=$HOME/.ssh/cm-%C" -o ControlPersist=30m)

[ -f "$BIN" ] || die "$BIN not found -- run scripts/qnx-build.sh first"

# Which libwpactrl did CMake link? (cache paths are container paths under /opt/qnx800)
WPALIB="$(sed -n 's/^WPACTRL_LIBRARY:FILEPATH=//p' "$BUILD/CMakeCache.txt" 2>/dev/null || true)"
WPALIB_HOST="${WPALIB/#\/opt\/qnx800/$SDP}"

LAUNCHER="$(mktemp)"
trap 'rm -f "$LAUNCHER"' EXIT
cat >"$LAUNCHER" <<'EOF'
#!/bin/sh
# Launcher installed by scripts/deploy.sh. Run as root: sudo ~/bin/wifi-analyzer [-i bcm0] [-c CA]
# (sudo strips LD_LIBRARY_PATH, so it's set here, after privilege change.)
here=$(cd "$(dirname "$0")" && pwd)
LD_LIBRARY_PATH="$here/../lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LD_LIBRARY_PATH
exec "$here/wifi_spectrum_analyzer" "$@"
EOF

echo ">> Deploying to $TARGET"
ssh "${CM[@]}" "$TARGET" 'mkdir -p ~/bin ~/lib'
scp "${CM[@]}" "$BIN" "$LAUNCHER" "$TARGET:bin/"
ssh "${CM[@]}" "$TARGET" "mv ~/bin/$(basename "$LAUNCHER") ~/bin/wifi-analyzer && chmod 755 ~/bin/wifi-analyzer ~/bin/wifi_spectrum_analyzer"

case "$WPALIB_HOST" in
    *.so*)
        # The binary's DT_NEEDED is the soname (libwpactrl-2.11.so.1), as the
        # unversioned .so symlink is link-time only and .so.1.sym is debug info.
        LIBDIR="$(dirname "$WPALIB_HOST")"
        SONAME_FILE="$LIBDIR/libwpactrl-2.11.so.1"
        [ -f "$SONAME_FILE" ] || die "$SONAME_FILE not found"
        echo ">> Copying $(basename "$SONAME_FILE") to ~/lib"
        scp "${CM[@]}" "$SONAME_FILE" "$TARGET:lib/"
        ;;
    *.a)
        echo ">> libwpactrl is statically linked; nothing else to copy"
        ;;
    *)
        echo "warning: couldn't tell how libwpactrl was linked (no CMakeCache entry)" >&2
        ;;
esac

echo ">> Done. On the Pi:  sudo ~/bin/wifi-analyzer -i bcm0 -c CA"
