#!/usr/bin/env bash
set -euo pipefail
if (( EUID == 0 )); then echo 'Run without sudo.' >&2; exit 1; fi
kit=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
state="${XDG_DATA_HOME:-$HOME/.local/share}/vibepollo-bazzite"
mkdir -p "$state/logs"
log="$state/logs/drivers-$(date +%Y%m%d-%H%M%S).log"
echo "Log: $log"
python3 -u "$kit/build-drivers.py" "$@" 2>&1 | tee "$log"
echo "Send this log: $log"
