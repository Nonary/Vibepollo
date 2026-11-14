#!/usr/bin/bash
# Production SDDM observation and reconciliation with inert command boundaries.
set -euo pipefail
source "$1/packaging/linux/vibepollo-session-controller"
fail_test() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
fixture_bound=1 fixture_host=1 fixture_status=0 fixture_check=0
fixture_output='wayland-9 sddm-42-123'
bound_compositor=sddm-42-123
bound_mutation=''
reset_candidate() {
  candidate_session=c1 candidate_user=sddm candidate_uid=982 candidate_gid=982
  candidate_home=/var/lib/sddm candidate_runtime=/run/user/982
  candidate_groups=982 candidate_role=greeter candidate_backend=kwin
  candidate_display='' candidate_compositor='' candidate_xdisplay=:99 candidate_xauthority=stale
}
read_session_record() {
  ((fixture_bound)) || return 2
  local -n destination=$1
  destination=([session]=c1 [user]=sddm [uid]=982 [gid]=982 [home]=/var/lib/sddm
    [runtime]=/run/user/982 [groups]=982 [role]=greeter [backend]=kwin
    [compositor]="$bound_compositor" [display]=wayland-9 [xdisplay]='' [xauthority]='' [generation]=1)
  [[ -z "$bound_mutation" ]] || destination[$bound_mutation]=changed
}
session_command_bounded() {
  [[ "$1:$2:$3" == 5:982:982 ]] || fail_test 'SDDM helper escaped selected UID/GID/timeout'
  shift 3
  case "$*" in
    '/usr/bin/env -i XDG_RUNTIME_DIR=/run/user/982 /usr/libexec/vibeshine/vibepollo-kwin-session-environment --sddm-session c1')
      ((fixture_status == 0)) || return "$fixture_status"
      printf '%s\n' "$fixture_output" ;;
    "/usr/bin/env -i XDG_RUNTIME_DIR=/run/user/982 /usr/libexec/vibeshine/vibepollo-kwin-session-environment --sddm-check c1 $bound_compositor")
      return "$fixture_check" ;;
    *) fail_test "unexpected helper call: $*" ;;
  esac
}
expect_observation() {
  local expected=$1 status=0
  reset_candidate
  observe_sddm_environment || status=$?
  [[ "$status" == "$expected" ]] || fail_test "expected status $expected, got $status"
}
expect_observation 0
[[ "$candidate_display:$candidate_compositor" == wayland-9:sddm-42-123 &&
   -z "$candidate_xdisplay" && -z "$candidate_xauthority" ]] || fail_test 'SDDM imported desktop X11 credentials'
for fixture_status in 1 124 137; do
  expect_observation 1 # A live saved KWin survives a missing socket or timeout.
done
fixture_status=1
fixture_check=1
expect_observation 1 # Unavailable /proc reads also retry, without admitting.
for fixture_check in 3 5; do expect_observation 3; done
fixture_check=0
for bound_mutation in session user uid gid home runtime groups role backend; do expect_observation 3; done
bound_mutation=''
fixture_bound=0
expect_observation 1 # An unready boot is waiting, not a compositor replacement.
fixture_bound=1
fixture_status=3
expect_observation 3
fixture_status=5
expect_observation 5
[[ "$rejected_session" == *'CompositorCommand=kwin_wayland'* && "$rejected_session" == *'without Weston or a wrapper'* ]] ||
  fail_test 'unsupported compositor did not explain the supported configuration'
fixture_status=0
for fixture_output in 'wayland-9 sddm-42-123 extra' 'wayland-../9 sddm-42-123' 'wayland-9 sddm-0-1' $'wayland-9 sddm-42-123\ninjected'; do
  expect_observation 1
done
fixture_output='wayland-9 sddm-42-123'

# Readiness must use the observed socket, not PLM units or a manager-imported
# environment. Existing Wayland/KScreen probes remain mandatory.
reset_candidate
/usr/bin/setpriv() { :; }
user_unit_running() { fail_test 'SDDM required a Plasma Login Manager unit'; }
user_systemctl() { fail_test 'SDDM imported the user manager environment'; }
probes=''
probe_wayland() { probes+=W; }
probe_x11() { probes+=X; }
session_is_ready || fail_test 'supported SDDM readiness rejected'
[[ "$probes" == WX ]] || fail_test 'SDDM skipped a display readiness probe'

# Run the actual logind parser -> SDDM observer -> publication revalidation
# path. Redirect only the fixed runtime-directory assignment to a private
# fixture; mock the OS commands without requiring a real account or /run path.
(
  fixture_runtime=$(mktemp -d)
  trap 'rm -rf -- "$fixture_runtime"' EXIT
  definition=$(declare -f observe_active_session)
  assignment='candidate_runtime=/run/user/$candidate_uid'
  [[ "$definition" == *"$assignment"* ]] || fail_test 'runtime fixture boundary moved'
  eval "${definition//"$assignment"/'candidate_runtime=$fixture_runtime'}"
  desktop_user=fixture desktop_uid=1000 desktop_backend=hyprland-kms
  fixture_status=0 fixture_check=0
  loginctl_active_session() { printf 'c1\n'; }
  loginctl_session_properties() {
    printf 'User=982\nName=sddm\nSeat=seat0\nRemote=no\nType=wayland\nClass=greeter\nActive=yes\nState=active\nService=sddm-greeter\n'
  }
  /usr/bin/getent() { printf 'sddm:x:982:982::/var/lib/sddm:/usr/bin/nologin\n'; }
  /usr/bin/stat() { case "$2" in %u) printf '982\n' ;; %a) printf '700\n' ;; *) return 1 ;; esac; }
  /usr/bin/id() { printf '982\n'; }
  hyprland_invocation() { fail_test 'SDDM greeter entered the desktop Hyprland path'; }
  read_session_record() {
    local -n destination=$1
    destination=([session]=c1 [user]=sddm [uid]=982 [gid]=982 [home]=/var/lib/sddm
      [runtime]="$fixture_runtime" [groups]=982 [role]=greeter [backend]=kwin
      [compositor]=sddm-42-123 [display]=wayland-9 [xdisplay]='' [xauthority]='' [generation]=1)
  }
  session_command_bounded() {
    [[ "$1:$2:$3" == 5:982:982 ]] || fail_test 'real observer selected an unexpected identity'
    case "$*" in
      *'--sddm-session c1')
        ((fixture_status == 0)) || return "$fixture_status"
        printf 'wayland-9 sddm-42-123\n' ;;
      *'--sddm-check c1 sddm-42-123') return "$fixture_check" ;;
      *) fail_test "unexpected real-observer command: $*" ;;
    esac
  }
  observe_active_session || fail_test 'logind SDDM greeter did not reach its production observer'
  [[ "$candidate_role:$candidate_backend:$candidate_compositor" == greeter:kwin:sddm-42-123 ]] ||
    fail_test 'SDDM greeter inherited the configured desktop backend'
  candidate_still_active || fail_test 'unchanged SDDM binding failed publication revalidation'
  fixture_status=1
  if candidate_still_active; then fail_test 'a transient observation admitted a new host at publication'; fi
) || exit $?

# Exercise real lifecycle decisions, including PID changes and handoff, with
# only service operations replaced. A transient cannot create a new host.
observe_active_session() { reset_candidate; observe_sddm_environment; }
read_settings() { :; }
host_is_active() { ((fixture_host)); }
events=''
quiesce() { events+=Q; fixture_host=0; fixture_bound=0; }
session_is_ready() { read_sddm_environment; }
candidate_still_active() { :; }
publish_session() { events+=P; fixture_bound=1; bound_compositor=$candidate_compositor; }
start_host() { events+=S; fixture_host=1; }
fixture_status=1
reconcile_once || fail_test 'reconciliation failed'
[[ -z "$events" && "$fixture_host:$fixture_bound" == 1:1 ]] || fail_test 'transient stopped a running greeter'
[[ "$session_observation_retry_pending" == 1 ]] || fail_test 'transient did not schedule retry'
fixture_status=0
reconcile_once || fail_test 'reconciliation failed'
[[ -z "$events" && "$session_observation_retry_pending" == 0 ]] || fail_test 'recovered greeter restarted'
fixture_output='wayland-9 sddm-43-456'
reconcile_once || fail_test 'reconciliation failed'
[[ "$events" == QPS && "$bound_compositor" == sddm-43-456 ]] || fail_test 'replacement did not revoke before publishing'
events=''
fixture_status=1 fixture_check=3
reconcile_once || fail_test 'reconciliation failed'
[[ "$events" == Q && "$fixture_host:$fixture_bound" == 0:0 ]] || fail_test 'dead saved KWin remained bound'
events=''
fixture_check=0
reconcile_once || fail_test 'reconciliation failed'
[[ "$events" == Q && "$last_status" == no-session && "$fixture_host" == 0 ]] || fail_test 'unready greeter admitted a host'
events=''
fixture_status=5
reconcile_once || fail_test 'reconciliation failed'
[[ "$events" == Q && "$last_status" == unsupported-greeter ]] || fail_test 'non-KWin greeter diagnosis was lost'
printf 'PASS: SDDM readiness, transient retries, confirmed loss/replacement and unsupported diagnostics\n'
