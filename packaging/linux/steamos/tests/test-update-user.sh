#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
steamos_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
script="$steamos_dir/local/update-user.sh"
bash -n "$script"
[[ $(id -u) -ne 0 ]] || { printf 'SKIP: updater refuses root\n'; exit 0; }
root=$(mktemp -d /tmp/vibepollo-update-tests.XXXXXXXX)
trap 'rm -rf -- "$root"' EXIT
mkdir -p "$root/fakebin" "$root/payload/bin" "$root/payload/share/vibepollo/web/v2"
cp /usr/bin/env "$root/payload/bin/vibepollo"
touch "$root/payload/share/vibepollo/"{apps.json,web/index.html,web/v2/index.html}
cat > "$root/fakebin/systemctl" <<'EOF'
#!/usr/bin/env bash
set -eu
printf '%s\n' "$*" >> "$TEST_CASE/calls"
shift
action=$1
shift
case "$action" in
  show)
    if [[ "$*" == *--property=EnvironmentFiles* ]]; then
      printf '%s/vibepollo-steamos/local-runtime.env (ignore_errors=no)\n' "$XDG_DATA_HOME"
    elif [[ "$*" == *--property=ActiveState* ]]; then
      if [[ $(cat "$TEST_CASE/active") == yes ]]; then echo active; else echo inactive; fi
    elif [[ $(cat "$TEST_CASE/active") == yes ]]; then echo 4242; else echo 0; fi ;;
  is-enabled) cat "$TEST_CASE/enabled" ;;
  is-active) [[ $(cat "$TEST_CASE/active") == yes ]] ;;
  is-failed) [[ -f "$TEST_CASE/stale-listener" ]] ;;
  stop)
    [[ ! -f "$TEST_CASE/fail-stop" ]] || exit 1
    [[ ! -f "$TEST_CASE/fail-stop-after-restart" || ! -f "$TEST_CASE/restarted" ]] || exit 1
    echo no > "$TEST_CASE/active"
    if [[ -f "$TEST_CASE/late-pairing" ]]; then
      printf 'final pairing state\n' > "$XDG_CONFIG_HOME/vibepollo/pairings.json"
      rm "$TEST_CASE/late-pairing"
    fi ;;
  start) echo yes > "$TEST_CASE/active" ;;
  restart)
    touch "$TEST_CASE/restarted"
    echo yes > "$TEST_CASE/active"
    if [[ -f "$TEST_CASE/fail-restart" ]]; then
      printf 'bad new pairing state\n' > "$XDG_CONFIG_HOME/vibepollo/pairings.json"
      printf 'bad app identities\n' > "$XDG_CONFIG_HOME/vibepollo/apps.json"
      printf 'bad external state\n' > "$TEST_CASE/external-state.json"
      exit 1
    fi ;;
  daemon-reload|status) ;;
  *) echo "unexpected systemctl command $action" >&2; exit 1 ;;
esac
EOF
cat > "$root/fakebin/ss" <<'EOF'
#!/usr/bin/env bash
set -eu
[[ "$*" =~ sport\ =\ :([0-9]+) ]] || exit 2
port=${BASH_REMATCH[1]}
pid=4242
[[ ! -f "$TEST_CASE/stale-listener" ]] || pid=4343
printf 'LISTEN 0 4096 127.0.0.1:%s 0.0.0.0:* users:(("host",pid=%s,fd=12))\n' "$port" "$pid"
EOF
cat > "$root/fakebin/readlink" <<'EOF'
#!/usr/bin/env bash
if [[ "$*" == '-f -- /proc/4242/exe' ]]; then
  /usr/bin/readlink -f "$XDG_DATA_HOME/vibepollo-steamos/current/bin/vibepollo"
else
  /usr/bin/readlink "$@"
fi
EOF
cat > "$root/fakebin/curl" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$TEST_CASE/http-calls"
exit 0
EOF
cat > "$root/fakebin/journalctl" <<'EOF'
#!/usr/bin/env bash
if [[ -f "$TEST_CASE/fatal" ]]; then echo '[Fatal]: encoder initialization failed'; fi
exit 0
EOF
cat > "$root/fakebin/python3" <<'EOF'
#!/usr/bin/env bash
set -eu
if [[ ${1:-} == - ]]; then
  code=$(cat)
  if [[ "$code" == *"helper = pathlib.Path('/opt/vibeshine-private-display/current/bin/vibeshine-kms-capture')"* ]]; then
    [[ -f "$TEST_CASE/private-ready" ]]
    exit
  fi
  if [[ -f "$TEST_CASE/fail-config" && "$code" == *"updates = {'virtual_display_mode': 'per_client'}"* ]]; then
    /usr/bin/python3 "$@" <<< "$code"
    exit 1
  fi
  exec /usr/bin/python3 "$@" <<< "$code"
fi
exec /usr/bin/python3 "$@"
EOF
chmod +x "$root/fakebin/"*
new_case() {
  case_dir="$root/$1"
  mkdir -p "$case_dir/home/config/vibepollo" "$case_dir/home/config/systemd/user" \
    "$case_dir/home/data/vibepollo-steamos/releases/original/bin" "$case_dir/home/.local/bin" "$case_dir/driver/lib/dri"
  config="$case_dir/home/config/vibepollo"
  install_root="$case_dir/home/data/vibepollo-steamos"
  printf 'enabled\n' > "$case_dir/enabled"
  printf 'yes\n' > "$case_dir/active"
  printf 'original external pairing state\n' > "$case_dir/external-state.json"
  printf 'port = 49000\nfile_state = %s/external-state.json\n' "$case_dir" > "$config/vibepollo.conf"
  printf 'original pairing state\n' > "$config/pairings.json"
  printf '{"apps":[{"name":"Hades II","uuid":"stable-app-uuid"}]}\n' > "$config/apps.json"
  cp /usr/bin/true "$install_root/releases/original/bin/vibepollo"
  ln -s releases/original "$install_root/current"
  printf 'original service\n' > "$case_dir/home/config/systemd/user/vibepollo-steamos.service"
  printf 'original launcher\n' > "$case_dir/home/.local/bin/vibepollo-steamos-session"
  touch "$case_dir/driver/lib/dri/radeonsi_drv_video.so"
  printf 'LIBVA_DRIVERS_PATH=%s/driver/lib/dri\nLIBVA_DRIVER_NAME=radeonsi\nVIBEPOLLO_PRIVATE_VAAPI=1\n' "$case_dir" > "$install_root/local-runtime.env"
  cp -a "$config" "$case_dir/original-profile"
  cp "$case_dir/external-state.json" "$case_dir/original-external"
  cp "$install_root/local-runtime.env" "$case_dir/original-runtime"
  test_env=("HOME=$case_dir/home" "XDG_CONFIG_HOME=$case_dir/home/config" "XDG_DATA_HOME=$case_dir/home/data" \
    "PATH=$root/fakebin:/usr/bin:/bin" "TEST_CASE=$case_dir")
}
run_update() { env "${test_env[@]}" bash "$script" --payload "$root/payload" "$@"; }
assert_rollback() {
  [[ $(readlink "$install_root/current") == releases/original ]]
  [[ $(cat "$case_dir/active") == yes && $(cat "$case_dir/enabled") == enabled ]]
  [[ $(cat "$case_dir/home/.local/bin/vibepollo-steamos-session") == 'original launcher' ]]
  [[ $(cat "$case_dir/home/config/systemd/user/vibepollo-steamos.service") == 'original service' ]]
  diff -r "$case_dir/original-profile" "$config"
  cmp "$case_dir/original-external" "$case_dir/external-state.json"
  cmp "$case_dir/original-runtime" "$install_root/local-runtime.env"
}

new_case check
run_update --check > "$case_dir/result"
assert_rollback
if grep -Eq -- '--user (stop|start|restart|daemon-reload|enable|disable)' "$case_dir/calls"; then exit 1; fi
[[ -z $(find "$case_dir/home/data" -maxdepth 1 -name 'vibepollo-before-update-*' -print) ]]

new_case private_missing
if run_update --check --enable-private-display > "$case_dir/result" 2>&1; then exit 1; fi
assert_rollback
if grep -Eq -- '--user (stop|start|restart|daemon-reload|enable|disable)' "$case_dir/calls"; then exit 1; fi

new_case failed_restart
touch "$case_dir/fail-restart"
if run_update > "$case_dir/result" 2>&1; then exit 1; fi
assert_rollback

new_case final_pairing
touch "$case_dir/fail-restart" "$case_dir/late-pairing"
printf 'final pairing state\n' > "$case_dir/original-profile/pairings.json"
if run_update > "$case_dir/result" 2>&1; then exit 1; fi
assert_rollback

new_case first_stop_failure
touch "$case_dir/fail-stop"
if run_update > "$case_dir/result" 2>&1; then exit 1; fi
assert_rollback
if grep -Eq -- '--user (start|restart) ' "$case_dir/calls"; then exit 1; fi
grep -q 'restoring installation files only' "$case_dir/result"

new_case candidate_cannot_stop
touch "$case_dir/fail-restart" "$case_dir/fail-stop-after-restart"
if run_update > "$case_dir/result" 2>&1; then exit 1; fi
[[ $(readlink "$install_root/current") != releases/original ]]
[[ $(cat "$case_dir/active") == yes ]]
[[ $(cat "$config/pairings.json") == 'bad new pairing state' ]]
[[ $(cat "$config/apps.json") == 'bad app identities' ]]
[[ $(cat "$case_dir/external-state.json") == 'bad external state' ]]
backup=$(find "$case_dir/home/data" -maxdepth 1 -type d -name 'vibepollo-before-update-*')
diff -r "$case_dir/original-profile" "$backup/profile"
[[ ! -e "$backup/failed-profile" ]]
if grep -Eq -- '--user start ' "$case_dir/calls"; then exit 1; fi
grep -q 'updated host could not be confirmed stopped' "$case_dir/result"

new_case fatal_journal
touch "$case_dir/fatal"
if run_update > "$case_dir/result" 2>&1; then exit 1; fi
assert_rollback

new_case stale_listener
touch "$case_dir/stale-listener"
if run_update > "$case_dir/result" 2>&1; then exit 1; fi
assert_rollback

new_case successful_private
touch "$case_dir/private-ready"
run_update --enable-private-display > "$case_dir/result" 2>&1
[[ $(readlink "$install_root/current") != releases/original ]]
[[ $(cat "$case_dir/active") == yes && $(cat "$case_dir/enabled") == enabled ]]
grep -Fx 'virtual_display_mode = per_client' "$config/vibepollo.conf"
grep -Fx 'virtual_display_layout = extended_primary' "$config/vibepollo.conf"
cmp "$case_dir/original-profile/apps.json" "$config/apps.json"
cmp "$case_dir/original-profile/pairings.json" "$config/pairings.json"
cmp "$case_dir/original-external" "$case_dir/external-state.json"
cmp "$case_dir/original-runtime" "$install_root/local-runtime.env"
grep -q 'https://127.0.0.1:49001/' "$case_dir/http-calls"
grep -q 'http://127.0.0.1:49000/serverinfo' "$case_dir/http-calls"

new_case explicit_layout
touch "$case_dir/private-ready"
printf 'virtual_display_mode = disabled\nvirtual_display_layout = extended_isolated\ndd_resolution_option = auto\n' >> "$config/vibepollo.conf"
run_update --enable-private-display > "$case_dir/result" 2>&1
grep -Fx 'virtual_display_layout = extended_isolated' "$config/vibepollo.conf"
grep -Fx 'dd_resolution_option = auto' "$config/vibepollo.conf"
[[ $(grep -c '^virtual_display_mode =' "$config/vibepollo.conf") == 1 ]]
grep -Fx 'virtual_display_mode = per_client' "$config/vibepollo.conf"

new_case deferred_private
touch "$case_dir/private-ready"
run_update --enable-private-display --defer-start > "$case_dir/result" 2>&1
[[ $(readlink "$install_root/current") != releases/original ]]
[[ $(cat "$case_dir/active") == no && $(cat "$case_dir/enabled") == enabled ]]
grep -Fx 'virtual_display_mode = per_client' "$config/vibepollo.conf"
cmp "$case_dir/original-profile/apps.json" "$config/apps.json"
cmp "$case_dir/original-profile/pairings.json" "$config/pairings.json"
[[ ! -e "$case_dir/http-calls" && ! -e "$case_dir/restarted" ]]
if grep -Eq -- '--user (start|restart) ' "$case_dir/calls"; then exit 1; fi
grep -q 'host remains stopped' "$case_dir/result"

new_case deferred_disabled
printf 'disabled\n' > "$case_dir/enabled"
if run_update --defer-start > "$case_dir/result" 2>&1; then exit 1; fi
[[ $(readlink "$install_root/current") == releases/original ]]
[[ $(cat "$case_dir/active") == yes && $(cat "$case_dir/enabled") == disabled ]]
if grep -Eq -- '--user (stop|start|restart|daemon-reload|enable|disable)' "$case_dir/calls"; then exit 1; fi
diff -r "$case_dir/original-profile" "$config"
grep -q 'persistently enabled' "$case_dir/result"

new_case deferred_failed_config
touch "$case_dir/private-ready" "$case_dir/fail-config"
if run_update --enable-private-display --defer-start > "$case_dir/result" 2>&1; then exit 1; fi
assert_rollback
[[ ! -e "$case_dir/restarted" ]]
grep -q -- '--user start vibepollo-steamos.service' "$case_dir/calls"
printf 'SteamOS update transaction: 13 scenarios PASS\n'
