#!/usr/bin/bash
# Actual production observation/reconciliation; replace only external edges.
set -uo pipefail
fixture_runtime=$(mktemp -d)
trap 'rm -rf -- "$fixture_runtime"' EXIT
# Redirect only the fixed runtime-directory filesystem edge for portable CI.
source <(sed 's|^  candidate_runtime=/run/user/\$candidate_uid$|  candidate_runtime='"$fixture_runtime"'|' \
  "$1/packaging/linux/vibepollo-session-controller")
fail_test() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
desktop_user=fixture desktop_uid=1000 desktop_backend=hyprland-kms
fixture_unit=active fixture_substate=running fixture_pid=42
fixture_invocation=1234567890abcdef1234567890abcdef
fixture_query_ok=1 fixture_peer_status=0 fixture_environment_ok=1
fixture_cgroup=/user.slice/user-1000.slice/user@1000.service/session.slice/wayland-wm@hyprland.desktop.service
fixture_peer=42-123
bound_present=1 host_running=1 quiesce_calls=0 publish_calls=0 start_calls=0
read_settings() { desktop_user=fixture; desktop_uid=1000; desktop_backend=hyprland-kms; }
loginctl_active_session() { printf '42\n'; }
loginctl_session_properties() {
  printf '%s\n' User=1000 Name=fixture Seat=seat0 Remote=no Type=wayland Class=user Active=yes State=active Service=sddm
}
/usr/bin/getent() { printf 'fixture:x:1000:1000:Fixture:/home/fixture:/bin/bash\n'; }
/usr/bin/stat() { case "$*" in *'%a'*) printf '700\n' ;; *) printf '1000\n' ;; esac; }
/usr/bin/id() { printf '1000\n'; }
/usr/bin/setpriv() { case "$*" in *'/usr/bin/stat -c %u'*) printf '1000\n' ;; *) return 0 ;; esac; }
user_systemctl() {
  ((fixture_query_ok)) || return 1
  case "$*" in
    *show-environment*) ((fixture_environment_ok)) || return 1; printf 'WAYLAND_DISPLAY=wayland-1\n' ;;
    *wayland-wm@hyprland.desktop.service*)
      printf '%s\n' "ActiveState=$fixture_unit" "SubState=$fixture_substate" "MainPID=$fixture_pid" "InvocationID=$fixture_invocation" "ControlGroup=$fixture_cgroup" ;;
    *) return 1 ;;
  esac
}
hyprland_socket_peer_identity() { ((fixture_peer_status == 0)) || return "$fixture_peer_status"; printf '%s\n' "$fixture_peer"; }
read_session_record() {
  ((bound_present)) || return 1
  local -n record_adapter=$1
  record_adapter=([session]=42 [generation]=1 [user]=fixture [uid]=1000 [gid]=1000
    [home]=/home/fixture [runtime]=/run/user/1000 [display]=wayland-1 [xdisplay]='' [xauthority]=''
    [groups]=1000 [role]=desktop [backend]=hyprland-kms [compositor]=1234567890abcdef1234567890abcdef-42-123)
}
host_is_active() { ((host_running)); }
quiesce() { ((quiesce_calls+=1)); bound_present=0; host_running=0; return 0; }
publish_session() { ((publish_calls+=1)); return 0; }
start_host() { ((start_calls+=1)); return 0; }
session_is_ready() { return 0; }
# Reconciliation still executes both production candidate_still_active checks.

# An unavailable systemd query, or unavailable peer under the same known unit,
# is unknown. Preserve an existing binding across repeated suspend probes.
fixture_query_ok=0
for cycle in {1..10}; do reconcile_once || fail_test 'unknown query errored'; done
[[ "$quiesce_calls" == 0 && "$session_observation_retry_pending" == 1 ]] || fail_test 'unknown unit query revoked running binding'
fixture_query_ok=1 fixture_peer_status=1
for cycle in {1..10}; do reconcile_once || fail_test 'unknown peer errored'; done
[[ "$quiesce_calls" == 0 && "$start_calls" == 0 ]] || fail_test 'same-invocation unavailable peer admitted or revoked'
printf 'PASS: unavailable observations preserve existing suspend retry without replacement admission\n'

# Successfully observed stopped service is authoritative even without socket
# environment; stale host and record disappear on the first reconciliation.
fixture_unit=inactive fixture_substate=dead fixture_pid=0 fixture_environment_ok=0
observe_active_session; status=$?
[[ "$status" == 3 ]] || fail_test "authoritative stopped unit returned $status instead of revoked identity"
reconcile_once || fail_test 'stopped-unit reconciliation errored'
[[ "$quiesce_calls" == 1 && "$bound_present" == 0 && "$host_running" == 0 && "$start_calls" == 0 ]] || fail_test 'stopped compositor retained old generation'
for cycle in {1..10}; do reconcile_once || fail_test 'repeated stopped reconciliation errored'; done
[[ "$start_calls" == 0 && "$publish_calls" == 0 ]] || fail_test 'stopped compositor admitted replacement'
printf 'PASS: authoritative stopped compositor immediately revokes host and record\n'

fixture_unit=active fixture_substate=running fixture_pid=43 fixture_environment_ok=1
fixture_invocation=abcdef1234567890abcdef1234567890 fixture_peer_status=1
bound_present=1 host_running=1
before=$quiesce_calls
observe_active_session; status=$?
[[ "$status" == 3 ]] || fail_test "changed invocation plus unavailable peer returned $status"
reconcile_once || fail_test 'changed invocation reconciliation errored'
[[ "$quiesce_calls" == $((before+1)) && "$bound_present" == 0 && "$start_calls" == 0 ]] || fail_test 'unverified replacement retained old binding or was admitted'
for cycle in {1..10}; do reconcile_once || fail_test 'repeated replacement reconciliation errored'; done
[[ "$start_calls" == 0 && "$publish_calls" == 0 ]] || fail_test 'unverified replacement admitted after retry'
printf 'PASS: changed known invocation revokes old generation before a peer can be admitted\n'

# Same replacement with missing environment must also revoke immediately.
bound_present=1 host_running=1 fixture_environment_ok=0
before=$quiesce_calls
reconcile_once || fail_test 'missing replacement environment errored'
[[ "$quiesce_calls" == $((before+1)) && "$bound_present" == 0 ]] || fail_test 'missing replacement environment masked invocation change'
fixture_environment_ok=1

# A successfully connected peer rejected by UID/cgroup authority is not an
# unavailable probe. Revocation applies even under an unchanged unit.
fixture_invocation=1234567890abcdef1234567890abcdef fixture_peer_status=3
bound_present=1 host_running=1
before=$quiesce_calls
reconcile_once || fail_test 'rejected peer errored'
[[ "$quiesce_calls" == $((before+1)) && "$bound_present" == 0 ]] || fail_test 'rejected authoritative peer retained old generation'

# A unit responding from an unexpected cgroup is an authoritative identity
# mismatch, independently of the peer adapter and under the same invocation.
fixture_peer_status=0
fixture_cgroup=/user.slice/user-1001.slice/user@1001.service/session.slice/wayland-wm@hyprland.desktop.service
bound_present=1 host_running=1
before=$quiesce_calls
reconcile_once || fail_test 'rejected unit cgroup errored'
[[ "$quiesce_calls" == $((before+1)) && "$bound_present" == 0 ]] || fail_test 'unexpected unit cgroup retained old generation'
fixture_cgroup=/user.slice/user-1000.slice/user@1000.service/session.slice/wayland-wm@hyprland.desktop.service

# Positive control: after revocation a fully verified replacement progresses
# through production observation, final authority checks, publication and start.
fixture_invocation=abcdef1234567890abcdef1234567890 fixture_peer_status=0 fixture_peer=43-124
reconcile_once || fail_test 'verified replacement errored'
[[ "$publish_calls" == 1 && "$start_calls" == 1 ]] || fail_test 'verified replacement was not admitted'
printf 'PASS: verified replacement reaches fresh publication/start after revocation\n'
