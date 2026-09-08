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
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 1 \
  -subj /CN=MigrationFixture -keyout "$test_root/client.key" -out "$test_root/client.pem" 2>/dev/null
cp /usr/bin/env "$test_root/payload/bin/vibepollo"
touch "$test_root/payload/share/vibepollo/"{apps.json,web/index.html,web/v2/index.html}
cat > "$test_root/fakebin/systemctl" <<'EOF'
#!/usr/bin/env bash
set -eu
printf '%s\n' "$*" >> "$TEST_CASE/calls"
shift
action=$1
shift
case "$*" in
  *app-dev.lizardbyte.app.Sunshine.service*) name=sunshine ;;
  *vibeshine-steamos.service*) name=vibeshine ;;
  *app-io.github.Nonary.vibeshine.service*) name=vibeshine_canonical ;;
  *sunshine.service*|*vibeshine.service*) [[ "$action" != is-enabled ]] || echo not-found; exit 1 ;;
  *) name=vibepollo ;;
esac
[[ -e "$TEST_CASE/$name.enabled" ]] || { [[ "$action" != is-enabled ]] || echo not-found; exit 1; }
case "$action" in
  is-enabled) cat "$TEST_CASE/$name.enabled"; [[ $(cat "$TEST_CASE/$name.enabled") == enabled ]] ;;
  is-active) [[ $(cat "$TEST_CASE/$name.active") == yes ]] ;;
  is-failed) [[ -e "$TEST_CASE/failed" && "$name" == vibepollo ]] ;;
  show)
    if [[ "$*" == *--property=EnvironmentFiles* ]]; then
      [[ ! -e "$TEST_CASE/ignore-environment" ]] || exit 0
      python3 - "$TEST_CASE/home/config/systemd/user/vibepollo-steamos.service.d/90-local-runtime.conf" <<'PY'
import pathlib, sys
dropin = pathlib.Path(sys.argv[1])
if dropin.exists():
    for line in dropin.read_text().splitlines():
        if line.startswith('EnvironmentFile=/'):
            print(line.partition('=')[2].replace('%%', '%') + ' (ignore_errors=no)')
PY
    elif [[ "$*" == *--property=ActiveState* ]]; then
      if [[ $(cat "$TEST_CASE/$name.active") == yes ]]; then echo active; else echo inactive; fi
    else
      if [[ $(cat "$TEST_CASE/$name.active") == yes ]]; then echo 4242; else echo 0; fi
    fi ;;
  enable) echo enabled > "$TEST_CASE/$name.enabled" ;;
  disable)
    echo disabled > "$TEST_CASE/$name.enabled"
    if [[ "$*" == *--now* ]]; then echo no > "$TEST_CASE/$name.active"; fi ;;
  stop)
    if [[ "$name" == vibepollo && -e "$TEST_CASE/fail-stop" ]]; then exit 1; fi
    echo no > "$TEST_CASE/$name.active" ;;
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
[[ "$*" =~ sport\ =\ :([0-9]+) ]] || exit 2
port=${BASH_REMATCH[1]}
pid=4242
[[ ! -e "$TEST_CASE/stale-listener" ]] || pid=4343
if [[ "$port" == "$(cat "$TEST_CASE/http.port")" ]]; then
  [[ ! -e "$TEST_CASE/ui-only" ]] || exit 0
  [[ ! -e "$TEST_CASE/stale-gamestream" ]] || pid=4343
fi
printf 'LISTEN 0 4096 127.0.0.1:%s 0.0.0.0:* users:(("host",pid=%s,fd=12))\n' "$port" "$pid"
EOF
cat > "$test_root/fakebin/readlink" <<'EOF'
#!/usr/bin/env bash
if [[ "$*" == '-f -- /proc/4242/exe' ]]; then
  /usr/bin/readlink -f "$XDG_DATA_HOME/vibepollo-steamos/current/bin/vibepollo"
else
  /usr/bin/readlink "$@"
fi
EOF
cat > "$test_root/fakebin/curl" <<'EOF'
#!/usr/bin/env bash
if [[ "$*" == *http://127.0.0.1:* ]]; then
  printf 'GameStream readiness %s\n' "$*" >> "$TEST_CASE/calls"
  [[ ! -e "$TEST_CASE/fail-serverinfo" ]]
else
  printf 'HTTPS readiness\n' >> "$TEST_CASE/calls"
  [[ ! -e "$TEST_CASE/fail-https" ]]
fi
EOF
chmod +x "$test_root/fakebin/"*
new_case() {
  case_dir="$test_root/$1"
  mkdir -p "$case_dir/home/config/sunshine/credentials" "$case_dir/home/data" "$case_dir/driver/lib/dri"
  printf enabled > "$case_dir/sunshine.enabled"
  printf yes > "$case_dir/sunshine.active"
  printf disabled > "$case_dir/vibepollo.enabled"
  printf no > "$case_dir/vibepollo.active"
  printf 47989 > "$case_dir/http.port"
  source_config="$case_dir/home/config/sunshine"
  target_config="$case_dir/home/config/vibepollo"
  printf 'certificate\n' > "$source_config/credentials/cacert.pem"
  printf 'private key\n' > "$source_config/credentials/cakey.pem"
  python3 - "$test_root/client.pem" "$source_config/sunshine_state.json" <<'PY'
import json, pathlib, sys
certificate, state = map(pathlib.Path, sys.argv[1:])
state.write_text(json.dumps({'root': {'uniqueid': 'paired-machine', 'named_devices': [{
    'name': 'client', 'uuid': '00000000-0000-0000-0000-000000000001', 'cert': certificate.read_text(),
}]}, 'username': 'test', 'password': 'hash', 'salt': 'fixture-salt'}) + '\n')
PY
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
assert_native_environment() {
  if [[ ${VIBEPOLLO_TEST_SYSTEMD_INTEGRATION:-0} != 1 ]]; then
    printf 'SKIP: set VIBEPOLLO_TEST_SYSTEMD_INTEGRATION=1 for the live user-manager parser test\n'
    return
  fi
  # Exercise systemd's actual unit-file parser. Quoted EnvironmentFile paths
  # were silently ignored on the target SteamOS even though mocked cutovers passed.
  if ! /usr/bin/systemctl --user show-environment >/dev/null 2>&1; then
    printf 'SKIP: native EnvironmentFile parser test needs a running user manager\n'
    return
  fi
  python3 - "$case_dir/home/config/systemd/user/vibepollo-steamos.service.d/90-local-runtime.conf" \
    "$1/local-runtime.env" "$test_root" <<'PY'
import json, os, pathlib, subprocess, sys, uuid
dropin, environment, root = map(pathlib.Path, sys.argv[1:])
runtime = pathlib.Path(os.environ.get('XDG_RUNTIME_DIR', f'/run/user/{os.getuid()}')) / 'systemd/user'
runtime.mkdir(parents=True, exist_ok=True)
name = f'vibepollo-environment-regression-{uuid.uuid4().hex}.service'
unit = runtime / name
probe = root / 'environment-probe.py'
output = root / 'environment-result.json'
keys = ['LIBVA_DRIVERS_PATH', 'LIBVA_DRIVER_NAME', 'VIBEPOLLO_PRIVATE_VAAPI']
probe.write_text('import json, os, pathlib, sys\n'
                 f'pathlib.Path(sys.argv[1]).write_text(json.dumps({{k: os.environ.get(k) for k in {keys!r}}}))\n')
def systemctl(*args):
    return subprocess.check_output(['/usr/bin/systemctl', '--user', *args], text=True).rstrip('\n')
try:
    unit.write_text(f'[Service]\nType=oneshot\nExecStart=/usr/bin/python3 {probe} {output}\n' + dropin.read_text())
    systemctl('daemon-reload')
    actual = systemctl('show', name, '--property=EnvironmentFiles', '--value')
    assert actual == f'{environment} (ignore_errors=no)', repr(actual)
    systemctl('start', name)
    expected = dict(line.split('=', 1) for line in environment.read_text().splitlines())
    assert json.loads(output.read_text()) == expected
finally:
    unit.unlink(missing_ok=True)
    systemctl('daemon-reload')
PY
}

new_case success
printf 'port = 48000\n' >> "$source_config/sunshine.conf"
printf 48000 > "$case_dir/http.port"
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
grep -Fxq "EnvironmentFile=$case_dir/home/data/vibepollo-steamos/local-runtime.env" \
  "$case_dir/home/config/systemd/user/vibepollo-steamos.service.d/90-local-runtime.conf"
assert_native_environment "$case_dir/home/data/vibepollo-steamos"
grep -Eq 'sport = :48001' "$case_dir/calls"
grep -Eq 'sport = :48000' "$case_dir/calls"
grep -Fq 'http://127.0.0.1:48000/serverinfo' "$case_dir/calls"

# EnvironmentFile paths retain embedded spaces and escape literal % specifiers.
new_case environment-path
spaced_data="$case_dir/home/data with spaces % specifier"
mkdir -p "$spaced_data"
test_env+=("XDG_DATA_HOME=$spaced_data")
run_replace --service-environment "$case_dir/runtime.env"
grep -Fxq "EnvironmentFile=${spaced_data//%/%%}/vibepollo-steamos/local-runtime.env" \
  "$case_dir/home/config/systemd/user/vibepollo-steamos.service.d/90-local-runtime.conf"
assert_native_environment "$spaced_data/vibepollo-steamos"
assert_source_preserved

# A manager that ignores the environment must fail before stopping Sunshine.
new_case environment-rejected
touch "$case_dir/ignore-environment"
if run_replace --service-environment "$case_dir/runtime.env"; then
  echo 'ERROR: ignored service environment was accepted' >&2
  exit 1
fi
! grep -Eq '(disable|stop|restart|start).*app-dev.lizardbyte.app.Sunshine.service' "$case_dir/calls"
assert_rolled_back

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

# Legacy aliases retain the original client identity in the copied state.
new_case duplicate-aliases
python3 - "$source_config/sunshine_state.json" <<'PY'
import copy, json, pathlib, sys
path = pathlib.Path(sys.argv[1])
state = json.loads(path.read_text())
alias = copy.deepcopy(state['root']['named_devices'][0])
alias.update(name='Legacy alias', uuid='00000000-0000-0000-0000-000000000002')
state['root']['named_devices'].append(alias)
path.write_text(json.dumps(state))
PY
cp "$source_config/sunshine_state.json" "$case_dir/original/sunshine_state.json"
run_replace
assert_source_preserved
python3 - "$source_config/sunshine_state.json" "$target_config/sunshine_state.json" <<'PY'
import json, pathlib, sys
source, target = [json.loads(pathlib.Path(path).read_text()) for path in sys.argv[1:]]
source['root']['named_devices'] = source['root']['named_devices'][:1]
assert source == target
PY
backup_dirs=("$case_dir/home/data/"sunshine-before-vibepollo-*)
cmp "$source_config/sunshine_state.json" "${backup_dirs[0]}/sunshine/sunshine_state.json"

# A disabled duplicate must not be silently merged into an enabled client.
new_case conflicting-aliases
python3 - "$source_config/sunshine_state.json" <<'PY'
import copy, json, pathlib, sys
path = pathlib.Path(sys.argv[1])
state = json.loads(path.read_text())
client = state['root']['named_devices'][0]
client['enabled'] = True
alias = copy.deepcopy(client)
alias.update(uuid='00000000-0000-0000-0000-000000000002', enabled=False)
state['root']['named_devices'].append(alias)
path.write_text(json.dumps(state))
PY
cp "$source_config/sunshine_state.json" "$case_dir/original/sunshine_state.json"
if run_replace; then echo 'ERROR: conflicting pairing aliases passed' >&2; exit 1; fi
assert_rolled_back

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

# A responsive UI cannot stand in for GameStream startup and pairing readiness.
new_case ui-only
touch "$case_dir/ui-only" "$case_dir/failed"
if run_replace; then echo 'ERROR: UI-only readiness passed' >&2; exit 1; fi
assert_rolled_back
grep -Eq 'sport = :47990' "$case_dir/calls"
grep -Eq 'sport = :47989' "$case_dir/calls"

# Ownership of the UI socket does not establish ownership of GameStream.
new_case stale-gamestream
touch "$case_dir/stale-gamestream" "$case_dir/failed"
if run_replace; then echo 'ERROR: stale GameStream listener accepted' >&2; exit 1; fi
assert_rolled_back
! grep -Eq 'GameStream readiness' "$case_dir/calls"

# A listener must also answer the discovery request successfully.
new_case broken-serverinfo
touch "$case_dir/fail-serverinfo" "$case_dir/failed"
if run_replace; then echo 'ERROR: failed serverinfo request passed' >&2; exit 1; fi
assert_rolled_back
grep -Eq 'HTTPS readiness' "$case_dir/calls"
grep -Eq 'GameStream readiness' "$case_dir/calls"

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
new_case both_ambiguous
mkdir "$case_dir/home/config/vibeshine"
if run_replace; then exit 1; fi
[[ ! -e "$case_dir/calls" ]]
assert_rolled_back

new_case vibeshine_success
mv "$source_config" "$case_dir/home/config/vibeshine"
source_config="$case_dir/home/config/vibeshine"
mv "$source_config/sunshine.conf" "$source_config/vibeshine.conf"
python3 - "$source_config/vibeshine.conf" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
path.write_text(path.read_text().replace('/sunshine/', '/vibeshine/'))
PY
cp -a "$source_config/." "$case_dir/original-vibeshine"
printf enabled > "$case_dir/vibeshine.enabled"
printf yes > "$case_dir/vibeshine.active"
run_replace
[[ $(cat "$case_dir/sunshine.active") == no && $(cat "$case_dir/vibeshine.active") == no ]]
[[ $(cat "$case_dir/vibeshine.enabled") == disabled ]]
[[ -f "$target_config/vibepollo.conf" && ! -e "$target_config/vibeshine.conf" ]]
cmp "$source_config/sunshine_state.json" "$target_config/sunshine_state.json"
diff -r "$case_dir/original-vibeshine" "$source_config"

new_case both_rollback
mkdir "$case_dir/home/config/vibeshine"
printf enabled > "$case_dir/vibeshine.enabled"
printf yes > "$case_dir/vibeshine.active"
touch "$case_dir/fail-restart"
if run_replace --source sunshine; then exit 1; fi
assert_rolled_back
[[ $(cat "$case_dir/vibeshine.active") == yes && $(cat "$case_dir/vibeshine.enabled") == enabled ]]
new_case failed_stop_retains_data
touch "$case_dir/fail-restart" "$case_dir/fail-stop"
if run_replace; then exit 1; fi
[[ -f "$target_config/vibepollo.conf" && -f "$target_config/sunshine_state.json" ]]
[[ $(cat "$case_dir/vibepollo.active") == yes && $(cat "$case_dir/sunshine.active") == no ]]
assert_source_preserved

new_case canonical_vibeshine
printf enabled > "$case_dir/vibeshine_canonical.enabled"
printf yes > "$case_dir/vibeshine_canonical.active"
run_replace
[[ $(cat "$case_dir/vibeshine_canonical.active") == no && $(cat "$case_dir/vibeshine_canonical.enabled") == disabled ]]

new_case masked_active
printf masked > "$case_dir/vibeshine.enabled"
printf yes > "$case_dir/vibeshine.active"
if run_replace; then exit 1; fi
[[ ! -e "$target_config" && $(cat "$case_dir/sunshine.active") == yes && $(cat "$case_dir/vibeshine.active") == yes ]]
printf 'Sunshine and Vibeshine replacement migration and rollback tests passed\n'
