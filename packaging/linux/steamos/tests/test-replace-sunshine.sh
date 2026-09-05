#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
steamos_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
script="$steamos_dir/local/replace-sunshine.sh"
bash -n "$script"
if [[ $(id -u) -eq 0 ]]; then
  printf 'SKIP: local replacement intentionally refuses root\n'
  exit 0
fi
test_root=$(mktemp -d /tmp/vibepollo-replace-test.XXXXXXXX)
trap 'rm -rf -- "$test_root"' EXIT
mkdir -p "$test_root/fakebin" "$test_root/payload/bin" "$test_root/payload/share/vibepollo/web/v2"
cp /usr/bin/env "$test_root/payload/bin/vibepollo"
touch "$test_root/payload/share/vibepollo/"{apps.json,web/index.html,web/v2/index.html}
cat > "$test_root/fakebin/systemctl" <<'EOF'
#!/usr/bin/env bash
set -eu
printf '%s\n' "$*" >> "$TEST_CASE/calls"
shift
action=$1
shift
if [[ "$*" == *app-dev.lizardbyte.app.Sunshine.service* ]]; then name=sunshine; else name=vibepollo; fi
case "$action" in
  is-enabled) cat "$TEST_CASE/$name.enabled"; [[ $(cat "$TEST_CASE/$name.enabled") == enabled ]] ;;
  is-active) [[ $(cat "$TEST_CASE/$name.active") == yes ]] ;;
  is-failed) [[ -e "$TEST_CASE/failed" && "$name" == vibepollo ]] ;;
  show) echo 4242 ;;
  enable) echo enabled > "$TEST_CASE/$name.enabled" ;;
  disable)
    echo disabled > "$TEST_CASE/$name.enabled"
    if [[ "$*" == *--now* ]]; then echo no > "$TEST_CASE/$name.active"; fi ;;
  stop) echo no > "$TEST_CASE/$name.active" ;;
  start|restart)
    echo yes > "$TEST_CASE/$name.active"
    if [[ "$name" == vibepollo && -e "$TEST_CASE/fail-restart" ]]; then exit 1; fi ;;
  daemon-reload) ;;
  *) echo "unexpected systemctl command $action" >&2; exit 1 ;;
esac
EOF
cat > "$test_root/fakebin/ss" <<'EOF'
#!/usr/bin/env bash
printf 'Listener owner check %s\n' "$*" >> "$TEST_CASE/calls"
pid=4242
[[ ! -e "$TEST_CASE/stale-listener" ]] || pid=4343
printf 'LISTEN 0 4096 127.0.0.1:47990 0.0.0.0:* users:(("host",pid=%s,fd=12))\n' "$pid"
EOF
cat > "$test_root/fakebin/readlink" <<'EOF'
#!/usr/bin/env bash
if [[ "$*" == '-f -- /proc/4242/exe' ]]; then
  /usr/bin/readlink -f "$TEST_CASE/home/data/vibepollo-steamos/current/bin/vibepollo"
else
  /usr/bin/readlink "$@"
fi
EOF
cat > "$test_root/fakebin/curl" <<'EOF'
#!/usr/bin/env bash
printf 'HTTPS readiness\n' >> "$TEST_CASE/calls"
[[ ! -e "$TEST_CASE/fail-https" ]]
EOF
chmod +x "$test_root/fakebin/"*
new_case() {
  case_dir="$test_root/$1"
  mkdir -p "$case_dir/home/config/sunshine/credentials" "$case_dir/home/data" "$case_dir/driver/lib/dri"
  printf enabled > "$case_dir/sunshine.enabled"
  printf yes > "$case_dir/sunshine.active"
  printf disabled > "$case_dir/vibepollo.enabled"
  printf no > "$case_dir/vibepollo.active"
  source_config="$case_dir/home/config/sunshine"
  target_config="$case_dir/home/config/vibepollo"
  printf 'certificate\n' > "$source_config/credentials/cacert.pem"
  printf 'private key\n' > "$source_config/credentials/cakey.pem"
  printf '{"root":{"uniqueid":"paired-machine","named_devices":[{"name":"client"}]},"username":"test","password":"hash"}\n' > "$source_config/sunshine_state.json"
  cat > "$source_config/sunshine.conf" <<EOF
sunshine_name = Deck
encoder = vaapi
adapter_name = /dev/dri/renderD128
capture = kwin
output_name = eDP-1
file_state = $source_config/sunshine_state.json
pkey = ../sunshine/credentials/cakey.pem
cert = credentials/cacert.pem
file_apps = $source_config/apps.json
EOF
  printf '{"apps":[{"name":"Desktop","image-path":"%s/cover.png","cmd":"echo keep"}]}\n' "$source_config" > "$source_config/apps.json"
  cp -a "$source_config" "$case_dir/original"
  printf 'LIBVA_DRIVERS_PATH=%s\nLIBVA_DRIVER_NAME=radeonsi\nVIBEPOLLO_PRIVATE_VAAPI=1\n' \
    "$case_dir/driver/lib/dri" > "$case_dir/runtime.env"
  test_env=("HOME=$case_dir/home" "XDG_CONFIG_HOME=$case_dir/home/config" "XDG_DATA_HOME=$case_dir/home/data" \
    "PATH=$test_root/fakebin:/usr/bin:/bin" "TEST_CASE=$case_dir")
}
run_replace() { env "${test_env[@]}" bash "$script" --payload "$test_root/payload" "$@"; }
assert_source_preserved() { diff -r "$case_dir/original" "$source_config"; }
assert_rolled_back() {
  [[ ! -e "$target_config" ]]
  [[ $(cat "$case_dir/sunshine.enabled") == enabled && $(cat "$case_dir/sunshine.active") == yes ]]
  [[ $(cat "$case_dir/vibepollo.enabled") == disabled && $(cat "$case_dir/vibepollo.active") == no ]]
  assert_source_preserved
}

new_case success
printf 'port = 48000\n' >> "$source_config/sunshine.conf"
cp "$source_config/sunshine.conf" "$case_dir/original/sunshine.conf"
run_replace --service-environment "$case_dir/runtime.env"
assert_source_preserved
[[ $(cat "$case_dir/sunshine.active") == no && $(cat "$case_dir/sunshine.enabled") == disabled ]]
[[ $(cat "$case_dir/vibepollo.active") == yes && $(cat "$case_dir/vibepollo.enabled") == enabled ]]
cmp "$source_config/sunshine_state.json" "$target_config/sunshine_state.json"
cmp "$source_config/credentials/cakey.pem" "$target_config/credentials/cakey.pem"
cmp "$source_config/credentials/cacert.pem" "$target_config/credentials/cacert.pem"
! grep -Eq '^(capture|output_name)[[:space:]]*=' "$target_config/vibepollo.conf"
grep -Eq '^encoder = vaapi$' "$target_config/vibepollo.conf"
grep -Fq "file_state = $target_config/sunshine_state.json" "$target_config/vibepollo.conf"
grep -Fq "pkey = $target_config/credentials/cakey.pem" "$target_config/vibepollo.conf"
grep -Fq "$target_config/cover.png" "$target_config/apps.json"
[[ $(stat -c %a "$target_config") == 700 ]]
backup_dirs=("$case_dir/home/data/"sunshine-before-vibepollo-*)
[[ ${#backup_dirs[@]} == 1 && $(stat -c %a "${backup_dirs[0]}") == 700 ]]
cmp "$case_dir/runtime.env" "$case_dir/home/data/vibepollo-steamos/local-runtime.env"
grep -Eq 'EnvironmentFile=' "$case_dir/home/config/systemd/user/vibepollo-steamos.service.d/90-local-runtime.conf"
grep -Eq 'sport = :48001' "$case_dir/calls"

# A profile must never be overwritten, even if it is empty.
new_case existing
mkdir "$target_config"
if run_replace; then echo 'ERROR: existing profile was accepted' >&2; exit 1; fi
[[ ! -e "$case_dir/calls" ]]
assert_source_preserved

# External credentials/state are copied once when both keys refer to one file.
new_case external
cp "$source_config/sunshine_state.json" "$case_dir/external-state.json"
sed -i '\|^file_state =|d' "$source_config/sunshine.conf"
printf 'file_state = %s\ncredentials_file = %s\n' "$case_dir/external-state.json" "$case_dir/external-state.json" >> "$source_config/sunshine.conf"
cp "$source_config/sunshine.conf" "$case_dir/original/sunshine.conf"
run_replace
assert_source_preserved
cmp "$case_dir/external-state.json" "$target_config/imported-file_state.json"
grep -Fq "credentials_file = $target_config/imported-file_state.json" "$target_config/vibepollo.conf"

# Failed restart restores the pre-staged bundle and private driver environment.
new_case rollback
env "${test_env[@]}" "$steamos_dir/install-user.sh" --payload "$test_root/payload" --no-start
printf disabled > "$case_dir/vibepollo.enabled"
old_release=$(readlink "$case_dir/home/data/vibepollo-steamos/current")
cp "$case_dir/home/.local/bin/vibepollo-steamos-session" "$case_dir/old-launcher"
mkdir -p "$case_dir/home/config/systemd/user/vibepollo-steamos.service.d"
printf '[Service]\nEnvironment=PREVIOUS=yes\n' > "$case_dir/home/config/systemd/user/vibepollo-steamos.service.d/90-local-runtime.conf"
printf previous > "$case_dir/home/data/vibepollo-steamos/local-runtime.env"
touch "$case_dir/fail-restart"
if run_replace --service-environment "$case_dir/runtime.env"; then echo 'ERROR: failed restart passed' >&2; exit 1; fi
assert_rolled_back
[[ $(readlink "$case_dir/home/data/vibepollo-steamos/current") == "$old_release" ]]
cmp "$case_dir/old-launcher" "$case_dir/home/.local/bin/vibepollo-steamos-session"
grep -Eq 'PREVIOUS=yes' "$case_dir/home/config/systemd/user/vibepollo-steamos.service.d/90-local-runtime.conf"
[[ $(cat "$case_dir/home/data/vibepollo-steamos/local-runtime.env") == previous ]]

# Being active alone is insufficient: a failed HTTPS endpoint rolls back too.
new_case readiness
touch "$case_dir/fail-https" "$case_dir/failed"
if run_replace; then echo 'ERROR: failed readiness passed' >&2; exit 1; fi
assert_rolled_back
[[ ! -e "$case_dir/home/data/vibepollo-steamos/current" ]]

# A stale Sunshine listener cannot satisfy readiness for the new service.
new_case stale
touch "$case_dir/stale-listener" "$case_dir/failed"
if run_replace; then echo 'ERROR: old listener accepted as Vibepollo' >&2; exit 1; fi
assert_rolled_back
! grep -Eq 'HTTPS readiness' "$case_dir/calls"

new_case disabled
printf disabled > "$case_dir/sunshine.enabled"
printf no > "$case_dir/sunshine.active"
touch "$case_dir/fail-restart"
if run_replace; then exit 1; fi
[[ $(cat "$case_dir/sunshine.enabled") == disabled && $(cat "$case_dir/sunshine.active") == no ]]
assert_source_preserved

# Native preflight failure must leave the active Sunshine service alone.
new_case preflight
mkdir -p "$case_dir/bad-payload"
cp -a "$test_root/payload/." "$case_dir/bad-payload/"
printf '#!/usr/bin/env bash\nexit 1\n' > "$case_dir/bad-payload/bin/vibepollo"
if env "${test_env[@]}" bash "$script" --payload "$case_dir/bad-payload"; then exit 1; fi
! grep -Eq '(disable|stop|restart|start) ' "$case_dir/calls"
assert_rolled_back

new_case injection
printf 'LD_LIBRARY_PATH=$(touch %s/injected)\n' "$case_dir" > "$case_dir/runtime.env"
if run_replace --service-environment "$case_dir/runtime.env"; then exit 1; fi
[[ ! -e "$case_dir/injected" ]]
! grep -Eq '(disable|stop|restart|start) ' "$case_dir/calls"
assert_rolled_back
new_case marker
printf 'LIBVA_DRIVERS_PATH=%s\n' "$case_dir/driver/lib/dri" > "$case_dir/runtime.env"
if run_replace --service-environment "$case_dir/runtime.env"; then exit 1; fi
! grep -Eq '(disable|stop|restart|start) ' "$case_dir/calls"
assert_rolled_back
printf 'Sunshine replacement migration and rollback tests passed\n'
