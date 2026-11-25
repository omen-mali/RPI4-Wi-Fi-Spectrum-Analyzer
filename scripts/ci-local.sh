#!/usr/bin/env bash
# The hardware half of CI, run locally: build the QNX binary, run a short
# smoke test on the Pi, and post the results as commit statuses on HEAD.
#
# Usage:
#   scripts/ci-local.sh [--no-pi] [--pi user@host] [--seconds N] [--release] [--dry-run]
#     --no-pi       build only, and post nothing for pi/smoke
#     --pi HOST     the Pi as user@host (default: WSA_PI from local.env)
#     --seconds N   length of the smoke run, at least 10 (default: 60)
#     --release     when HEAD is a pushed vX.Y.Z tag, attach the QNX binary to
#                   that GitHub Release, with its CHANGELOG.md section as notes.
#                   The QNX non-commercial license allows handing out binaries
#                   only as demonstrations whose users accept the license's
#                   demonstration EULA (Exhibit B), so the EULA text is
#                   attached too: set WSA_QNX_DEMO_EULA in local.env to a file
#                   holding it (see README, "License").
#     --dry-run     build and run everything, but post no statuses and publish
#                   nothing: print what would be posted, and leave the release
#                   assets and notes in build/ci/<sha>/. The commit and tag
#                   don't have to be on GitHub, and gh needn't be logged in.
#
# Statuses (context: meaning):
#   qnx/build  scripts/qnx-build.sh clean succeeded, the description gives the
#              binary's size and the warning count
#   pi/smoke   the app ran on the Pi and passed scripts/smoke-check.py with
#              the limits in scripts/ci-thresholds.env. "error" means the Pi
#              couldn't be reached or the run timed out, not that it failed.
#
# Run it from a clean checkout of a commit that's on GitHub: the statuses are
# posted on HEAD's SHA, with your existing gh login. You type the SSH and sudo
# passwords for the Pi when they're asked for. The full report goes to
# build/ci/<sha>/, which isn't committed. Setup-specific settings (the Pi,
# the country, the SDP location) come from local.env, see local.env.example.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"
# shellcheck source=scripts/ci-thresholds.env
. "$REPO/scripts/ci-thresholds.env"
# shellcheck source=scripts/local-env.sh
. "$REPO/scripts/local-env.sh"
wsa_load_local_env

PI_ARG=""
RUN_SECS=60
NO_PI=0
RELEASE=0
DRY_RUN=0
BIN=build/qnx/wifi_spectrum_analyzer
CM=(-o ControlMaster=auto -o "ControlPath=$HOME/.ssh/cm-%C" -o ControlPersist=30m)

die() { echo "error: $*" >&2; exit 1; }
usage() { sed -n '2,/^set -euo/{/^set -euo/d;s/^# \{0,1\}//;p;}' "$0"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --no-pi)   NO_PI=1 ;;
        --pi)      [ $# -ge 2 ] || die "--pi needs user@host"; PI_ARG="$2"; shift ;;
        --seconds) [ $# -ge 2 ] || die "--seconds needs a number"; RUN_SECS="$2"; shift ;;
        --release) RELEASE=1 ;;
        --dry-run) DRY_RUN=1 ;;
        -h|--help) usage; exit 0 ;;
        *)         die "unknown option '$1' (see --help)" ;;
    esac
    shift
done
case "$RUN_SECS" in ''|*[!0-9]*) die "--seconds must be a whole number" ;; esac
[ "$RUN_SECS" -ge 10 ] || die "--seconds must be at least 10"
[ "$NO_PI" = 1 ] || wsa_require_pi "$PI_ARG"

# --- Refuse to run when the statuses couldn't describe HEAD --------------------
# Untracked files are allowed: the build only uses files CMakeLists.txt names.
# A dry run posts nothing, so what GitHub has only warrants a warning there.
github_needs() {  # message
    if [ "$DRY_RUN" = 1 ]; then
        echo "warning (dry run): $1" >&2
    else
        die "$1"
    fi
}
[ -z "$(git status --porcelain --untracked-files=no)" ] \
    || die "uncommitted changes: commit or stash first (statuses are posted on HEAD's SHA)"
gh auth status >/dev/null 2>&1 || github_needs "gh isn't logged in: run 'gh auth login'"
SHA="$(git rev-parse HEAD)"
[ -n "$(git branch -r --contains "$SHA" 2>/dev/null)" ] \
    || github_needs "push this commit first: GitHub has no commit to attach a status to"

if [ "$DRY_RUN" = 1 ]; then
    SLUG="$(gh repo view --json nameWithOwner -q .nameWithOwner 2>/dev/null || echo '<owner>/<repo>')"
    echo ">> Dry run: nothing is posted or published"
else
    SLUG="$(gh repo view --json nameWithOwner -q .nameWithOwner)"
fi
OUT="$REPO/build/ci/$SHA"
mkdir -p "$OUT"
: >"$OUT/statuses.txt"

# --release is checked before anything is built, so a bad tag fails fast.
if [ "$RELEASE" = 1 ]; then
    TAG="$(git describe --exact-match --tags HEAD 2>/dev/null)" \
        || die "--release: HEAD isn't a tag"
    [[ "$TAG" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "--release: tag $TAG isn't vX.Y.Z"
    REMOTE_SHA="$(git ls-remote origin "refs/tags/$TAG^{}" | cut -f1)"
    [ -n "$REMOTE_SHA" ] || REMOTE_SHA="$(git ls-remote origin "refs/tags/$TAG" | cut -f1)"
    [ "$REMOTE_SHA" = "$SHA" ] || github_needs "--release: push tag $TAG first (GitHub doesn't have it on this commit)"
    NOTES="$OUT/release-notes.md"
    [ -f CHANGELOG.md ] || die "--release: CHANGELOG.md not found"
    # The section runs to the next "## " heading; link definitions ("[x]: url") are left out.
    awk -v h="## [${TAG#v}]" 'index($0, h) == 1 {f = 1; next} f && /^## / {exit} f && /^\[[^]]*\]: / {next} f' \
        CHANGELOG.md >"$NOTES"
    grep -q '[^[:space:]]' "$NOTES" || die "--release: CHANGELOG.md has no '## [${TAG#v}]' section"
    EULA="$(wsa_local_path "${WSA_QNX_DEMO_EULA:-}")"
    [ -n "$EULA" ] || die "--release: set WSA_QNX_DEMO_EULA in local.env to the QNX demonstration EULA text (see README, \"License\")"
    [ -s "$EULA" ] || die "--release: WSA_QNX_DEMO_EULA=$EULA is missing or empty"
    ASSET_NAME="wifi_spectrum_analyzer-$TAG-qnx800-aarch64le"
    cat >>"$NOTES" <<NOTICE

---

**About the binary.** \`$ASSET_NAME\` was built with the QNX SDP 8.0 under a
non-commercial QNX license. It's a demonstration build for non-commercial evaluation
only, and using it means accepting the QNX demonstration EULA attached to this release
(\`QNX-DEMO-EULA.txt\`). It needs a QNX Neutrino 8.0 target, which is licensed
separately. The source code is under the Apache License 2.0, however, the QNX software
it is built with and runs on isn't.
NOTICE
fi

# --- Statuses ---------------------------------------------------------------
PENDING=" "
WATCHDOG=""
CHILD=""

post() {  # context state description
    local desc="${3//$HOME/~}"
    desc="${desc:0:140}"
    if [ "$DRY_RUN" = 1 ]; then
        echo ">> [dry run, not posted] $1: $2 ($desc)"
    elif gh api -X POST "repos/$SLUG/statuses/$SHA" -f state="$2" -f context="$1" \
            -f description="$desc" >/dev/null; then
        echo ">> $1: $2 ($desc)"
    else
        echo "warning: couldn't post $1=$2" >&2
    fi
    printf '%s\t%s\t%s\n' "$1" "$2" "$desc" >>"$OUT/statuses.txt"
    if [ "$2" = pending ]; then
        PENDING="$PENDING$1 "
    else
        PENDING="${PENDING// $1 / }"
    fi
}

on_exit() {
    local rc=$?
    if [ -n "$CHILD" ]; then
        pkill -TERM -P "$CHILD" 2>/dev/null || true
        kill -TERM "$CHILD" 2>/dev/null || true
    fi
    if [ -n "$WATCHDOG" ]; then
        pkill -P "$WATCHDOG" 2>/dev/null || true
        kill "$WATCHDOG" 2>/dev/null || true
    fi
    local ctx
    for ctx in $PENDING; do
        post "$ctx" error "ci-local interrupted"
    done
    echo ">> Report: $OUT"
    exit "$rc"
}
trap on_exit EXIT
trap 'exit 130' INT TERM HUP

# Runs a command with a time limit; returns 124 if the limit hit. macOS has no
# timeout(1), so a background watchdog kills the command and its children.
# Its stdin is passed on explicitly: a background job's stdin is otherwise
# /dev/null, and the SSH and sudo password prompts need the terminal.
run_with_timeout() {  # seconds command...
    local secs="$1" pid rc=0
    shift
    rm -f "$OUT/.timed-out"
    "$@" 0<&0 &
    pid=$!
    CHILD=$pid
    ( sleep "$secs"; touch "$OUT/.timed-out"; pkill -TERM -P "$pid"; kill -TERM "$pid" ) \
        2>/dev/null &
    WATCHDOG=$!
    wait "$pid" || rc=$?
    CHILD=""
    pkill -P "$WATCHDOG" 2>/dev/null || true
    kill "$WATCHDOG" 2>/dev/null || true
    wait "$WATCHDOG" 2>/dev/null || true
    WATCHDOG=""
    if [ -e "$OUT/.timed-out" ]; then
        rm -f "$OUT/.timed-out"
        return 124
    fi
    return "$rc"
}

# --- qnx/build --------------------------------------------------------------
post qnx/build pending "building"
echo ">> scripts/qnx-build.sh clean (output: $OUT/qnx-build.txt)"
if scripts/qnx-build.sh clean >"$OUT/qnx-build.txt" 2>&1; then
    size="$(wc -c <"$BIN" | tr -d ' ')"
    warnings="$(grep -c 'warning:' "$OUT/qnx-build.txt" || true)"
    post qnx/build success "$size bytes, $warnings warnings"
else
    tail -20 "$OUT/qnx-build.txt" >&2
    err="$(grep -i 'error' "$OUT/qnx-build.txt" | tail -1 || true)"
    post qnx/build failure "${err:-scripts/qnx-build.sh failed}"
    exit 1
fi

# --- pi/smoke ---------------------------------------------------------------
SMOKE=skipped
if [ "$NO_PI" = 0 ]; then
    post pi/smoke pending "running on the Pi"
    if ! ssh -o BatchMode=no -o ConnectTimeout=5 "${CM[@]}" "$PI" true; then
        post pi/smoke error "Pi unreachable"
        exit 0
    fi
    if ! scripts/deploy.sh "$PI" 2>&1 | tee "$OUT/deploy.txt"; then
        post pi/smoke failure "deploy.sh failed"
        exit 1
    fi
    # measure-timing.sh puts wpa_supplicant's priority back itself.
    rc=0
    run_with_timeout "$CI_PI_TIMEOUT" \
        env MEASURE_LOCAL_DIR="$OUT/pi" scripts/measure-timing.sh "$PI" "$RUN_SECS" 0 smoke || rc=$?
    if [ "$rc" = 124 ]; then
        post pi/smoke error "Pi run timed out after ${CI_PI_TIMEOUT}s"
        exit 1
    fi
    python3 scripts/analyze-timing.py --json "$OUT/pi/smoke.log" >"$OUT/smoke.json" 2>/dev/null || true
    verdict="$(python3 scripts/smoke-check.py --seconds "$RUN_SECS" --period-ms "$CI_PERIOD_MS" \
        --p99-max-ms "$CI_WAKE_P99_MAX_MS" --min-aps "$CI_MIN_APS" "$OUT/pi/smoke.log")" || true
    echo "$verdict" >"$OUT/smoke-verdict.txt"
    case "$verdict" in
        "PASS "*) post pi/smoke success "${verdict#PASS }"; SMOKE=success ;;
        *)        post pi/smoke failure "${verdict#FAIL }"; SMOKE=failure ;;
    esac
fi

# --- release ----------------------------------------------------------------
if [ "$RELEASE" = 1 ]; then
    # A binary that failed on the Pi isn't published. --no-pi skips the check.
    [ "$SMOKE" != failure ] || die "--release: pi/smoke failed, not publishing"
    ASSET="$OUT/$ASSET_NAME"
    EULA_ASSET="$OUT/QNX-DEMO-EULA.txt"
    cp "$BIN" "$ASSET"
    cp "$EULA" "$EULA_ASSET"
    if [ "$DRY_RUN" = 1 ]; then
        echo ">> [dry run, not published] would run one of:"
        echo "     gh release create $TAG $ASSET_NAME QNX-DEMO-EULA.txt --verify-tag --title $TAG --notes-file release-notes.md"
        echo "     gh release upload $TAG $ASSET_NAME QNX-DEMO-EULA.txt --clobber   (if the release exists)"
        echo ">> Assets and notes: $OUT"
    elif gh release view "$TAG" >/dev/null 2>&1; then
        gh release upload "$TAG" "$ASSET" "$EULA_ASSET" --clobber
    else
        gh release create "$TAG" "$ASSET" "$EULA_ASSET" --verify-tag --title "$TAG" \
            --notes-file "$NOTES"
    fi
    [ "$DRY_RUN" = 1 ] || echo ">> Released $TAG: $ASSET_NAME and QNX-DEMO-EULA.txt"
fi

[ "$SMOKE" != failure ]
