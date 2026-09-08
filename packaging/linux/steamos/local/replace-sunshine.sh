#!/usr/bin/env bash
# Replace Sunshine or Vibeshine user services with a validated SteamOS bundle.
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
umask 077

die() { printf 'replace-sunshine.sh: %s\n' "$*" >&2; exit 1; }
payload=
service_environment=
source_host=
while (($#)); do
  case "$1" in
    --source)
      (($# >= 2)) || die '--source requires sunshine or vibeshine'
      case "$2" in sunshine|vibeshine) source_host=$2 ;; *) die '--source requires sunshine or vibeshine' ;; esac
      shift 2 ;;
    --payload|--service-environment)
      (($# >= 2)) || die "$1 requires a path"
      if [[ "$1" == --payload ]]; then payload=$2; else service_environment=$2; fi
      shift 2 ;;
    -h|--help)
      cat <<'EOF'
Usage: replace-sunshine.sh --payload DIR [--source sunshine|vibeshine] [--service-environment FILE]

Stops legacy hosts, copies the selected host's settings and pairings into a new Vibepollo profile,
and starts the supplied SteamOS bundle. An active stream will disconnect.
Existing Vibepollo profiles are refused. All original profiles are kept.
If both legacy profiles exist, --source is required; identities are never merged.
On failure, the prior installation and legacy service states are restored.
The optional environment file accepts LIBVA_DRIVERS_PATH and LIBVA_DRIVER_NAME,
with VIBEPOLLO_PRIVATE_VAAPI=1 required for a private driver path.
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
if [[ -z "$source_host" ]]; then
  for candidate in sunshine vibeshine; do
    if [[ -e "$config_home/$candidate" || -L "$config_home/$candidate" ]]; then
      [[ -z "$source_host" ]] || die 'both Sunshine and Vibeshine profiles exist; choose --source sunshine or --source vibeshine'
      source_host=$candidate
    fi
  done
fi
[[ -n "$source_host" ]] || die 'no Sunshine or Vibeshine profile found'
source_config="$config_home/$source_host"
target_config="$config_home/vibepollo"
install_root="$data_home/vibepollo-steamos"
unit_dir="$config_home/systemd/user"
launcher="$HOME/.local/bin/vibepollo-steamos-session"
dropin="$unit_dir/vibepollo-steamos.service.d/90-local-runtime.conf"
legacy_units=()
legacy_enabled=()
legacy_active=()
vibepollo_unit=vibepollo-steamos.service
[[ -d "$source_config" && ! -L "$source_config" ]] || die 'legacy profile must be a real directory'
[[ ! -e "$target_config" && ! -L "$target_config" ]] || die 'Vibepollo profile already exists; refusing to overwrite it'
[[ -n "$payload" && -d "$payload" && ! -L "$payload" ]] || die '--payload must name a real directory'
payload=$(CDPATH= cd -- "$payload" && pwd -P)
[[ -f "$payload/bin/vibepollo" && -x "$payload/bin/vibepollo" && ! -L "$payload/bin/vibepollo" ]] || die 'payload executable is missing'
for asset in apps.json web/index.html web/v2/index.html; do
  [[ -f "$payload/share/vibepollo/$asset" ]] || die "payload is missing $asset"
done
for command in python3 systemctl curl timeout flock ss; do command -v "$command" >/dev/null || die "missing $command"; done
systemctl --user is-active --quiet "$vibepollo_unit" && die 'Vibepollo is already running'
[[ ! -e "$install_root/current" || -L "$install_root/current" ]] || die 'bundle current must be a symlink'
install -d -- "$data_home"
exec 8> "$data_home/.vibepollo-sunshine-cutover.lock"
flock -n 8 || die 'another legacy-host replacement is running'
backup=$(mktemp -d -- "$data_home/$source_host-before-vibepollo-$(date -u +%Y%m%dT%H%M%SZ).XXXXXXXX")
chmod 0700 -- "$backup"
printf 'Private rollback backup: %s\n' "$backup"

# Parse data without sourcing it. Save an immutable copy for installation and
# preflight, so an edited input cannot change what the service receives.
python3 - "$service_environment" "$backup/runtime.env" <<'PY'
import os, pathlib, re, sys
source, destination = sys.argv[1:]
values = {}
if source:
    path = pathlib.Path(source)
    if path.is_symlink() or not path.is_file() or path.stat().st_size > 16384:
        raise SystemExit('runtime environment must be a small regular file')
    for line in path.read_text().splitlines():
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        key, sep, value = line.partition('=')
        if not sep or key not in {'LIBVA_DRIVERS_PATH', 'LIBVA_DRIVER_NAME', 'VIBEPOLLO_PRIVATE_VAAPI'} or key in values:
            raise SystemExit('unsupported or repeated runtime environment setting')
        if key == 'VIBEPOLLO_PRIVATE_VAAPI':
            valid = value == '1'
        elif key == 'LIBVA_DRIVER_NAME':
            valid = re.fullmatch(r'[A-Za-z0-9_-]+', value)
        else:
            valid = all(re.fullmatch(r'/[A-Za-z0-9_./+-]+', p) and os.path.isdir(p) for p in value.split(':'))
        if not valid:
            raise SystemExit('runtime environment contains an invalid value or missing directory')
        values[key] = value
if 'LIBVA_DRIVERS_PATH' in values and values.get('VIBEPOLLO_PRIVATE_VAAPI') != '1':
    raise SystemExit('private driver path requires VIBEPOLLO_PRIVATE_VAAPI=1')
pathlib.Path(destination).write_text(''.join(f'{k}={v}\n' for k, v in values.items()))
PY
runtime_env=()
while IFS= read -r line; do runtime_env+=("$line"); done < "$backup/runtime.env"
if ! timeout 20 env "${runtime_env[@]}" "$payload/bin/vibepollo" --help > "$backup/preflight.log" 2>&1; then
  die "payload cannot run natively; see $backup/preflight.log"
fi
https_port=$(python3 - "$source_config/$source_host.conf" <<'PY'
import pathlib, re, sys
ports = []
for line in pathlib.Path(sys.argv[1]).read_text().splitlines():
    match = re.match(r'\s*port\s*=\s*([^#]*?)(?:\s*#.*)?$', line)
    if match:
        value = match[1].strip()
        if not value.isdecimal() or not 1024 <= int(value) <= 65514:
            raise SystemExit('Sunshine port must be an integer from 1024 through 65514')
        ports.append(int(value))
if len(ports) > 1:
    raise SystemExit('Sunshine profile repeats the port setting')
print((ports[0] if ports else 47989) + 1)
PY
)

vibepollo_enabled=$(systemctl --user is-enabled "$vibepollo_unit" 2>/dev/null || true)
for unit in app-dev.lizardbyte.app.Sunshine.service sunshine.service vibeshine-steamos.service vibeshine.service app-io.github.Nonary.vibeshine.service; do
  enabled=$(systemctl --user is-enabled "$unit" 2>/dev/null || true)
  case "$enabled" in
    enabled|enabled-runtime|disabled|static|indirect) ;;
    ''|not-found) continue ;;
    masked|masked-runtime)
      systemctl --user is-active --quiet "$unit" && die "$unit is active but masked; stop it before replacement"
      continue ;;
    *) die "unsupported legacy service state for $unit: $enabled" ;;
  esac
  active=no
  systemctl --user is-active --quiet "$unit" && active=yes
  legacy_units+=("$unit"); legacy_enabled+=("$enabled"); legacy_active+=("$active")
  printf '%s enabled=%s active=%s\n' "$unit" "$enabled" "$active" >> "$backup/service-state"
done
previous=$(readlink -- "$install_root/current" || true)
printf '%s\n' "$previous" > "$backup/previous-release"
for pair in "launcher:$launcher" "unit:$unit_dir/$vibepollo_unit" "dropin:$dropin" "runtime:$install_root/local-runtime.env"; do
  name=${pair%%:*}; path=${pair#*:}
  [[ ! -e "$path" && ! -L "$path" ]] || cp -a -- "$path" "$backup/$name"
done
installation_started=no
cutover_started=no
migration_started=no
success=no
restore_enabled() {
  local unit=$1 state=$2
  case "$state" in
    enabled) systemctl --user enable "$unit" ;;
    enabled-runtime) systemctl --user enable --runtime "$unit" ;;
    *) systemctl --user disable "$unit" ;;
  esac
}
rollback() {
  local status=$?
  trap - EXIT INT TERM HUP
  if [[ "$success" != yes && "$installation_started" == yes ]]; then
    printf 'Cutover failed; restoring legacy hosts and the prior Vibepollo installation.\n' >&2
    set +e
    if ! systemctl --user stop "$vibepollo_unit" || systemctl --user is-active --quiet "$vibepollo_unit" ||
       [[ $(systemctl --user show "$vibepollo_unit" --property=MainPID --value) != 0 ]] ||
       [[ ! $(systemctl --user show "$vibepollo_unit" --property=ActiveState --value) =~ ^(inactive|failed)$ ]]; then
      printf 'ERROR: new host could not be stopped; retaining its profile and all backups at %s. Legacy hosts remain stopped to avoid conflicts.\n' "$backup" >&2
      exit "$status"
    fi
    systemctl --user disable "$vibepollo_unit"
    if [[ "$migration_started" == yes ]]; then rm -rf -- "$target_config"; fi
    if [[ -n "$previous" ]]; then
      ln -s -- "$previous" "$install_root/.cutover-rollback.$$"
      mv -Tf -- "$install_root/.cutover-rollback.$$" "$install_root/current"
    else
      rm -f -- "$install_root/current"
    fi
    for pair in "launcher:$launcher" "unit:$unit_dir/$vibepollo_unit" "dropin:$dropin" "runtime:$install_root/local-runtime.env"; do
      name=${pair%%:*}; path=${pair#*:}
      rm -f -- "$path"
      if [[ -e "$backup/$name" || -L "$backup/$name" ]]; then
        install -d -- "$(dirname -- "$path")"
        cp -a -- "$backup/$name" "$path"
      fi
    done
    systemctl --user daemon-reload
    restore_enabled "$vibepollo_unit" "$vibepollo_enabled"
    if [[ "$cutover_started" == yes ]]; then
      for index in "${!legacy_units[@]}"; do
        restore_enabled "${legacy_units[index]}" "${legacy_enabled[index]}"
        if [[ "${legacy_active[index]}" == yes ]]; then
          systemctl --user start "${legacy_units[index]}" || printf 'ERROR: legacy host restart failed; use the private backup at %s.\n' "$backup" >&2
        fi
      done
    fi
  fi
  exit "$status"
}
trap rollback EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
installation_started=yes
"$steamos_dir/install-user.sh" --payload "$payload" --no-enable
if [[ -n "$service_environment" ]]; then
  install -Dm600 -- "$backup/runtime.env" "$install_root/local-runtime.env"
  # EnvironmentFile takes one unquoted path, including embedded spaces.
  # Escape systemd specifiers; quotes and newlines in this local path are refused.
  [[ "$install_root" != *[$'\n\r"\\']* ]] || die 'installation path cannot be represented in the service environment'
  env_path=${install_root//%/%%}/local-runtime.env
  install -d -- "$(dirname -- "$dropin")"
  printf '[Service]\nEnvironmentFile=%s\n' "$env_path" > "$dropin"
fi
systemctl --user daemon-reload
if [[ -n "$service_environment" ]]; then
  loaded_environment=$(systemctl --user show "$vibepollo_unit" --property=EnvironmentFiles --value)
  [[ "$loaded_environment" == "$install_root/local-runtime.env (ignore_errors=no)" ]] ||
    die 'systemd did not load the private encoder environment file'
fi
cutover_started=yes
for unit in "${legacy_units[@]}"; do
  systemctl --user disable --now "$unit"
  systemctl --user is-active --quiet "$unit" && die "$unit did not stop"
done

# The source service is now stopped, so the backup includes its final pairing
# state. Keep Sunshine's original directory untouched throughout the cutover.
migration_started=yes
python3 - "$source_config" "$target_config" "$backup/$source_host" "$steamos_dir/local" "$source_host" <<'PY'
import json, os, pathlib, re, shutil, stat, sys
source, target, backup = map(pathlib.Path, sys.argv[1:4])
sys.path.insert(0, sys.argv[4])
from pairing_migration import normalize_pairing_state
count = size = 0
for directory, dirs, files in os.walk(source, followlinks=False):
    for name in dirs + files:
        item = pathlib.Path(directory, name)
        mode = item.lstat().st_mode
        if not (stat.S_ISDIR(mode) or stat.S_ISREG(mode)):
            raise SystemExit(f'profile contains a link or special file: {item}')
        count += 1
        size += item.lstat().st_size
        if count > 10000 or size > 256 * 1024 * 1024:
            raise SystemExit('profile exceeds local migration size limit')
shutil.copytree(source, backup)
backup.chmod(0o700)
shutil.copytree(backup, target)
target.chmod(0o700)
source_host = sys.argv[5]
old = target / (source_host + '.conf')
if not old.is_file():
    raise SystemExit('legacy profile has no host configuration file')
path_keys = {'file_state', 'credentials_file', 'file_apps', 'pkey', 'cert', 'log_path', 'vibeshine_file_state'}
lines = []
apps = target / 'apps.json'
pairing_states = {target / 'sunshine_state.json', target / 'vibeshine_state.json'}
external_files = {}
for line in old.read_text().splitlines(keepends=True):
    match = re.match(r'\s*([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$', line)
    if match:
        key, value = match.groups()
        if key in {'capture', 'output_name'}:
            continue  # Empty defaults select the current mode's capture/output.
        if key in path_keys and value:
            declared = pathlib.Path(value)
            if not declared.is_absolute():
                declared = source / declared
            resolved = declared.resolve()
            if resolved.is_relative_to(source.resolve()):
                line = f'{key} = {target / resolved.relative_to(source.resolve())}\n'
            else:
                # External state must not remain shared with the old service.
                if declared.exists():
                    if declared.is_symlink() or not declared.is_file() or declared.stat().st_size > 64 * 1024 * 1024:
                        raise SystemExit(f'cannot migrate external {key} file')
                    if resolved not in external_files:
                        copied = target / f'imported-{key}{declared.suffix}'
                        if copied.exists():
                            raise SystemExit(f'external {key} destination already exists')
                        shutil.copy2(declared, copied)
                        external_files[resolved] = copied
                    line = f'{key} = {external_files[resolved]}\n'
                elif key == 'log_path':
                    line = f'log_path = {target / "vibepollo.log"}\n'
                else:
                    raise SystemExit(f'configured {key} file is missing')
        if key == 'file_apps' and value:
            migrated_value = line.partition('=')[2].strip()
            apps = pathlib.Path(migrated_value)
            if not apps.is_absolute():
                apps = target / apps
        if key in {'file_state', 'vibeshine_file_state'} and value:
            pairing_state = pathlib.Path(line.partition('=')[2].strip())
            if not pairing_state.is_absolute():
                pairing_state = target / pairing_state
            pairing_states.add(pairing_state)
    lines.append(line)
(target / 'vibepollo.conf').write_text(''.join(lines))
old.unlink()
for pairing_state in pairing_states:
    if pairing_state.is_file():
        consolidated = normalize_pairing_state(pairing_state)
        if consolidated:
            print(f'Consolidated {consolidated} legacy pairing aliases; all distinct client certificates retained.')
if apps.is_file():
    def remap(value):
        if isinstance(value, str):
            return value.replace(str(source) + '/', str(target) + '/')
        if isinstance(value, list):
            return [remap(item) for item in value]
        if isinstance(value, dict):
            return {key: remap(item) for key, item in value.items()}
        return value
    original = json.loads(apps.read_text())
    migrated = remap(original)
    if migrated != original:
        apps.write_text(json.dumps(migrated, indent=2) + '\n')
PY
systemctl --user enable "$vibepollo_unit"
service_started_at=$(date -u +'%Y-%m-%d %H:%M:%S UTC')
systemctl --user restart "$vibepollo_unit"
expected_executable=$(readlink -f -- "$install_root/current/bin/vibepollo")
gamestream_port=$((https_port - 1))
ready_pid=
ready_once() {
  local main_pid executable listeners found port
  systemctl --user is-active --quiet "$vibepollo_unit" || return 1
  main_pid=$(systemctl --user show "$vibepollo_unit" --property=MainPID --value) || return 1
  [[ "$main_pid" =~ ^[1-9][0-9]*$ ]] || return 1
  executable=$(readlink -f -- "/proc/$main_pid/exe") || return 1
  [[ "$executable" == "$expected_executable" ]] || return 1
  # The config UI starts before encoder probing and pairing-state loading.
  # GameStream opens only after those startup steps, so UI readiness alone
  # could commit a cutover immediately before the host reports a fatal error.
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
  curl --noproxy '*' --insecure --fail --silent --show-error --output /dev/null --connect-timeout 1 --max-time 2 \
    "https://127.0.0.1:$https_port/" > /dev/null 2>&1 || return 1
  curl --noproxy '*' --fail --silent --show-error --output /dev/null --connect-timeout 1 --max-time 2 \
    "http://127.0.0.1:$gamestream_port/serverinfo" > /dev/null 2>&1
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
    if ((SECONDS - stable_since >= 3)); then ready=yes; break; fi
  else
    stable_since=-1
  fi
  systemctl --user is-failed --quiet "$vibepollo_unit" && break
  sleep 1
done
if [[ "$ready" != yes ]]; then
  systemctl --user status "$vibepollo_unit" --no-pager --full > "$backup/service-status.log" 2>&1 || true
  if command -v journalctl >/dev/null; then
    journalctl --user -u "$vibepollo_unit" --since "$service_started_at" --no-pager > "$backup/service-journal.log" 2>&1 || true
  fi
  die "Vibepollo service and HTTPS readiness check failed; diagnostics: $backup"
fi
success=yes
printf 'Vibepollo is active at https://localhost:%s/. %s settings and pairings were copied.\n' "$https_port" "$source_host"
printf 'Legacy user services are disabled; original profiles and private backup remain at %s.\n' "$backup"
