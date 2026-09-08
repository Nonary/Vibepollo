#!/usr/bin/bash

set -uo pipefail

readonly controller_source=$1
# shellcheck source=/dev/null
source "$controller_source"
controller_quiesce_definition=$(declare -f quiesce)
controller_close_broker_definition=$(declare -f close_broker_admission)
controller_stop_host_definition=$(declare -f stop_host)
controller_stop_brokers_definition=$(declare -f stop_broker_instances)
controller_stop_apps_definition=$(declare -f stop_bound_session_apps)
controller_remove_record_definition=$(declare -f remove_session_record)

declare -a events=()
mock_policy=1
mock_observe=0
mock_identity=0
mock_complete=0
mock_host=0
mock_ready=1
mock_quiesce=1
mock_publish=1
mock_start=1
mock_active_first=1
mock_active_second=1
mock_active_call=0
session_observation_retry_pending=0

fail_test() {
  /usr/bin/printf 'FAIL: %s\n' "$*" >&2
  exit 1
}

event_log() {
  local joined
  joined=$(IFS=,; /usr/bin/printf '%s' "${events[*]}")
  /usr/bin/printf '%s' "$joined"
}

expect_events() {
  local expected=$1 actual
  actual=$(event_log)
  [[ "$actual" == "$expected" ]] || fail_test "expected events [$expected], got [$actual]"
}

expect_state() {
  local expected_host=$1 expected_binding=$2
  [[ "$mock_host:$mock_identity" == "$expected_host:$expected_binding" ]] ||
    fail_test "expected host/binding $expected_host:$expected_binding, got $mock_host:$mock_identity"
}

reset_scenario() {
  events=()
  last_status=''
  health_cycle=0
  readiness_retry_pending=0
  session_observation_retry_pending=0
  mock_policy=1
  mock_observe=0
  mock_identity=0
  mock_complete=0
  mock_host=0
  mock_ready=1
  mock_quiesce=1
  mock_publish=1
  mock_start=1
  mock_active_first=1
  mock_active_second=1
  mock_active_call=0
}

read_settings() { ((mock_policy)); }

observe_active_session() {
  ((mock_observe == 0)) || return "$mock_observe"
  candidate_session=2
  candidate_user=chasep
  candidate_uid=1000
  candidate_gid=1000
  candidate_home=/home/chasep
  candidate_runtime=/run/user/1000
  candidate_groups=1000,998
  candidate_role=desktop
}

read_session_record() {
  ((mock_identity))
}

binding_matches_candidate() {
  case "${1:-identity}" in
    identity) events+=(I); ((mock_identity)) ;;
    complete) events+=(C); ((mock_complete)) ;;
    *) return 1 ;;
  esac
}

host_is_active() { ((mock_host)); }

quiesce() {
  events+=(Q)
  ((mock_quiesce)) || return 1
  mock_host=0
  mock_identity=0
  mock_complete=0
}

session_is_ready() {
  events+=(R)
  ((mock_ready)) || return 1
  candidate_display=wayland-0
  candidate_xdisplay=:0
  candidate_xauthority=/run/user/1000/xauth_test
}

candidate_still_active() {
  events+=(A)
  ((mock_active_call += 1))
  if ((mock_active_call == 1)); then ((mock_active_first)); else ((mock_active_second)); fi
}

publish_session() {
  events+=(P)
  ((mock_publish)) || return 1
  mock_identity=1
  mock_complete=1
}

start_host() {
  events+=(S)
  ((mock_start)) || return 1
  mock_host=1
}

remove_session_record() { events+=(D); mock_identity=0; mock_complete=0; }
announce() { :; }

# Invalid policy always attempts to turn the host off.
reset_scenario
mock_policy=0
mock_host=1
mock_identity=1
reconcile_once
expect_events Q
expect_state 0 0

# An unsupported graphical session cannot fall back to the greeter.
reset_scenario
mock_observe=2
mock_host=1
mock_identity=1
reconcile_once
expect_events Q
expect_state 0 0

# Suspend can interrupt the authoritative session query while the existing
# binding and host remain valid. Preserve them for one observation retry.
reset_scenario
mock_observe=1
mock_identity=1
mock_complete=1
mock_host=1
reconcile_once
expect_events ''
expect_state 1 1
[[ "$session_observation_retry_pending" == 1 ]] ||
  fail_test 'interrupted session observation did not schedule a retry'

# A successful observation clears the retry and keeps the running host.
mock_observe=0
events=()
reconcile_once
expect_events I
expect_state 1 1
[[ "$session_observation_retry_pending" == 0 ]] ||
  fail_test 'successful session observation retry remained pending'

# Repeated observation failures do not stop an already-running automatic host.
reset_scenario
mock_observe=1
mock_identity=1
mock_complete=1
mock_host=1
session_observation_retry_pending=1
reconcile_once
expect_events ''
expect_state 1 1
[[ "$session_observation_retry_pending" == 1 ]] ||
  fail_test 'repeated session observation failure did not preserve the running host'

# An unready target stays quiesced and never publishes or starts a host.
reset_scenario
mock_ready=0
reconcile_once
expect_events I,Q,R
expect_state 0 0

# A new ready desktop is published only after cleanup and two active-session checks.
reset_scenario
reconcile_once
expect_events I,Q,R,A,P,A,S
expect_state 1 1

# A matching healthy host uses the cheap path for four cycles.
reset_scenario
mock_identity=1
mock_complete=1
mock_host=1
reconcile_once
expect_events I
expect_state 1 1

# The fifth cycle actively verifies compositor health and the complete binding.
reset_scenario
mock_identity=1
mock_complete=1
mock_host=1
health_cycle=4
reconcile_once
expect_events I,R,C
expect_state 1 1

# Suspend can interrupt a compositor query while the same notified host and
# logind identity remain valid. Preserve it for one immediate readiness retry.
reset_scenario
mock_identity=1
mock_complete=1
mock_host=1
mock_ready=0
health_cycle=4
reconcile_once
expect_events I,R
expect_state 1 1
[[ "$readiness_retry_pending" == 1 ]] ||
  fail_test 'interrupted running-host readiness did not schedule an immediate retry'

# The pending retry bypasses the cheap health path and clears on success.
mock_ready=1
events=()
reconcile_once
expect_events I,R,C
expect_state 1 1
[[ "$readiness_retry_pending" == 0 ]] ||
  fail_test 'successful running-host readiness retry remained pending'

# Repeated readiness failures do not stop an already-running automatic host.
reset_scenario
mock_identity=1
mock_complete=1
mock_host=1
mock_ready=0
readiness_retry_pending=1
reconcile_once
expect_events I,R
expect_state 1 1
[[ "$readiness_retry_pending" == 1 ]] ||
  fail_test 'repeated readiness failure did not preserve the running host'

# New display credentials in the same logind session force disconnect/rebind.
reset_scenario
mock_identity=1
mock_complete=0
mock_host=1
health_cycle=4
reconcile_once
expect_events I,R,C,Q,A,P,A,S
expect_state 1 1

# A stopped host always revokes its complete binding. The next cycle publishes
# a new generation before entering full wrapper readiness.
reset_scenario
mock_identity=1
mock_complete=1
reconcile_once
expect_events I,Q
expect_state 0 0

# A stopped existing binding is discarded without a direct active-session
# restart check; the next generation receives both authoritative snapshots.
reset_scenario
mock_identity=1
mock_complete=1
mock_active_first=0
reconcile_once
expect_events I,Q
expect_state 0 0

# Cleanup failure prevents readiness probes, publication, and connector reuse.
reset_scenario
mock_host=1
mock_quiesce=0
reconcile_once
expect_events I,Q
expect_state 1 0

# A publish failure removes any partial record and keeps the host off.
reset_scenario
mock_publish=0
reconcile_once
expect_events I,Q,R,A,P,D
expect_state 0 0

# If logind changes after publication, the new record is discarded before start.
reset_scenario
mock_active_second=0
reconcile_once
expect_events I,Q,R,A,P,A,Q
expect_state 0 0

# A failed host start is followed by a complete cleanup.
reset_scenario
mock_start=0
reconcile_once
expect_events I,Q,R,A,P,A,S,Q
expect_state 0 0

# Once admission is closed, a wedged host must not short-circuit bounded
# broker/application shutdown attempts. Persistent state is retained unless
# every exact cgroup proof succeeds.
eval "$controller_quiesce_definition"
declare -a quiesce_events=()
close_broker_admission() { quiesce_events+=(C); }
stop_host() { quiesce_events+=(H); return 1; }
stop_broker_instances() { quiesce_events+=(B); }
stop_bound_session_apps() { quiesce_events+=(A); }
remove_session_record() { quiesce_events+=(D); }
if quiesce; then
  fail_test 'quiesce accepted a host cgroup that did not stop'
fi
[[ "${quiesce_events[*]}" == 'C H B A B' ]] ||
  fail_test "quiesce skipped bounded worker cleanup after host-stop failure: ${quiesce_events[*]}"
eval "$controller_close_broker_definition"
eval "$controller_stop_host_definition"
eval "$controller_stop_brokers_definition"
eval "$controller_stop_apps_definition"
eval "$controller_remove_record_definition"

# Targets intentionally omit MainPID; services still require a live main
# process in addition to their active/running state.
mock_unit_properties=$'ActiveState=active\nSubState=active'
mock_stop_result=0
mock_stop_effective=1
mock_app_default_output=''
declare -a mock_app_list_outputs=()
declare -a mock_stopped_units=()
mock_app_state_directory=$(/usr/bin/mktemp -d /tmp/vibepollo-controller-test.XXXXXX) ||
  fail_test 'could not create controller test state directory'
mock_app_list_counter=$mock_app_state_directory/list-counter
/usr/bin/printf '0\n' >"$mock_app_list_counter"
cleanup_controller_test_state() {
  /usr/bin/find "$mock_app_state_directory" -mindepth 1 -maxdepth 1 -type f -delete
  /usr/bin/rmdir "$mock_app_state_directory"
}
trap cleanup_controller_test_state EXIT
mock_reset_app_state() {
  /usr/bin/find "$mock_app_state_directory" -mindepth 1 -maxdepth 1 -type f \
    ! -name list-counter -delete
  /usr/bin/printf '0\n' >"$mock_app_list_counter"
}
mock_set_app_state() {
  [[ "$1" == vibepollo-app-*.service && "$2" =~ ^(active|inactive|failed|failed-running|absent)$ ]] ||
    fail_test 'invalid mock application state'
  /usr/bin/printf '%s\n' "$2" >"$mock_app_state_directory/$1"
}
mock_get_app_state() {
  local path=$mock_app_state_directory/$1
  [[ -f "$path" ]] && /usr/bin/cat "$path" || /usr/bin/printf 'absent\n'
}
user_systemctl() {
  shift 4
  case "$1" in
    show)
      if [[ "$2" == vibepollo-app-* ]]; then
        case "$(mock_get_app_state "$2")" in
          active)
            /usr/bin/printf '%s\n' \
              'LoadState=loaded' 'ActiveState=active' 'SubState=running' \
              'MainPID=1234' 'ControlGroup='
            ;;
          inactive)
            /usr/bin/printf '%s\n' \
              'LoadState=loaded' 'ActiveState=inactive' 'SubState=dead' \
              'MainPID=0' 'ControlGroup='
            ;;
          failed)
            /usr/bin/printf '%s\n' \
              'LoadState=loaded' 'ActiveState=failed' 'SubState=failed' \
              'MainPID=0' 'ControlGroup='
            ;;
          failed-running)
            /usr/bin/printf '%s\n' \
              'LoadState=loaded' 'ActiveState=failed' 'SubState=failed' \
              'MainPID=1234' 'ControlGroup='
            ;;
          absent) return 1 ;;
          *) return 1 ;;
        esac
      else
        /usr/bin/printf '%s\n' "$mock_unit_properties"
      fi
      ;;
    list-units)
      [[ "${*: -1}" == "vibepollo-app-42-*.service" ]] || return 1
      local output=$mock_app_default_output line listed_unit list_index
      list_index=$(<"$mock_app_list_counter")
      if ((list_index < ${#mock_app_list_outputs[@]})); then
        output=${mock_app_list_outputs[$list_index]}
      fi
      /usr/bin/printf '%s\n' "$((list_index + 1))" >"$mock_app_list_counter"
      while IFS= read -r line || [[ -n "$line" ]]; do
        [[ -n "$line" ]] || continue
        read -r listed_unit _ <<<"$line"
        [[ "$listed_unit" == '●' ]] && read -r _ listed_unit _ <<<"$line"
        [[ -f "$mock_app_state_directory/$listed_unit" ]] || mock_set_app_state "$listed_unit" active
      done <<<"$output"
      /usr/bin/printf '%s' "$output"
      ;;
    --no-block)
      [[ "$2" == stop && $# == 3 && "$3" == vibepollo-app-* ]] || return 1
      mock_stopped_units+=("$3")
      ((mock_stop_effective)) && mock_set_app_state "$3" absent
      return "$mock_stop_result"
      ;;
    is-active)
      [[ "${*: -1}" == vibepollo-app-* ]] || return 1
      case "$(mock_get_app_state "${*: -1}")" in
        absent) return 4 ;;
        inactive) return 3 ;;
        failed|failed-running) return 3 ;;
        active) return 0 ;;
        *) return 1 ;;
      esac
      ;;
    *) return 1 ;;
  esac
}
user_unit_running graphical-session.target || fail_test 'active target without MainPID was rejected'
if user_unit_running plasma-kwin_wayland.service; then
  fail_test 'service without MainPID was accepted'
fi
mock_unit_properties=$'ActiveState=active\nSubState=running\nMainPID=1234'
user_unit_running plasma-kwin_wayland.service || fail_test 'running service with MainPID was rejected'

# Type=notify makes systemd the readiness authority. Stop proof requires the
# inactive/dead state, a zero MainPID, and an absent or empty cgroup.
stopped_host_properties=$'LoadState=loaded\nActiveState=inactive\nSubState=dead\nMainPID=0\nControlGroup='
host_unit_is_stopped "$stopped_host_properties" || fail_test 'fully stopped host state was rejected'
if host_unit_is_stopped $'LoadState=loaded\nActiveState=active\nSubState=running\nMainPID=1234\nControlGroup='; then
  fail_test 'active host state was accepted as stopped'
fi
if host_unit_is_stopped $'LoadState=loaded\nActiveState=inactive\nSubState=dead\nMainPID=1234\nControlGroup='; then
  fail_test 'nonzero host MainPID was accepted as stopped'
fi

# Host stop is requested once, then the exact cgroup is polled to completion.
# The controller never escalates the GPU-owning service to SIGKILL.
mock_system_systemctl_calls_file=$mock_app_state_directory/system-systemctl-calls
: >"$mock_system_systemctl_calls_file"
mock_host_stop_requested=0
bounded_system_systemctl() {
  local deadline=$1
  shift
  [[ "$deadline" =~ ^[1-9][0-9]*$ ]] || return 1
  /usr/bin/printf '%s\n' "$*" >>"$mock_system_systemctl_calls_file"
  case "$*" in
    "show $host_service --property=LoadState --property=ActiveState --property=SubState --property=MainPID --property=ControlGroup")
      if ((mock_host_stop_requested)); then
        /usr/bin/printf '%s\n' "$stopped_host_properties"
      else
        /usr/bin/printf '%s\n' \
          'LoadState=loaded' 'ActiveState=active' 'SubState=running' \
          'MainPID=1234' 'ControlGroup='
      fi
      return 0
      ;;
    "--no-block stop $host_service") mock_host_stop_requested=1; return 0 ;;
    *) return 1 ;;
  esac
}
stop_host "$((SECONDS + 2))" || fail_test 'bounded host stop did not reach cgroup-empty state'
mapfile -t mock_system_systemctl_calls <"$mock_system_systemctl_calls_file"
[[ "${mock_system_systemctl_calls[*]}" == \
   "show $host_service --property=LoadState --property=ActiveState --property=SubState --property=MainPID --property=ControlGroup --no-block stop $host_service show $host_service --property=LoadState --property=ActiveState --property=SubState --property=MainPID --property=ControlGroup" ]] ||
  fail_test "unexpected bounded host-stop calls: ${mock_system_systemctl_calls[*]}"

# The controller passes X credentials only to target-UID helpers. The file
# itself must remain a regular file owned by that UID with no group/other mode.
candidate_uid=1000
xauthority_attributes_are_private '1000:600:regular file' || fail_test 'private Xauthority attributes were rejected'
if xauthority_attributes_are_private '1000:640:regular file'; then
  fail_test 'group-readable Xauthority attributes were accepted'
fi
if xauthority_attributes_are_private '1000:604:regular file'; then
  fail_test 'other-readable Xauthority attributes were accepted'
fi
if xauthority_attributes_are_private '1001:600:regular file'; then
  fail_test 'foreign-owned Xauthority attributes were accepted'
fi

# Transient application cleanup stops only exact-generation names, catches a
# unit that registers after an initially clean pass, and requires a sustained
# clean window before returning.
mock_reset_app_state
mock_app_default_output=''
mock_stopped_units=()
mock_stop_result=0
mock_stop_effective=1
mock_app_list_outputs=(
  $'vibepollo-app-42-101.service loaded active running first\n'
  ''
  $'vibepollo-app-42-202.service loaded activating start second\n'
  '' '' '' '' '' '' '' '' '' '' '' ''
)
stop_session_generation_apps desktop 1000 1000 /run/user/1000 42 "$((SECONDS + 4))" ||
  fail_test 'generation-scoped delayed-registration cleanup failed'
[[ "${mock_stopped_units[*]}" == 'vibepollo-app-42-101.service vibepollo-app-42-202.service' ]] ||
  fail_test "unexpected application units stopped: ${mock_stopped_units[*]}"
mock_list_count=$(<"$mock_app_list_counter")
((mock_list_count >= 13)) || fail_test 'cleanup returned without ten consecutive clean enumerations'

# A failed unit counts as stopped only with MainPID zero and an empty/absent
# cgroup; a contradictory failed state remains unsafe.
mock_set_app_state vibepollo-app-42-303.service failed
session_app_unit_is_stopped "$((SECONDS + 1))" desktop 1000 1000 /run/user/1000 \
  vibepollo-app-42-303.service || fail_test 'process-free failed unit was rejected'
mock_set_app_state vibepollo-app-42-303.service failed-running
if session_app_unit_is_stopped "$((SECONDS + 1))" desktop 1000 1000 /run/user/1000 \
     vibepollo-app-42-303.service; then
  fail_test 'failed unit with a live MainPID was accepted'
fi

# Cross-generation, duplicate, and malformed enumeration output all fail
# before any stop request is issued.
for unsafe_output in \
  $'vibepollo-app-41-303.service loaded active running stale\n' \
  $'vibepollo-app-42-404.service loaded active running first\nvibepollo-app-42-404.service loaded active running duplicate\n' \
  $'vibepollo-app-42-505.service malformed\n' \
  $'--all loaded active running option\n'; do
  mock_reset_app_state
  mock_app_list_outputs=("$unsafe_output")
  mock_app_default_output=''
  mock_stopped_units=()
  if stop_session_generation_apps desktop 1000 1000 /run/user/1000 42 "$((SECONDS + 1))"; then
    fail_test 'cleanup accepted malformed or cross-generation enumeration output'
  fi
  ((${#mock_stopped_units[@]} == 0)) || fail_test 'unsafe unit reached systemctl stop'
done

# A manager that acknowledges stop but keeps the exact unit active cannot make
# cleanup succeed; the overall deadline still bounds retries.
mock_reset_app_state
mock_app_default_output=$'vibepollo-app-42-606.service loaded active running wedged\n'
mock_app_list_outputs=()
mock_stopped_units=()
mock_stop_effective=0
cleanup_started=$SECONDS
if stop_session_generation_apps desktop 1000 1000 /run/user/1000 42 "$((SECONDS + 1))"; then
  fail_test 'cleanup accepted a unit that remained active after stop'
fi
((SECONDS - cleanup_started <= 2)) || fail_test 'wedged application cleanup exceeded its total deadline'
((${#mock_stopped_units[@]} == 1)) || fail_test 'wedged application stop was retried instead of polled'

for supported in plasmalogin plasmalogin-autologin sddm sddm-autologin; do
  desktop_service_supported "$supported" || fail_test "desktop login service $supported was rejected"
done
for unsupported in plasmalogin-greeter sddm-greeter gdm-password lightdm login sshd ''; do
  if desktop_service_supported "$unsupported"; then
    fail_test "desktop login service '$unsupported' was accepted"
  fi
done

# Batched broker observations retain exact-unit, PID and cgroup proof while
# reducing a pass over many old failed units to one systemctl invocation.
(
  batch_calls=$mock_app_state_directory/batch-calls
  : >"$batch_calls"
  batch_output=''
  bounded_system_systemctl() {
    /usr/bin/printf '%s\n' "$*" >>"$batch_calls"
    /usr/bin/printf '%s' "$batch_output"
  }
  session_app_control_group_is_empty() { [[ "$1" != /test/populated ]]; }
  declare -a batch_units=()
  declare -A stopped=()
  for ((i=1; i<=47; i++)); do
    unit="vibepollo-session-exec@$i.service"
    batch_units+=("$unit")
    batch_output+="Id=$unit"$'\nLoadState=loaded\nActiveState=failed\nSubState=failed\nMainPID=0\nControlGroup=\n\n'
  done
  collect_stopped_broker_instances stopped "$((SECONDS + 5))" "${batch_units[@]}" ||
    fail_test 'batched stopped brokers were rejected'
  ((${#stopped[@]} == 47)) || fail_test 'batch omitted stopped brokers'
  [[ $(/usr/bin/wc -l <"$batch_calls") == 1 ]] || fail_test 'broker batch made more than one query'

  unit=${batch_units[0]}
  for unsafe_properties in \
    $'LoadState=loaded\nActiveState=failed\nSubState=failed\nMainPID=1234\nControlGroup=' \
    $'LoadState=loaded\nActiveState=failed\nSubState=failed\nMainPID=0\nControlGroup=/test/populated' \
    $'LoadState=loaded\nActiveState=inactive\nSubState=dead\nControlGroup=' \
    $'LoadState=loaded\nActiveState=inactive\nSubState=dead\nMainPID=0\nMainPID=1234\nControlGroup='; do
    batch_output="Id=$unit"$'\n'"$unsafe_properties"
    collect_stopped_broker_instances stopped "$((SECONDS + 5))" "$unit" ||
      fail_test 'valid batch identity was not parsed'
    ((${#stopped[@]} == 0)) || fail_test 'unsafe broker state counted as stopped'
  done
  record="Id=$unit"$'\n'"$stopped_host_properties"
  for malformed_batch in \
    '' \
    "$record"$'\n\n'"$record" \
    "Id=vibepollo-session-exec@unexpected.service"$'\n'"$stopped_host_properties" \
    "$stopped_host_properties"; do
    batch_output=$malformed_batch
    if collect_stopped_broker_instances stopped "$((SECONDS + 5))" "$unit"; then
      fail_test 'missing, duplicate or foreign batch identity was accepted'
    fi
  done
  # An empty list needs no query and must clear results from a previous pass.
  : >"$batch_calls"
  stopped[$unit]=1
  collect_stopped_broker_instances stopped "$((SECONDS + 5))" || fail_test 'empty batch failed'
  [[ ! -s "$batch_calls" && ${#stopped[@]} == 0 ]] || fail_test 'empty batch retained stale results'

  # A failed bulk query must use the existing per-unit absence proof.
  bounded_system_systemctl() {
    shift
    case "$1" in show) return 1 ;; is-active) return 4 ;; *) return 1 ;; esac
  }
  collect_stopped_broker_instances stopped "$((SECONDS + 5))" "$unit" || fail_test 'vanished broker fallback failed'
  [[ ${stopped[$unit]:-} == 1 ]] || fail_test 'proven absent broker was not collected'
)

# A late broker still resets the ten-pass clean window, and each pass queries
# all observed units again instead of trusting an earlier stopped snapshot.
(
  broker_pass=0
  broker_stops=0
  first=vibepollo-session-exec@first.service
  late=vibepollo-session-exec@late.service
  enumerate_broker_instances() {
    local -n listed=$1
    ((broker_pass += 1))
    listed=("$first")
    ((broker_pass < 3)) || listed+=("$late")
    return 0
  }
  bounded_system_systemctl() {
    shift
    if [[ "$1" == --no-block && "$2" == stop && "$3" == "$late" ]]; then
      ((broker_stops += 1))
      return 0
    fi
    [[ "$1" == show ]] || return 1
    local item
    for item in "$@"; do
      [[ "$item" == vibepollo-session-exec@*.service ]] || continue
      /usr/bin/printf 'Id=%s\n' "$item"
      if [[ "$item" == "$late" && "$broker_stops" == 0 ]]; then
        /usr/bin/printf '%s\n\n' $'LoadState=loaded\nActiveState=active\nSubState=running\nMainPID=1234\nControlGroup='
      else
        /usr/bin/printf '%s\n\n' "$stopped_host_properties"
      fi
    done
  }
  stop_broker_instances "$((SECONDS + 5))" || fail_test 'late broker cleanup failed'
  ((broker_pass == 13 && broker_stops == 1)) || fail_test 'late broker did not reset the clean window'
)

/usr/bin/bash "$(/usr/bin/dirname "${BASH_SOURCE[0]}")/test_session_command_timeout.sh" \
  "$controller_source" || fail_test 'session command timeout regression'

/usr/bin/printf 'PASS: deterministic machine-session controller transitions\n'
