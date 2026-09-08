#!/usr/bin/bash

set -uo pipefail

source "$1"
target_uid=$(/usr/bin/id -u)
target_gid=$(/usr/bin/id -g)
restricted_root=0
if [[ ${2:-} == --restricted-root ]]; then
  target_uid=$3
  target_gid=$4
  restricted_root=1
  [[ $EUID == 0 ]] || exit 1
  # Run this mode through setpriv with the controller's exact capability set.
  # It reproduces the cross-UID failure without granting CAP_KILL.
  while read -r key value _; do
    case "$key" in
      CapEff:|CapBnd:) ((16#$value == 0xc0)) || exit 1 ;;
    esac
  done </proc/self/status
elif ((EUID == 0)); then
  account=$(/usr/bin/getent passwd nobody) || exit 1
  IFS=: read -r _ _ target_uid target_gid _ <<<"$account"
else
  # The regular suite uses the real setpriv/timeout processes under one UID.
  # Only initgroups requires privilege, so keep the test runner's groups.
  definition=$(declare -f session_command_bounded)
  eval "${definition/--init-groups/--keep-groups}"
fi

temporary=$(/usr/bin/mktemp -d /tmp/vibepollo-session-timeout-test.XXXXXX) || exit 1
declare -a clients=()
cleanup_client() {
  local pid=$1
  [[ "$pid" =~ ^[1-9][0-9]*$ && -d /proc/$pid ]] || return 0
  if ((restricted_root)); then
    /usr/bin/setpriv --reuid "$target_uid" --regid "$target_gid" --init-groups --no-new-privs -- \
      /usr/bin/kill -KILL -- "$pid" 2>/dev/null || :
  else
    kill -KILL "$pid" 2>/dev/null || :
  fi
}
cleanup() {
  local pid
  for pid in "${clients[@]}"; do cleanup_client "$pid"; done
  /usr/bin/rm -rf -- "$temporary"
}
trap cleanup EXIT
fail() { /usr/bin/printf 'FAIL: %s\n' "$*" >&2; exit 1; }
client_is_running() {
  local pid=$1 state
  [[ -r /proc/$pid/stat ]] || return 1
  read -r _ _ state _ </proc/"$pid"/stat || return 1
  [[ "$state" != Z && "$state" != X ]]
}

if ((restricted_root)); then
  # Demonstrate the original defect against an isolated sleeping process.
  # The root monitor cannot signal its dropped-UID child and kills itself.
  /usr/bin/timeout --signal=KILL 1 \
    /usr/bin/setpriv --reuid "$target_uid" --regid "$target_gid" --init-groups --no-new-privs -- \
      /usr/bin/bash -c 'printf "%s\n" "$$"; exec /usr/bin/sleep 30' >"$temporary/old-pid" 2>/dev/null
  read -r pid <"$temporary/old-pid" || fail 'old wrapper did not start the fixture'
  clients+=("$pid")
  client_is_running "$pid" || fail 'restricted root fixture did not reproduce the orphaned client'
  cleanup_client "$pid"
fi

output=$(session_command_bounded 1 "$target_uid" "$target_gid" /usr/bin/printf '%s' ready)
[[ $? == 0 && "$output" == ready ]] || fail 'successful command output/status changed'
session_command_bounded 1 "$target_uid" "$target_gid" /usr/bin/bash -c 'exit 23'
[[ $? == 23 ]] || fail 'command failure status changed'

for termination in normal ignore; do
  started=$SECONDS
  session_command_bounded 1 "$target_uid" "$target_gid" \
    /usr/bin/bash -c '
      [[ "$1" != ignore ]] || trap "" TERM
      printf "%s\n" "$$"
      exec /usr/bin/sleep 30
    ' timeout-fixture "$termination" >"$temporary/new-pid" 2>/dev/null
  status=$?
  ((status == 124 || status == 137)) || fail "stalled client returned unexpected status $status"
  ((SECONDS - started <= 4)) || fail 'stalled client exceeded the outer deadline'
  read -r pid <"$temporary/new-pid" || fail 'new wrapper did not start the fixture'
  clients+=("$pid")
  # SIGKILL may leave a briefly reparented zombie while systemd reaps it; no
  # running client may survive. The normal TERM path is reaped by timeout.
  for ((attempt=0; attempt<20; ++attempt)); do
    client_is_running "$pid" || break
    /usr/bin/sleep 0.05
  done
  client_is_running "$pid" && fail "timed out $termination client was orphaned"
done

/usr/bin/printf 'PASS: session command timeout cancels clients and preserves command results\n'
