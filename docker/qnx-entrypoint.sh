#!/bin/bash
# Source the mounted QNX SDP environment, then run the given command.
set -e

if [ ! -f "${QNX_SDP}/qnxsdp-env.sh" ]; then
    echo "error: QNX SDP not found at ${QNX_SDP}." >&2
    echo "       Mount it, e.g. -v \$HOME/qnx800:${QNX_SDP}" >&2
    exit 1
fi
if [ ! -d "${HOME}/.qnx/license" ]; then
    echo "warning: no QNX license in ${HOME}/.qnx -- qcc will refuse to run." >&2
    echo "         Mount it, e.g. -v \$HOME/.qnx:${HOME}/.qnx" >&2
fi

# qnxsdp-env.sh must be sourced from bash; it sets QNX_HOST/QNX_TARGET/PATH.
# shellcheck disable=SC1091
source "${QNX_SDP}/qnxsdp-env.sh" >/dev/null

exec "$@"
