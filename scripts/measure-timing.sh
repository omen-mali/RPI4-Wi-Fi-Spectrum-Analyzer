#!/usr/bin/env bash
# Drive a timing capture on the Pi and analyze it locally.
#
# Usage: scripts/measure-timing.sh [user@host] [seconds_per_phase] [ncpu] [profile]
#        (defaults: WSA_PI from local.env, 600, 4, default)
#        profile: default (baseline, load) | followup (hr-idle, hr-load,
#                 hr-load-wpa14) | smoke (one unloaded phase, for
#                 scripts/ci-local.sh) -- see scripts/measure-remote.sh
#
# The app gets "-i WSA_IFACE" (default bcm0) and, if WSA_COUNTRY is set in
# local.env, "-c WSA_COUNTRY". Otherwise its built-in country default.
#
# Uses one multiplexed SSH connection, so an SSH password (if any) is asked
# once. sudo on the Pi is asked once at the start of the remote run.
# Results: build/measurements/<timestamp>/ (+ summary.txt), or the directory
# named by MEASURE_LOCAL_DIR if set.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/local-env.sh
. "$REPO/scripts/local-env.sh"
wsa_load_local_env
wsa_require_pi "${1:-}"
HOST="$PI"
IFACE="${WSA_IFACE:-bcm0}"
COUNTRY="${WSA_COUNTRY:-}"
# Both end up in a remote command line, so only plain names get through.
[[ "$IFACE" =~ ^[A-Za-z0-9]+$ ]] || { echo "WSA_IFACE '$IFACE' isn't an interface name" >&2; exit 2; }
[[ -z "$COUNTRY" || "$COUNTRY" =~ ^[A-Z0-9]{2}$ ]] || { echo "WSA_COUNTRY '$COUNTRY' isn't a two-letter code" >&2; exit 2; }
DUR="${2:-600}"
NCPU="${3:-4}"
PROFILE="${4:-default}"
case "$PROFILE" in default) NPHASES=2 ;; followup) NPHASES=3 ;; smoke) NPHASES=1 ;; *) echo "unknown profile $PROFILE" >&2; exit 2 ;; esac
CM=(-o ControlMaster=auto -o "ControlPath=$HOME/.ssh/cm-%C" -o ControlPersist=30m)

echo ">> Uploading measure-remote.sh to $HOST"
ssh "${CM[@]}" "$HOST" 'mkdir -p ~/bin ~/measure'   # ~/measure: tee target below
scp -q "${CM[@]}" "$REPO/scripts/measure-remote.sh" "$HOST:bin/measure-remote.sh"

echo ">> Running '$PROFILE' capture: $NPHASES phases x ${DUR}s (about $(( (NPHASES * DUR + 30) / 60 )) min)"
# Fetch whatever was captured even if the remote run ends non-zero.
# The remote output is also kept in ~/measure/last-run.out to diagnose early exits.
ssh -t "${CM[@]}" "$HOST" "WSA_IFACE=$IFACE WSA_COUNTRY=$COUNTRY sh ~/bin/measure-remote.sh $DUR $NCPU $PROFILE 2>&1 | tee ~/measure/last-run.out" \
    || echo "warning: remote capture exited with status $? -- fetching anyway" >&2

# Safety net: always put wpa_supplicant back to its image default (10r), even
# if the remote script ended before its own restore ran.
ssh "${CM[@]}" "$HOST" 'sudo -n slay -Q -f -P 10r wpa_supplicant-2.11 >/dev/null 2>&1; sudo -n pidin -p wpa_supplicant-2.11 | tail -1' \
    || echo "warning: could not verify wpa_supplicant priority -- check with: sudo pidin -p wpa_supplicant-2.11" >&2

RUN="$(ssh "${CM[@]}" "$HOST" 'ls -1d ~/measure/*/ | tail -1')"
RUN="${RUN%/}"
LOCAL="${MEASURE_LOCAL_DIR:-$REPO/build/measurements/$(basename "$RUN")}"
mkdir -p "$LOCAL"
# tar over ssh: scp with a remote wildcard failed against the QNX sshd. The
# run's contents are unpacked straight into LOCAL, whatever it's called.
echo ">> Fetching $RUN -> $LOCAL"
# The run's path is expanded here, on the client, by design.
# shellcheck disable=SC2029
ssh "${CM[@]}" "$HOST" "cd '$RUN' && tar cf - ." | tar xf - -C "$LOCAL"
ssh "${CM[@]}" "$HOST" 'cat ~/measure/last-run.out' >"$LOCAL/remote-output.txt" 2>/dev/null || true

# Phase logs in run order (by modification time).
LOGS=()
while IFS= read -r f; do LOGS+=("$f"); done < <(ls -1tr "$LOCAL"/*.log)
python3 "$REPO/scripts/analyze-timing.py" "${LOGS[@]}" | tee "$LOCAL/summary.txt"
echo ">> Summary: $LOCAL/summary.txt"
