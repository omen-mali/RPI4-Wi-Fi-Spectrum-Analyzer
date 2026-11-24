#!/bin/sh
# Runs on the Pi (QNX ksh). Captures analyzer logs for timing analysis.
#
# Usage: sh measure-remote.sh [seconds_per_phase] [ncpu] [profile]
#
# Profiles (phase = name / CPU load / wpa_supplicant priority / app args):
#   default   baseline        no load   (unchanged)  -
#             load            NCPU hogs (unchanged)  -
#   followup  hr-idle         no load   (unchanged)  -T 1000
#             hr-load         NCPU hogs (unchanged)  -T 1000
#             hr-load-wpa14   NCPU hogs 14r          -T 1000
#
# "NCPU hogs" are busy loops at the user's default priority (10 RR): below the
# scanner (FIFO 15) and equal to the analyzer thread and wpa_supplicant (RR 10).
# A changed wpa_supplicant priority is restored to WPA_DEFAULT_PRIO after the
# phase and on any exit path.
#
# Output: ~/measure/<timestamp>/{env.txt,<phase>.log,<phase>.pidin.txt,...}

DUR=${1:-600}
NCPU=${2:-4}
PROFILE=${3:-default}
APP=$HOME/bin/wifi-analyzer
APP_ARGS_BASE="-i bcm0 -c CA"
WPA_NAME=wpa_supplicant-2.11
WPA_DEFAULT_PRIO=10r
OUT=$HOME/measure/$(date +%Y%m%d-%H%M%S)
LOADPIDS=""
KEEPALIVE=""
WPA_CHANGED=""

case "$DUR" in
    ''|*[!0-9]*) echo "error: seconds_per_phase must be an integer"; exit 2 ;;
esac
[ "$DUR" -ge 10 ] || { echo "error: seconds_per_phase must be >= 10"; exit 2; }

case "$PROFILE" in
    default|followup) ;;
    *) echo "error: unknown profile '$PROFILE' (default|followup)"; exit 2 ;;
esac

mkdir -p "$OUT" || exit 1
[ -x "$APP" ] || { echo "error: $APP missing -- run scripts/deploy.sh"; exit 1; }

echo ">> sudo is needed (wpa_supplicant socket + RT priorities)"
sudo -v || exit 1
# Keep the sudo timestamp fresh for the whole run.
( while sleep 60; do sudo -n -v 2>/dev/null || exit; done ) &
KEEPALIVE=$!

start_load() {
    i=0
    while [ $i -lt "$NCPU" ]; do
        sh -c 'while :; do :; done' &
        LOADPIDS="$LOADPIDS $!"
        i=$((i + 1))
    done
}
stop_load() {
    # LOADPIDS is a space-separated list (POSIX sh has no arrays), so it's
    # split on purpose.
    # shellcheck disable=SC2086
    [ -n "$LOADPIDS" ] && kill $LOADPIDS 2>/dev/null
    LOADPIDS=""
}
# Note: QNX slay's exit status isn't 0 on success (it reflects how many
# processes matched), so it can't gate the restore. Track the request
# instead, and verify the result with pidin.
set_wpa_prio() {
    sudo -n slay -Q -f -P "$1" "$WPA_NAME" >/dev/null 2>&1
    return 0
}
restore_wpa_prio() {
    if [ -n "$WPA_CHANGED" ]; then
        set_wpa_prio "$WPA_DEFAULT_PRIO"
        WPA_CHANGED=""
    fi
}
cleanup() {
    stop_load
    sudo -n slay -Q -f -s SIGINT wifi_spectrum_analyzer >/dev/null 2>&1
    restore_wpa_prio
    [ -n "$KEEPALIVE" ] && kill "$KEEPALIVE" 2>/dev/null
}
trap 'echo ">> interrupted -- cleaning up"; cleanup; exit 130' INT TERM HUP

{
    echo "date:    $(date)"
    echo "profile: $PROFILE, ${DUR}s per phase, load hogs: $NCPU"
    uname -a
    echo
    pidin info
    echo
    echo "wpa_supplicant.conf (country/ctrl_interface only):"
    sudo -n grep -E '^(country|ctrl_interface)' /boot/wpa_supplicant.conf
    echo
    echo "$WPA_NAME threads at start:"
    sudo -n pidin -p "$WPA_NAME"
} >"$OUT/env.txt" 2>&1

# Start from the image default even if an earlier run was interrupted.
set_wpa_prio "$WPA_DEFAULT_PRIO"

# run_phase <name> <load:0|1> <wpa_prio|-> [extra app args...]
run_phase() {
    name=$1; load=$2; wpa_prio=$3
    shift 3
    echo ">> phase '$name' for ${DUR}s (load=$load wpa=$wpa_prio args='$*') ($(date))"

    if [ "$wpa_prio" != "-" ]; then
        WPA_CHANGED=1
        set_wpa_prio "$wpa_prio"
    fi
    if [ "$load" = 1 ]; then
        start_load
        sleep 2
    fi

    sudo -n sh -c "exec $APP $APP_ARGS_BASE $* >$OUT/$name.log 2>&1" &
    app=$!
    sleep 8
    # Thread priorities/policies as actually applied (e.g. 15f, 10r).
    # The app runs as root: pidin must too, or it sees only our own processes.
    # The output files are written as the login user on purpose, so they stay
    # ours to fetch and delete.
    # shellcheck disable=SC2024
    {
        sudo -n pidin -p wifi_spectrum_analyzer >"$OUT/$name.pidin.txt" 2>&1
        sudo -n pidin -p "$WPA_NAME" >>"$OUT/$name.pidin.txt" 2>&1
        [ "$load" = 1 ] && sudo -n pidin >"$OUT/$name.system-pidin.txt" 2>&1
    }
    sleep $((DUR - 8))
    sudo -n slay -Q -f -s SIGINT wifi_spectrum_analyzer >/dev/null 2>&1
    wait $app || true

    stop_load
    restore_wpa_prio
    echo "   done: $(grep -c 'Sending wpa_supplicant command: SCAN$' "$OUT/$name.log") scan cycles"
}

case "$PROFILE" in
    default)
        run_phase baseline      0 -
        run_phase load          1 -
        ;;
    followup)
        run_phase hr-idle       0 -   -T 1000
        run_phase hr-load       1 -   -T 1000
        run_phase hr-load-wpa14 1 14r -T 1000
        ;;
esac

cleanup
trap - INT TERM HUP
echo ">> results in $OUT"
exit 0
