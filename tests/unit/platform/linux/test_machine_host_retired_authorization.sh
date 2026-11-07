#!/usr/bin/bash

# `vibeshine authorize-commands` used to approve every custom command before
# the session broker would run it. Commands now follow their own "Run
# elevated" setting, so the subcommand only explains that and succeeds.

set -uo pipefail

host=$1

fail_test() {
  /usr/bin/printf 'FAIL: %s\n' "$*" >&2
  exit 1
}

# Older docs and issue threads still tell people to run this; it must succeed
# without root and must not touch any allowlist.
output=$(/usr/bin/bash "$host" authorize-commands 2>&1) ||
  fail_test 'authorize-commands no longer exits successfully'
[[ "$output" == *'no longer required'* ]] ||
  fail_test "authorize-commands did not explain the change: $output"
[[ "$output" == *'Run elevated'* ]] ||
  fail_test "authorize-commands did not point to the elevation setting: $output"

if /usr/bin/bash "$host" authorize-commands extra >/dev/null 2>&1; then
  fail_test 'authorize-commands accepted unexpected arguments'
fi

usage=$(/usr/bin/bash "$host" 2>&1) && fail_test 'missing subcommand succeeded'
[[ "$usage" != *authorize-commands* ]] ||
  fail_test 'usage still advertises authorize-commands'

# shellcheck source=/dev/null
source "$host"
[[ "$legacy_command_manifest" == /etc/vibeshine/session-commands ]] ||
  fail_test 'legacy allowlist path changed; upgrades would leave it behind'
for retired in authorize_commands ensure_command_manifest extract_desktop_authorizations \
               install_command_manifest migrate_legacy_command_manifest; do
  if declare -F "$retired" >/dev/null; then
    fail_test "retired allowlist function still exists: $retired"
  fi
done

/usr/bin/printf 'PASS\n'
