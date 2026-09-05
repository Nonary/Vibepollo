#!/usr/bin/bash

set -uo pipefail

# shellcheck source=/dev/null
source "$1"

fail_test() {
  /usr/bin/printf 'FAIL: %s\n' "$*" >&2
  exit 1
}

split_authorization_line $'desktop\t\tsetsid steam steam://open/bigpicture' ||
  fail_test 'empty working directory was rejected'
[[ "$authorization_role" == desktop ]] || fail_test 'role did not survive empty-directory parse'
[[ -z "$authorization_directory" ]] || fail_test 'empty directory collapsed into the command field'
[[ "$authorization_command" == 'setsid steam steam://open/bigpicture' ]] || fail_test 'command did not survive empty-directory parse'

split_authorization_line $'desktop\t/mnt/games/Test Game\tsteam -applaunch 123' ||
  fail_test 'absolute working directory was rejected'
[[ "$authorization_directory" == '/mnt/games/Test Game' ]] || fail_test 'absolute working directory changed'
[[ "$authorization_command" == 'steam -applaunch 123' ]] || fail_test 'working-directory command changed'

if split_authorization_line $'desktop\trelative\t/bin/true'; then
  fail_test 'relative working directory was accepted'
fi
if split_authorization_line $'greeter\t\t/bin/true'; then
  fail_test 'greeter command authorization was accepted'
fi
if split_authorization_line $'desktop\t/bin/true'; then
  fail_test 'malformed two-field authorization was accepted'
fi

run_definition=$(declare -f run_host)
profile_definition=$(declare -f prepare_service_profile)
[[ "$run_definition" == *'capability_free_exec "$machine_host" prepare-profile'* ]] ||
  fail_test 'capability-free supervisor does not delegate profile preparation'
[[ "$run_definition" == *'"$machine_host_executable" "$machine_profile/vibepollo.conf"'* ]] ||
  fail_test 'supervisor does not launch the private permitted-only host'
[[ "$run_definition" != *'rewrite_machine_cover_paths'* &&
   "$run_definition" != *'profile_tree_is_sanitized "$machine_profile"'* ]] ||
  fail_test 'service supervisor still parses the service-owned profile directly'
[[ "$profile_definition" == *'profile_tree_is_sanitized "$machine_profile"'* &&
   "$profile_definition" == *'rewrite_machine_cover_paths'* ]] ||
  fail_test 'capability-free profile child is incomplete'

application_environment=([EXPAND]='0123456789abcdef')
normalized_test_value=$(normalize_application_value 'before-$(EXPAND)-after' 64)
[[ "$normalized_test_value" == 'before-0123456789abcdef-after' ]] ||
  fail_test 'bounded environment expansion changed a valid command'
if normalize_application_value '$(EXPAND)$(EXPAND)' 31 >/dev/null 2>&1; then
  fail_test 'repeated environment references bypassed the expanded-value bound'
fi
if normalize_application_value '0123456789' 9 >/dev/null 2>&1; then
  fail_test 'literal application value bypassed the expanded-value bound'
fi

/usr/bin/printf 'PASS: bounded command manifest and permitted-only host policy\n'

# Persistent history must never satisfy a new host's readiness check.
test_logs=$(mktemp -d)
trap 'rm -rf -- "$test_logs"' EXIT
expected_owner=$(stat -c '%U:%G' "$test_logs")
old_log=$test_logs/vibepollo-20260901-120000-000.log
new_log=$test_logs/vibepollo-20260904-120000-000.log
printf 'Configuration UI available at localhost\nFound H.264 encoder: h264_nvenc\n' >"$old_log"
declare -A retained_logs=(["$old_log"]=1)
if find_host_readiness_log "$test_logs" "$expected_owner" retained_logs; then
  fail_test 'retained ready log was accepted for a new host'
fi
[[ -z "$host_log" ]] || fail_test 'stale readiness path was retained'
# A rollover belongs to the same launch, but is not its current log.
: >"$new_log.1"
printf 'starting\n' >"$new_log"
find_host_readiness_log "$test_logs" "$expected_owner" retained_logs ||
  fail_test 'new host log was not discovered alongside persistent history'
[[ "$host_log" == "$new_log" ]] || fail_test 'wrong host log selected'
chmod 644 "$new_log"
if find_host_readiness_log "$test_logs" "$expected_owner" retained_logs; then
  fail_test 'publicly readable host log was accepted'
fi
chmod 600 "$new_log"
second_log=$test_logs/vibepollo-20260904-120001-000.log
printf 'starting\n' >"$second_log"
if find_host_readiness_log "$test_logs" "$expected_owner" retained_logs; then
  fail_test 'ambiguous new host logs were accepted'
fi
rm -- "$second_log" "$new_log"
ln -s "$old_log" "$new_log"
if find_host_readiness_log "$test_logs" "$expected_owner" retained_logs; then
  fail_test 'symlink host log was accepted'
fi
printf 'PASS: persistent host log readiness isolation\n'
