#!/usr/bin/env bash
# Read-only readiness checks; run in the graphical SteamOS session as its user.
set -uo pipefail

failed=0
check() {
  local description=$1
  shift
  if "$@"; then
    printf 'PASS: %s\n' "$description"
  else
    printf 'FAIL: %s\n' "$description"
    failed=1
  fi
}

check 'Running as the desktop user' test "$(id -u)" -ne 0
check 'PipeWire session socket is available' test -S "${XDG_RUNTIME_DIR:-/nonexistent}/pipewire-0"
check 'Virtual keyboard, mouse and Xbox gamepad input is accessible' test -w /dev/uinput

render_access=no
for device in /dev/dri/renderD*; do
  if [[ -r "$device" && -w "$device" ]]; then render_access=yes; fi
done
check 'A GPU render device is accessible for hardware encoding' test "$render_access" == yes

if [[ ! -w /dev/uhid ]]; then
  printf 'NOTE: /dev/uhid is unavailable; select an Xbox controller instead of DualSense.\n'
fi

printf 'These checks do not prove capture or encoding. Verify an actual Moonlight stream in both modes.\n'
exit "$failed"
