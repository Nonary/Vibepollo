#!/usr/bin/bash
# Execute controller and package policy with inert process/systemd boundaries.
set -euo pipefail
root=$1
(
source "$root/packaging/linux/vibepollo-session-controller"
fail_test() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
fixture_invocation=1234567890abcdef1234567890abcdef
fixture_state=active
fixture_wayland=wayland-1
fixture_credential_ok=1
fixture_cgroup=/user.slice/user-1000.slice/user@1000.service/session.slice/wayland-wm@hyprland.desktop.service
calls=$(mktemp)
trap 'rm -f -- "$calls"' EXIT
candidate_user=fixture candidate_uid=1000 candidate_gid=1000
candidate_home=/home/fixture candidate_runtime=/run/user/1000 candidate_role=desktop
candidate_backend=hyprland-kms candidate_compositor=$fixture_invocation-42-123
/usr/bin/setpriv() {
  case "$*" in
    *'/usr/bin/stat -c %u'*) printf '1000\n' ;;
    *) return 0 ;;
  esac
}
user_systemctl() {
  case "$*" in
    *show-environment*) printf 'WAYLAND_DISPLAY=%s\nDISPLAY=:99\nXAUTHORITY=/arbitrary/stale/path\n' "$fixture_wayland" ;;
    *wayland-wm@hyprland.desktop.service*)
      printf 'ActiveState=%s\nSubState=running\nMainPID=42\nInvocationID=%s\nControlGroup=%s\n' "$fixture_state" "$fixture_invocation" "$fixture_cgroup" ;;
    *) return 1 ;;
  esac
}
xauthority_is_private() { ((fixture_credential_ok)); }
user_unit_running() {
  printf '%s\n' "$1" >>"$calls"
  case "$1" in graphical-session.target|wayland-session@hyprland.desktop.target) return 0 ;; *) return 1 ;; esac
}
session_command_bounded() {
  case "$*" in *'/usr/bin/python3 -I -c'*) printf 'socket-peer\n' >>"$calls"; printf '42-123\n' ;;
    *'/usr/bin/wayland-info'*) printf 'wayland-info\n' >>"$calls" ;;
    *) printf 'unexpected\n' >>"$calls" ;;
  esac
}
read_user_environment || fail_test 'published UWSM environment rejected'
[[ "$candidate_display" == wayland-1 && -z "$candidate_xdisplay" && -z "$candidate_xauthority" ]] ||
  fail_test 'Hyprland imported stale X11 credentials'
session_is_ready || fail_test 'ready fixed UWSM session rejected'
[[ $(wc -l <"$calls") == 4 ]] || fail_test 'readiness did not perform two fixed target checks, a socket peer check and a Wayland probe'
[[ $(tail -n 1 "$calls") == wayland-info ]] || fail_test 'readiness invoked a topology helper'
fixture_wayland=../../untrusted
if read_user_environment; then fail_test 'arbitrary Wayland path admitted'; fi
fixture_wayland=wayland-1
original_cgroup=$fixture_cgroup
fixture_cgroup=/user.slice/user-1001.slice/user@1001.service/session.slice/wayland-wm@hyprland.desktop.service
if session_is_ready; then fail_test 'another user unit cgroup admitted'; fi
fixture_cgroup=$original_cgroup
fixture_state=inactive
if session_is_ready; then fail_test 'stopped compositor admitted'; fi
fixture_state=active
fixture_invocation=abcdef1234567890abcdef1234567890
if session_is_ready; then fail_test 'replacement compositor accepted under old invocation'; fi
fixture_invocation=${candidate_compositor%%-*}
fixture_invocation=00000000000000000000000000000000
if hyprland_invocation; then fail_test 'zero invocation admitted'; fi
fixture_invocation=${candidate_compositor%%-*}
candidate_backend=kwin
fixture_credential_ok=0
if read_user_environment; then fail_test 'KWin missing Xauthority accepted'; fi
candidate_backend=hyprland-kms
candidate_compositor=1234567890abcdef1234567890abcdef-42-123
candidate_session=42 candidate_groups=1000 candidate_display=wayland-1
candidate_xdisplay='' candidate_xauthority=''
fixture_bound_backend=hyprland-kms
fixture_bound_compositor=$candidate_compositor
read_session_record() {
  local -n destination=$1
  destination=([session]=42 [user]=fixture [uid]=1000 [gid]=1000 [home]=/home/fixture
    [runtime]=/run/user/1000 [groups]=1000 [role]=desktop [display]=wayland-1
    [xdisplay]='' [xauthority]='' [backend]="$fixture_bound_backend" [compositor]="$fixture_bound_compositor")
}
binding_matches_candidate complete || fail_test 'exact compositor binding rejected'
fixture_bound_compositor=abcdef1234567890abcdef1234567890-42-123
if binding_matches_candidate identity; then fail_test 'same-socket compositor replacement retained host binding'; fi
fixture_bound_compositor=1234567890abcdef1234567890abcdef-43-123
if binding_matches_candidate identity; then fail_test 'watchdog child replacement retained host binding'; fi
fixture_bound_compositor=1234567890abcdef1234567890abcdef-42-124
if binding_matches_candidate identity; then fail_test 'reused peer PID retained host binding'; fi
fixture_bound_compositor=$candidate_compositor
fixture_bound_backend=kwin
if binding_matches_candidate identity; then fail_test 'backend policy switch retained host binding'; fi
printf 'PASS: Hyprland UWSM readiness, bounded environment and restart fencing\n'
)

# Exercise the installed package parser and concrete launch overrides without
# reading /etc or starting any host. Substitute stat ownership only; all file
# parsing, duplicate checks and policy preservation use production functions.
(
  fail_test() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
  source "$root/packaging/linux/vibepollo-machine-host"
  settings=$(mktemp)
  trap 'rm -f -- "$settings"' EXIT
  /usr/bin/stat() { printf 'root:root:600\n'; }
  printf 'desktop_user=fixture\n' >"$settings"
  [[ $(read_desktop_backend "$settings") == kwin ]] || fail_test 'legacy package policy did not default to KWin'
  printf 'desktop_backend=hyprland-kms\n' >>"$settings"
  [[ $(read_desktop_user "$settings") == fixture ]] || fail_test 'backend changed selected user'
  [[ $(read_desktop_backend "$settings") == hyprland-kms ]] || fail_test 'upgrade parser discarded backend selection'
  printf 'desktop_backend=kwin\n' >>"$settings"
  if read_desktop_backend "$settings" >/dev/null 2>&1; then fail_test 'duplicate backend policy accepted'; fi
  printf 'desktop_user=fixture\ndesktop_backend=other\n' >"$settings"
  if read_desktop_backend "$settings" >/dev/null 2>&1; then fail_test 'unknown backend accepted'; fi
  [[ $(hyprland_host_overrides) == $'capture=kms\nvirtual_display_mode=disabled' ]] || fail_test 'Hyprland host overrides changed'
)
printf 'PASS: package policy compatibility and explicit KMS launch options\n'

# Run the actual upgrade rewrite against an isolated policy path. Only fixed
# filesystem destinations and privilege/identity adapters are substituted;
# configure-auto and write_settings execute their production ordered path.
(
  fail_test() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
  fixture_directory=$(mktemp -d)
  trap 'rm -rf -- "$fixture_directory"' EXIT
  fixture_settings=$fixture_directory/machine.conf
  fixture_session=$fixture_directory/session.env
  source <(sed -e "s|^readonly settings_file=.*|readonly settings_file=$fixture_settings|" \
    -e "s|^readonly session_file=.*|readonly session_file=$fixture_session|" \
    "$root/packaging/linux/vibepollo-machine-host")
  require_root() { :; }
  resolve_desktop_identity() { desktop_user=$1; }
  prepare_upgrade_profile() { printf 'upgrade-profile\n' >"$fixture_directory/events"; }
  /usr/bin/stat() {
    if [[ "${@: -1}" == "$fixture_session" ]]; then printf 'root:vibepollo:640\n';
    else printf 'root:root:600\n'; fi
  }
  /usr/bin/install() { :; }
  /usr/bin/chown() { :; }
  /usr/bin/mktemp() { command mktemp --tmpdir="$fixture_directory" .machine.XXXXXX; }
  /usr/bin/getent() { printf 'fixture:x:1000:1000:Fixture:/home/fixture:/bin/bash\n'; }
  printf 'desktop_user=fixture\ndesktop_backend=hyprland-kms\n' >"$fixture_settings"
  configure_automatically
  [[ $(read_desktop_backend "$fixture_settings") == hyprland-kms ]] || fail_test 'package upgrade lost backend selection'
  [[ $(cat "$fixture_directory/events") == upgrade-profile ]] || fail_test 'package upgrade omitted profile phase'
  printf 'session=42\ngeneration=3\nuser=fixture\nuid=1000\ngid=1000\nhome=/home/fixture\nruntime=/run/user/1000\ndisplay=wayland-1\nxdisplay=\nxauthority=\ngroups=1000\nrole=desktop\nbackend=hyprland-kms\ncompositor=1234567890abcdef1234567890abcdef-42-123\n' >"$fixture_session"
  load_session || fail_test 'published Hyprland record rejected by host'
  [[ "$session_backend" == hyprland-kms && -z "$session_xdisplay" ]] || fail_test 'host did not retain explicit backend'
  printf 'session=42\ngeneration=3\nuser=fixture\nuid=1000\ngid=1000\nhome=/home/fixture\nruntime=/run/user/1000\ndisplay=wayland-1\nxdisplay=:0\nxauthority=/run/user/1000/xauth_fixture\ngroups=1000\nrole=desktop\n' >"$fixture_session"
  load_session || fail_test 'legacy KWin record rejected by upgraded host'
  [[ "$session_backend" == kwin && -z "$session_compositor" ]] || fail_test 'legacy record retained stale Hyprland identity'
)
printf 'PASS: production package upgrade rewrite and legacy/new host record transitions\n'
