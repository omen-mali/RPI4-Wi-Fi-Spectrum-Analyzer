# shellcheck shell=bash
# Sourced by the host-side scripts (deploy.sh, measure-timing.sh, qnx-build.sh,
# ci-local.sh), not run on its own. Callers set REPO first.
#
# Settings that belong to one setup (which Pi, which regulatory country, where
# the QNX SDP lives) are kept out of the repo, in a file of shell assignments:
# $WSA_LOCAL_ENV if set, otherwise local.env at the repo root, which git
# ignores. local.env.example lists the variables. A value in that file replaces
# the same variable from the environment, and CLI arguments win over
# both.

# Loads the settings file if there is one. A WSA_LOCAL_ENV naming a missing
# file is an error; a missing default local.env isn't.
wsa_load_local_env() {
    local file="${WSA_LOCAL_ENV:-$REPO/local.env}"
    if [ -f "$file" ]; then
        # shellcheck source=/dev/null
        . "$file"
    elif [ -n "${WSA_LOCAL_ENV:-}" ]; then
        echo "error: WSA_LOCAL_ENV=$WSA_LOCAL_ENV doesn't exist" >&2
        exit 1
    fi
}

# Sets PI to the Pi's user@host: the argument if non-empty, else WSA_PI.
# Exits with a hint when neither is set.
wsa_require_pi() {
    PI="${1:-${WSA_PI:-}}"
    if [ -z "$PI" ]; then
        echo "error: no Pi given: pass user@host, or set WSA_PI in local.env (see local.env.example)" >&2
        exit 1
    fi
}
