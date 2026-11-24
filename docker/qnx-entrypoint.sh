#!/bin/bash
# Source the mounted QNX SDP environment (if any), then run the given command.
set -e

# Without the SDP mounted (host unit tests, scripts/host-test.sh) there's
# nothing to source: run the command as is.
if [ ! -f "${QNX_SDP}/qnxsdp-env.sh" ]; then
    exec "$@"
fi
if [ ! -d "${HOME}/.qnx/license" ]; then
    echo "warning: no QNX license in ${HOME}/.qnx -- qcc will refuse to run." >&2
    echo "         Mount it, e.g. -v \$HOME/.qnx:${HOME}/.qnx" >&2
fi

# qnxsdp-env.sh must be sourced from bash; it sets QNX_HOST/QNX_TARGET/PATH.
# shellcheck disable=SC1091
source "${QNX_SDP}/qnxsdp-env.sh" >/dev/null

exec "$@"
