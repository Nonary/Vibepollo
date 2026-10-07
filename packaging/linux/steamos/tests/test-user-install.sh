#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

steamos_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)

bash -n \
  "$steamos_dir/install-user.sh" \
  "$steamos_dir/uninstall-user.sh" \
  "$steamos_dir/vibepollo-steamos-session"

if [[ $(id -u) -eq 0 ]]; then
  printf 'SKIP: user installer intentionally refuses root\n'
  exit 0
fi

test_root=$(mktemp -d /tmp/vibepollo-steamos-test.XXXXXXXX)
cleanup() {
  case "$test_root" in
    /tmp/vibepollo-steamos-test.*) rm -rf -- "$test_root" ;;
  esac
}
trap cleanup EXIT INT TERM HUP

install -d \
  "$test_root/fakebin" \
  "$test_root/home" \
  "$test_root/payload/bin" \
  "$test_root/payload/share/vibepollo/web/v2" \
  "$test_root/runtime"
install -m 0755 /usr/bin/env "$test_root/payload/bin/vibepollo"
touch "$test_root/payload/share/vibepollo/apps.json" \
  "$test_root/payload/share/vibepollo/web/index.html" \
  "$test_root/payload/share/vibepollo/web/v2/index.html"
cat > "$test_root/fakebin/systemctl" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$STEAMOS_TEST_LOG"
if [[ "$*" == '--user restart vibepollo-steamos.service' && -e "$STEAMOS_TEST_FAIL" ]]; then
  rm -- "$STEAMOS_TEST_FAIL"
  exit 1
fi
EOF
chmod +x "$test_root/fakebin/systemctl"

test_env=(
  "HOME=$test_root/home"
  "USER=${USER:-deck}"
  "XDG_CONFIG_HOME=$test_root/home/config"
  "XDG_DATA_HOME=$test_root/home/data"
  "XDG_RUNTIME_DIR=$test_root/runtime"
  "PATH=$test_root/fakebin:/usr/bin:/bin"
  "STEAMOS_TEST_LOG=$test_root/systemctl.log"
  "STEAMOS_TEST_FAIL=$test_root/fail-restart"
)

# A dormant first install must not schedule a host alongside an existing service.
env "${test_env[@]}" "$steamos_dir/install-user.sh" \
  --payload "$test_root/payload" --no-enable
test -L "$test_root/home/data/vibepollo-steamos/current"
test -x "$test_root/home/.local/bin/vibepollo-steamos-session"
test -f "$test_root/home/config/systemd/user/vibepollo-steamos.service"
! grep -Eq ' (enable|restart|start) ' "$test_root/systemctl.log"

# --no-start retains its documented enable-on-next-session behavior.
env "${test_env[@]}" "$steamos_dir/install-user.sh" \
  --payload "$test_root/payload" --no-start
grep -qx -- '--user enable vibepollo-steamos.service' "$test_root/systemctl.log"
! grep -q 'restart' "$test_root/systemctl.log"
first_release=$(readlink "$test_root/home/data/vibepollo-steamos/current")

# A second install must actually restart the running host on the new payload.
env "${test_env[@]}" "$steamos_dir/install-user.sh" --payload "$test_root/payload"
second_release=$(readlink "$test_root/home/data/vibepollo-steamos/current")
test "$first_release" != "$second_release"
test -d "$test_root/home/data/vibepollo-steamos/$first_release"
grep -qx -- '--user restart vibepollo-steamos.service' "$test_root/systemctl.log"

# Failed activation restores the old release and launcher, then restarts it.
cp "$test_root/home/.local/bin/vibepollo-steamos-session" "$test_root/previous-launcher"
touch "$test_root/fail-restart"
if env "${test_env[@]}" "$steamos_dir/install-user.sh" --payload "$test_root/payload"; then
  printf 'ERROR: failed service restart was reported as success\n' >&2
  exit 1
fi
test "$(readlink "$test_root/home/data/vibepollo-steamos/current")" == "$second_release"
cmp "$test_root/previous-launcher" "$test_root/home/.local/bin/vibepollo-steamos-session"

# The launcher remembers custom XDG directories even without the install shell.
wrapper_env=$(env "${test_env[@]}" XDG_DATA_HOME="$test_root/incorrect" \
  XDG_CONFIG_HOME="$test_root/incorrect" "$test_root/home/.local/bin/vibepollo-steamos-session")
grep -qx "XDG_DATA_HOME=$test_root/home/data" <<< "$wrapper_env"
grep -qx "XDG_CONFIG_HOME=$test_root/home/config" <<< "$wrapper_env"

printf '%s\n' \
  'GAMESCOPE_WAYLAND_DISPLAY=gamescope-0' \
  'DISPLAY=:99' \
  "IGNORED=\$(touch $test_root/was-sourced)" \
  > "$test_root/runtime/gamescope-environment"

wrapper_env=$(env "${test_env[@]}" \
  "$test_root/home/.local/bin/vibepollo-steamos-session")
grep -qx 'GAMESCOPE_WAYLAND_DISPLAY=gamescope-0' <<< "$wrapper_env"
! grep -qx 'DISPLAY=:99' <<< "$wrapper_env"
grep -qx "PWD=$test_root/home/data/vibepollo-steamos/current" <<< "$wrapper_env"
test ! -e "$test_root/was-sourced"

printf '%s\n' \
  'GAMESCOPE_WAYLAND_DISPLAY=gamescope-0' \
  'GAMESCOPE_WAYLAND_DISPLAY=gamescope-1' \
  > "$test_root/runtime/gamescope-environment"
wrapper_env=$(env "${test_env[@]}" GAMESCOPE_WAYLAND_DISPLAY=stale-gamescope \
  "$test_root/home/.local/bin/vibepollo-steamos-session")
! grep -q '^GAMESCOPE_WAYLAND_DISPLAY=' <<< "$wrapper_env"

# Uninstall must preserve the user's existing configuration and pairings.
install -d "$test_root/home/config/sunshine"
printf 'pairing-state\n' > "$test_root/home/config/sunshine/sunshine_state.json"

env "${test_env[@]}" "$steamos_dir/uninstall-user.sh"
test ! -e "$test_root/home/data/vibepollo-steamos"
test ! -e "$test_root/home/config/systemd/user/vibepollo-steamos.service"
test ! -e "$test_root/home/.local/bin/vibepollo-steamos-session"
grep -qx 'pairing-state' "$test_root/home/config/sunshine/sunshine_state.json"

printf 'SteamOS user packaging tests passed\n'
