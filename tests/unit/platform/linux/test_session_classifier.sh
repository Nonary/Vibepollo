#!/usr/bin/bash

set -euo pipefail

readonly controller_source=$1
# shellcheck source=/dev/null
source "$controller_source"

fail_test() {
  /usr/bin/printf 'FAIL: %s\n' "$*" >&2
  exit 1
}

check_role() {
  local expected_role=$1
  shift
  classify_session_role "$@" || fail_test "expected accepted role $expected_role for $*"
  [[ "$candidate_role" == "$expected_role" ]] ||
    fail_test "expected role $expected_role, got $candidate_role for $*"
  [[ -z "$rejected_session" ]] || fail_test "accepted session retained rejection: $rejected_session"
}

check_rejected() {
  local expected_status=$1 expected_description=$2 observed_status
  shift 2
  if classify_session_role "$@"; then
    observed_status=0
  else
    observed_status=$?
  fi
  [[ "$observed_status" == "$expected_status" ]] ||
    fail_test "expected classifier status $expected_status, got $observed_status for $*"
  [[ -z "$candidate_role" ]] || fail_test "rejected session retained role $candidate_role"
  [[ "$rejected_session" == "$expected_description" ]] ||
    fail_test "expected rejection '$expected_description', got '$rejected_session'"
}

desktop_user=chasep
desktop_uid=1000
desktop=(1000 chasep seat0 no wayland user yes active plasmalogin)
for service in plasmalogin plasmalogin-autologin sddm sddm-autologin; do
  check_role desktop 1000 chasep seat0 no wayland user yes active "$service"
done

# Each authoritative logind boundary rejects a desktop session independently.
for index in 2 3 4 6 7; do
  rejected=("${desktop[@]}")
  case "$index" in
    2) rejected[$index]=seat1 ;;
    3) rejected[$index]=yes ;;
    4) rejected[$index]=x11 ;;
    6) rejected[$index]=no ;;
    7) rejected[$index]=closing ;;
  esac
  candidate_role=stale
  check_rejected 1 '' "${rejected[@]}"
done

# Desktop identity, class, and service are separate admission boundaries.
check_rejected 2 'user=other class=user service=plasmalogin' \
  1000 other seat0 no wayland user yes active plasmalogin
check_rejected 2 'user=chasep class=user service=plasmalogin' \
  1001 chasep seat0 no wayland user yes active plasmalogin
check_rejected 2 'user=chasep class=greeter service=plasmalogin' \
  1000 chasep seat0 no wayland greeter yes active plasmalogin
check_rejected 2 'user=chasep class=user service=plasmalogin-greeter' \
  1000 chasep seat0 no wayland user yes active plasmalogin-greeter
check_rejected 2 'user=chasep class=user service=gdm-password' \
  1000 chasep seat0 no wayland user yes active gdm-password

# The greeter is admitted only for its own exact user, class, and service.
check_role greeter 982 plasmalogin seat0 no wayland greeter yes active plasmalogin-greeter
check_rejected 2 'user=other class=greeter service=plasmalogin-greeter' \
  982 other seat0 no wayland greeter yes active plasmalogin-greeter
check_rejected 2 'user=plasmalogin class=user service=plasmalogin-greeter' \
  982 plasmalogin seat0 no wayland user yes active plasmalogin-greeter
check_rejected 2 'user=plasmalogin class=greeter service=sddm-greeter' \
  982 plasmalogin seat0 no wayland greeter yes active sddm-greeter

# Malformed logind identity data is an observation failure, not a role denial.
check_rejected 1 '' 01000 chasep seat0 no wayland user yes active plasmalogin
check_rejected 1 '' 1000 'BadName' seat0 no wayland user yes active plasmalogin

# Feed actual loginctl key/value output through the production observation
# parser and classifier. Stop at passwd lookup so the test needs no user, seat,
# or runtime-directory setup.
(
  fixture_properties=''
  loginctl_active_session() { /usr/bin/printf '42\n'; }
  loginctl_session_properties() {
    [[ "$1" == 42 ]] || return 1
    /usr/bin/printf '%s\n' "$fixture_properties"
  }
  /usr/bin/getent() { return 1; }

  expect_parsed_role_before_passwd_lookup() {
    local expected_role=$1 observed_status
    candidate_role=stale
    rejected_session=stale
    if observe_active_session; then
      fail_test 'observation continued beyond the mocked passwd boundary'
    else
      observed_status=$?
    fi
    [[ "$observed_status" == 1 ]] || fail_test "expected passwd-boundary failure, got $observed_status"
    [[ "$candidate_session" == 42 && "$candidate_role" == "$expected_role" ]] ||
      fail_test "logind properties did not classify $expected_role before passwd lookup"
    [[ -z "$rejected_session" ]] || fail_test "accepted logind record retained rejection: $rejected_session"
  }

  fixture_properties=$'User=1000\nName=chasep\nSeat=seat0\nRemote=no\nType=wayland\nClass=user\nActive=yes\nState=active\nService=sddm'
  expect_parsed_role_before_passwd_lookup desktop
  fixture_properties=$'User=982\nName=plasmalogin\nSeat=seat0\nRemote=no\nType=wayland\nClass=greeter\nActive=yes\nState=active\nService=plasmalogin-greeter'
  expect_parsed_role_before_passwd_lookup greeter

  fixture_properties=$'User=1000\nName=chasep\nSeat=seat0\nRemote=no\nType=wayland\nClass=user\nActive=yes\nActive=no\nState=active\nService=sddm'
  candidate_role=stale
  if observe_active_session; then fail_test 'duplicate logind property was accepted'; else observed_status=$?; fi
  [[ "$observed_status" == 1 && -z "$candidate_role" ]] ||
    fail_test 'duplicate logind property reached role classification'

  fixture_properties=$'User=1000\nName=chasep\nSeat=seat0\nRemote=no\nType=wayland\nClass=user\nActive=yes\nState=active'
  candidate_role=stale
  if observe_active_session; then fail_test 'incomplete logind properties were accepted'; else observed_status=$?; fi
  [[ "$observed_status" == 2 && -z "$candidate_role" ]] ||
    fail_test 'missing service property did not fail closed as unsupported role'
  [[ "$rejected_session" == 'user=chasep class=user service=' ]] ||
    fail_test "unexpected incomplete-record rejection: $rejected_session"
) || exit $?

/usr/bin/printf 'PASS: production logind session classifier boundaries\n'
