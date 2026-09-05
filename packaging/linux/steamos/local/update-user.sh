#!/usr/bin/env bash
# Transactionally update an existing native SteamOS installation.
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
umask 077

die() { printf 'update-user.sh: %s\n' "$*" >&2; exit 1; }
payload=
check=no
private_display=no
defer_start=no
while (($#)); do
  case "$1" in
    --payload) (($# >= 2)) || die '--payload requires a directory'; payload=$2; shift 2 ;;
    --check) check=yes; shift ;;
    --enable-private-display) private_display=yes; shift ;;
    --defer-start) defer_start=yes; shift ;;
    -h|--help)
      cat <<'EOF'
Usage: update-user.sh --payload DIR [--check] [--enable-private-display] [--defer-start]

Updates the existing Vibepollo user installation, preserving its profile and
paired clients. Execution restarts the service and disconnects active streams.
Failure restores the previous release, profile, launcher, and service state
when the updated host can be stopped; otherwise backups are retained untouched.
--check performs preflight only; it does not install or restart anything.
--enable-private-display enables client-driven private displays. The kernel
pool and privileged capture helper must already have been provisioned.
--defer-start stops the previous host and prepares the update for the next
boot without starting it. The existing service must be persistently enabled.
Startup and streaming remain unverified until the machine is rebooted.
EOF
      exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done
[[ $(id -u) -ne 0 ]] || die 'run as the desktop user, without sudo'
steamos_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
config_home=${XDG_CONFIG_HOME:-"$HOME/.config"}
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
[[ "$HOME" == /* && "$config_home" == /* && "$data_home" == /* ]] || die 'XDG paths must be absolute'
profile="$config_home/vibepollo"
install_root="$data_home/vibepollo-steamos"
unit=vibepollo-steamos.service
unit_file="$config_home/systemd/user/$unit"
launcher="$HOME/.local/bin/vibepollo-steamos-session"
runtime_file="$install_root/local-runtime.env"
[[ -d "$profile" && ! -L "$profile" ]] || die 'an existing regular Vibepollo profile is required'
[[ -f "$profile/vibepollo.conf" && ! -L "$profile/vibepollo.conf" ]] || die 'vibepollo.conf is missing or unsafe'
[[ -L "$install_root/current" && -x "$install_root/current/bin/vibepollo" ]] || die 'the existing release is unavailable'
[[ -f "$unit_file" && ! -L "$unit_file" && -f "$launcher" && ! -L "$launcher" ]] || die 'the existing service or launcher is unavailable'
[[ -n "$payload" && -d "$payload" && ! -L "$payload" ]] || die '--payload must name a real directory'
payload=$(CDPATH= cd -- "$payload" && pwd -P)
[[ -f "$payload/bin/vibepollo" && -x "$payload/bin/vibepollo" && ! -L "$payload/bin/vibepollo" ]] || die 'payload executable is missing'
for asset in apps.json web/index.html web/v2/index.html; do
  [[ -f "$payload/share/vibepollo/$asset" ]] || die "payload is missing $asset"
done
for command in python3 systemctl journalctl curl timeout flock ss readlink; do
  command -v "$command" >/dev/null || die "missing $command"
done
work=$(mktemp -d /tmp/vibepollo-update-check.XXXXXXXX)
trap 'rm -rf -- "$work"' EXIT

# Read the existing encoder environment as data; never source its contents.
python3 - "$runtime_file" "$work/runtime.env" "$profile" "$work/port" <<'PY'
import os, pathlib, re, stat, sys
runtime, target, profile, port_file = map(pathlib.Path, sys.argv[1:])
values = {}
if runtime.exists() or runtime.is_symlink():
    if runtime.is_symlink() or not runtime.is_file() or runtime.stat().st_size > 16384:
        raise SystemExit('runtime environment must be a small regular file')
    for line in runtime.read_text().splitlines():
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        key, sep, value = line.partition('=')
        if not sep or key in values or key not in {'LIBVA_DRIVERS_PATH', 'LIBVA_DRIVER_NAME', 'VIBEPOLLO_PRIVATE_VAAPI'}:
            raise SystemExit('unsupported or repeated runtime environment setting')
        valid = value == '1' if key == 'VIBEPOLLO_PRIVATE_VAAPI' else (
            re.fullmatch(r'[A-Za-z0-9_-]+', value) if key == 'LIBVA_DRIVER_NAME' else
            all(re.fullmatch(r'/[A-Za-z0-9_./+-]+', p) and os.path.isdir(p) for p in value.split(':')))
        if not valid:
            raise SystemExit('invalid runtime environment or missing encoder directory')
        values[key] = value
    if 'LIBVA_DRIVERS_PATH' in values:
        if values.get('VIBEPOLLO_PRIVATE_VAAPI') != '1':
            raise SystemExit('private encoder directory requires VIBEPOLLO_PRIVATE_VAAPI=1')
        driver = values.get('LIBVA_DRIVER_NAME', '')
        if not driver or not any((pathlib.Path(p) / (driver + '_drv_video.so')).is_file() for p in values['LIBVA_DRIVERS_PATH'].split(':')):
            raise SystemExit('the existing private encoder library is missing')
target.write_text(''.join(f'{key}={value}\n' for key, value in values.items()))
count = size = 0
for directory, dirs, files in os.walk(profile, followlinks=False):
    for name in dirs + files:
        item = pathlib.Path(directory, name)
        info = item.lstat()
        if not (stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode)):
            raise SystemExit(f'profile contains an unsupported link or special file: {item}')
        count += 1
        size += info.st_size
        if count > 10000 or size > 512 * 1024 * 1024:
            raise SystemExit('profile exceeds the local backup size limit')
ports = []
for line in (profile / 'vibepollo.conf').read_text().splitlines():
    match = re.match(r'\s*port\s*=\s*([^#]*?)(?:\s*#.*)?$', line)
    if match:
        value = match[1].strip()
        if not value.isdecimal() or not 1024 <= int(value) <= 65514:
            raise SystemExit('port must be an integer from 1024 through 65514')
        ports.append(int(value))
if len(ports) > 1:
    raise SystemExit('profile repeats the port setting')
port_file.write_text(str(ports[0] if ports else 47989))
PY
runtime_env=()
while IFS= read -r line; do runtime_env+=("$line"); done < "$work/runtime.env"
loaded_environment=$(systemctl --user show "$unit" --property=EnvironmentFiles --value)
if [[ -f "$runtime_file" ]]; then
  [[ "$loaded_environment" == "$runtime_file (ignore_errors=no)" ]] || die 'the service does not load its existing encoder environment'
elif [[ -n "$loaded_environment" ]]; then
  die 'the service references an unsupported external environment file'
fi
timeout 20 env "${runtime_env[@]}" "$payload/bin/vibepollo" --help > "$work/preflight.log" 2>&1 || {
  head -c 4096 "$work/preflight.log" >&2
  die 'payload cannot run natively; the installed service was preserved'
}
if [[ "$private_display" == yes ]]; then
  python3 - <<'PY'
import os, pathlib, stat, subprocess
helper = pathlib.Path('/opt/vibeshine-private-display/current/bin/vibeshine-kms-capture')
resolved = helper.resolve(strict=True)
if not resolved.is_relative_to('/opt/vibeshine-private-display'):
    raise SystemExit('private capture helper resolves outside its installation')
for path in (resolved, *resolved.parents):
    info = path.lstat()
    if info.st_uid != 0 or info.st_mode & 0o022 or not (stat.S_ISDIR(info.st_mode) or stat.S_ISREG(info.st_mode)):
        raise SystemExit('private capture helper or its parent is not immutable root-owned data')
info = resolved.stat()
if not stat.S_ISREG(info.st_mode) or not os.access(resolved, os.X_OK):
    raise SystemExit('private capture helper is not executable by this user')
caps = subprocess.check_output(['/usr/bin/getcap', str(resolved)], text=True).strip().split()
if caps[1:] != ['cap_sys_admin=p']:
    raise SystemExit('private capture helper has incorrect capabilities')
broker = pathlib.Path('/run/vibeshine/vkms-control.sock').lstat()
if not stat.S_ISSOCK(broker.st_mode) or broker.st_uid != 0 or broker.st_mode & 0o007:
    raise SystemExit('the managed display control socket is unavailable or unsafe')
if not any('/devices/faux/vibeshine' in str(p.resolve()) for p in pathlib.Path('/sys/class/drm').glob('card*-*')):
    raise SystemExit('the managed display kernel pool has not been provisioned')
PY
fi
gamestream_port=$(cat "$work/port")
https_port=$((gamestream_port + 1))
enabled=$(systemctl --user is-enabled "$unit" 2>/dev/null || true)
case "$enabled" in enabled|enabled-runtime|disabled|static|indirect) ;; *) die 'service is missing, masked, or has an unsupported enable state' ;; esac
if [[ "$defer_start" == yes && "$enabled" != enabled ]]; then
  die '--defer-start requires the existing service to be persistently enabled for next boot'
fi
was_active=no
systemctl --user is-active --quiet "$unit" && was_active=yes
if [[ "$check" == yes ]]; then
  printf 'Update preflight passed. No installation, profile, service, or display changed.\n'
  exit 0
fi

exec 8> "$data_home/.vibepollo-update.lock"
flock -n 8 || die 'another Vibepollo update is running'
backup=$(mktemp -d -- "$data_home/vibepollo-before-update-$(date -u +%Y%m%dT%H%M%SZ).XXXXXXXX")
previous=$(readlink -- "$install_root/current")
printf '%s\n' "$previous" > "$backup/previous-release"
cp -a -- "$unit_file" "$backup/unit"
cp -a -- "$launcher" "$backup/launcher"
printf 'active=%s\nenabled=%s\n' "$was_active" "$enabled" > "$backup/service-state"
installation_started=no
service_stopped=no
profile_saved=no
external_saved=no
success=no
rollback() {
  local status=$?
  local stopped=yes remaining_pid remaining_state
  trap - EXIT INT TERM HUP
  if [[ "$success" != yes && "$installation_started" == yes ]]; then
    printf 'Update failed; checking whether the previous Vibepollo installation can be restored. Backup: %s\n' "$backup" >&2
    set +e
    if [[ "$service_stopped" == yes ]]; then
      systemctl --user stop "$unit"
      remaining_pid=$(systemctl --user show "$unit" --property=MainPID --value) || remaining_pid=unknown
      remaining_state=$(systemctl --user show "$unit" --property=ActiveState --value) || remaining_state=unknown
      if [[ "$remaining_pid" != 0 || ( "$remaining_state" != inactive && "$remaining_state" != failed ) ]]; then
        stopped=no
        if [[ "$profile_saved" == yes || "$external_saved" == yes ]]; then
          printf 'ERROR: the updated host could not be confirmed stopped; its release and profile were left together to prevent active writes during restoration. Recovery backup: %s\n' "$backup" >&2
          rm -rf -- "$work"
          exit "$status"
        fi
        printf 'The previous host remains active or its state is unknown; restoring installation files only, without changing its profile or restarting it.\n' >&2
      fi
    fi
    ln -s -- "$previous" "$install_root/.update-rollback.$$"
    mv -Tf -- "$install_root/.update-rollback.$$" "$install_root/current"
    cp -a -- "$backup/unit" "$unit_file"
    cp -a -- "$backup/launcher" "$launcher"
    if [[ "$profile_saved" == yes ]]; then
      mv -- "$profile" "$backup/failed-profile"
      cp -a -- "$backup/profile" "$profile"
    fi
    if [[ "$external_saved" == yes ]]; then
      python3 - "$backup" <<'PY'
import json, pathlib, shutil, sys
backup = pathlib.Path(sys.argv[1])
for entry in json.loads((backup / 'external-state.json').read_text()):
    path = pathlib.Path(entry['path'])
    if entry['exists']:
        shutil.copy2(backup / entry['copy'], path)
    else:
        path.unlink(missing_ok=True)
PY
    fi
    systemctl --user daemon-reload
    if [[ "$service_stopped" == yes && "$stopped" == yes && "$was_active" == yes ]]; then
      systemctl --user start "$unit" || printf 'ERROR: previous service could not restart; inspect %s.\n' "$backup" >&2
    fi
  fi
  rm -rf -- "$work"
  exit "$status"
}
trap rollback EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
printf 'Updating Vibepollo; active streams will disconnect. Private backup: %s\n' "$backup"
installation_started=yes
"$steamos_dir/install-user.sh" --payload "$payload" --no-enable
service_stopped=yes
systemctl --user stop "$unit"
systemctl --user is-active --quiet "$unit" && die 'the previous host did not stop'
# Snapshot after the old process drains its final pairing and application state.
cp -a -- "$profile" "$backup/profile"
profile_saved=yes
python3 - "$profile" "$backup" <<'PY'
import json, pathlib, re, shutil, sys
profile, backup = map(pathlib.Path, sys.argv[1:])
mutable = {'file_state', 'file_apps', 'credentials_file', 'vibeshine_file_state'}
records = []
seen = set()
for line in (profile / 'vibepollo.conf').read_text().splitlines():
    match = re.match(r'\s*(\w+)\s*=\s*(.*?)\s*(?:#.*)?$', line)
    if not match or match[1] not in mutable or not match[2]:
        continue
    declared = pathlib.Path(match[2])
    if not declared.is_absolute():
        declared = profile / declared
    path = declared.resolve()
    if path.is_relative_to(profile.resolve()) or path in seen:
        continue
    seen.add(path)
    if declared.is_symlink() or (path.exists() and (not path.is_file() or path.stat().st_size > 64 * 1024 * 1024)):
        raise SystemExit('external application or pairing state is not a bounded regular file')
    copy = f'external-state-{len(records)}'
    exists = path.exists()
    if exists:
        shutil.copy2(path, backup / copy)
    records.append({'path': str(path), 'copy': copy, 'exists': exists})
(backup / 'external-state.json').write_text(json.dumps(records))
PY
external_saved=yes
if [[ "$private_display" == yes ]]; then
  python3 - "$profile/vibepollo.conf" <<'PY'
import os, pathlib, re, sys
path = pathlib.Path(sys.argv[1])
lines = path.read_text().splitlines(keepends=True)
settings = {}
for line in lines:
    match = re.match(r'\s*(\w+)\s*=\s*(.*?)\s*(?:#.*)?$', line)
    if match:
        settings[match[1]] = match[2]
updates = {'virtual_display_mode': 'per_client'}
# A fresh SteamOS profile has no layout setting. Keep its physical displays
# active; explicit application/client topology requests still take precedence.
if not settings.get('virtual_display_layout'):
    updates['virtual_display_layout'] = 'extended_primary'
result = []
for line in lines:
    match = re.match(r'\s*(\w+)\s*=', line)
    if not match or match[1] not in updates:
        result.append(line)
if result and not result[-1].endswith('\n'):
    result[-1] += '\n'
result.extend(f'{key} = {value}\n' for key, value in updates.items())
temporary = path.with_name('.vibepollo.conf.update')
with temporary.open('x') as stream:
    stream.write(''.join(result))
    stream.flush()
    os.fsync(stream.fileno())
os.replace(temporary, path)
PY
fi
if [[ "$defer_start" == yes ]]; then
  # Keep reconnecting clients away from a new host paired with the old kernel
  # until the caller performs its explicit reboot. Existing rollback still
  # restores and restarts the previous host on any error before this point.
  remaining_pid=$(systemctl --user show "$unit" --property=MainPID --value)
  remaining_state=$(systemctl --user show "$unit" --property=ActiveState --value)
  [[ "$remaining_pid" == 0 && ( "$remaining_state" == inactive || "$remaining_state" == failed ) ]] ||
    die 'the previous host could not be confirmed stopped for the deferred update'
  [[ $(systemctl --user is-enabled "$unit") == enabled ]] || die 'the updated service is not enabled for next boot'
  success=yes
  printf 'Vibepollo update prepared for next boot; the host remains stopped. Reboot to start it and verify streaming.\n'
  printf 'Rollback backup: %s\n' "$backup"
  exit 0
fi
service_started_at=$(date -u +'%Y-%m-%d %H:%M:%S UTC')
systemctl --user restart "$unit"
expected_executable=$(readlink -f -- "$install_root/current/bin/vibepollo")
ready_pid=
ready_once() {
  local main_pid executable listeners found port
  systemctl --user is-active --quiet "$unit" || return 1
  main_pid=$(systemctl --user show "$unit" --property=MainPID --value) || return 1
  [[ "$main_pid" =~ ^[1-9][0-9]*$ ]] || return 1
  executable=$(readlink -f -- "/proc/$main_pid/exe") || return 1
  [[ "$executable" == "$expected_executable" ]] || return 1
  for port in "$https_port" "$gamestream_port"; do
    found=no
    listeners=$(ss -H -ltnp "( sport = :$port )") || return 1
    while [[ "$listeners" =~ pid=([0-9]+), ]]; do
      [[ "${BASH_REMATCH[1]}" == "$main_pid" ]] || return 1
      found=yes
      listeners=${listeners#*"${BASH_REMATCH[0]}"}
    done
    [[ "$found" == yes ]] || return 1
  done
  ready_pid=$main_pid
  curl --noproxy '*' --insecure --fail --silent --output /dev/null --connect-timeout 1 --max-time 2 \
    "https://127.0.0.1:$https_port/" || return 1
  curl --noproxy '*' --fail --silent --output /dev/null --connect-timeout 1 --max-time 2 \
    "http://127.0.0.1:$gamestream_port/serverinfo" || return 1
  journalctl --user -u "$unit" --since "$service_started_at" --no-pager -o cat > "$backup/service-journal.log" 2>&1 || return 1
  if ! python3 - "$backup/service-journal.log" <<'PY'
import pathlib, re, sys
text = pathlib.Path(sys.argv[1]).read_text(errors='replace')
raise SystemExit(bool(re.search(r'(?im)\bFatal\b\s*\]?\s*:|segmentation fault|core dumped|Failed to initialize a video encoder|Address already in use', text)))
PY
  then
    touch "$work/fatal-startup"
    return 1
  fi
}
ready=no
deadline=$((SECONDS + 45))
stable_since=-1
stable_pid=
while ((SECONDS < deadline)); do
  if ready_once; then
    if ((stable_since < 0)) || [[ "$stable_pid" != "$ready_pid" ]]; then
      stable_since=$SECONDS
      stable_pid=$ready_pid
    fi
    if ((SECONDS - stable_since >= 5)); then ready=yes; break; fi
  else
    stable_since=-1
  fi
  [[ ! -f "$work/fatal-startup" ]] || break
  systemctl --user is-failed --quiet "$unit" && break
  sleep 1
done
if [[ "$ready" != yes ]]; then
  systemctl --user status "$unit" --no-pager --full > "$backup/service-status.log" 2>&1 || true
  die "updated host failed its startup checks; diagnostics: $backup"
fi
success=yes
printf 'Vibepollo update is active at https://localhost:%s/. Settings and paired clients were preserved.\n' "$https_port"
printf 'Rollback backup: %s\n' "$backup"
