#!/usr/bin/env bash

set -Eeuo pipefail
IFS=$'\n\t'

readonly expected_user="chasep"
readonly expected_home="/home/chasep"
readonly sunshine_service="vibepollo.service"
readonly watchdog_device="/dev/watchdog0"
readonly watchdog_config="/etc/systemd/system.conf.d/50-vibepollo-reboot-watchdog.conf"

assume_yes=0
dry_run=0
stage_dir=""

usage() {
  cat <<EOF
Safely deploy the current Vibepollo build and reboot this Linux host.

Usage: $(basename "$0") [--yes] [--dry-run]

  --yes      Skip the final interactive confirmation.
  --dry-run  Stage and validate the install without changing the host or rebooting.
  -h, --help Show this help.

Run this script as ${expected_user}, without sudo. It asks for sudo once.
EOF
}

log() {
  printf '[safe-reboot] %s\n' "$*"
}

die() {
  printf '[safe-reboot] ERROR: %s\n' "$*" >&2
  exit 1
}

cleanup_stage() {
  if [[ -n "$stage_dir" && -d "$stage_dir" ]]; then
    # stage_dir is the exact path returned by mktemp. Do not broaden this target.
    find -- "$stage_dir" -depth -delete
    stage_dir=""
  fi
}

trap cleanup_stage EXIT

for arg in "$@"; do
  case "$arg" in
    --yes)
      assume_yes=1
      ;;
    --dry-run)
      dry_run=1
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      usage >&2
      die "unknown argument: $arg"
      ;;
  esac
done

[[ $EUID -ne 0 ]] || die "run this as ${expected_user}, not with sudo"
[[ $(id -un) == "$expected_user" ]] || die "this host script must be run as ${expected_user}"
[[ $HOME == "$expected_home" ]] || die "expected HOME=${expected_home}, got ${HOME}"

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
repo_root="$(cd -- "$script_dir/.." && pwd -P)"
build_dir="$repo_root/build"

required_commands=(
  cmake
  cp
  find
  getcap
  install
  mktemp
  modinfo
  modprobe
  readlink
  sed
  setcap
  sha256sum
  sort
  sudo
  sync
  systemctl
  sysctl
  wdctl
)

for command_name in "${required_commands[@]}"; do
  command -v "$command_name" >/dev/null 2>&1 || die "required command not found: $command_name"
done

[[ -f "$build_dir/cmake_install.cmake" ]] || die "missing configured build directory: $build_dir"
[[ -x "$build_dir/vibepollo" ]] || die "missing built Vibepollo executable; run cmake --build build -j10 first"

calculate_drm_source_id() {
  local source_dir=$1
  local hash_input input_hash input_name hash_material=""
  local -a hash_inputs=()

  shopt -s nullglob
  hash_inputs=("$source_dir"/*.c "$source_dir"/*.h)
  shopt -u nullglob
  hash_inputs+=(
    "$source_dir/Makefile"
    "$source_dir/build-module"
    "$source_dir/dkms.conf.in"
  )
  mapfile -t hash_inputs < <(printf '%s\n' "${hash_inputs[@]}" | LC_ALL=C sort)

  for hash_input in "${hash_inputs[@]}"; do
    [[ -f "$hash_input" && ! -L "$hash_input" ]] || \
      die "missing or unsafe DRM hash input: $hash_input"
    [[ ${hash_input##*/} != *.mod.c ]] || continue
    input_hash="$(sha256sum -- "$hash_input")"
    input_hash=${input_hash%% *}
    input_name=${hash_input#"$source_dir/"}
    hash_material+="${input_name}:${input_hash}"$'\n'
  done

  printf '%s' "$hash_material" | sha256sum | sed 's/[[:space:]].*$//'
}

if (( !dry_run && !assume_yes )); then
  cat <<EOF

This will:
  1. Install the current build from:
       $build_dir
  2. Replace the installed Vibepollo DRM source and privileged helpers.
  3. Restore Vibepollo's KMS/NVENC capabilities.
  4. Arm the Intel hardware watchdog with a two-minute reboot deadline.
  5. Stop ${sunshine_service}, disconnecting any active stream.
  6. Reboot the machine.

EOF
  read -r -p "Type REBOOT to continue: " confirmation
  [[ $confirmation == "REBOOT" ]] || die "reboot cancelled"
fi

stage_dir="$(mktemp -d "${TMPDIR:-/tmp}/vibepollo-deploy.XXXXXXXX")"
[[ -d "$stage_dir" ]] || die "failed to create deployment staging directory"

log "staging the current build"
install_log="$stage_dir/cmake-install.log"
if ! DESTDIR="$stage_dir" cmake --install "$build_dir" >"$install_log" 2>&1; then
  tail -n 200 "$install_log" >&2
  die "CMake staging install failed"
fi

stage_user_prefix="$stage_dir$expected_home/.local"
[[ -d "$stage_user_prefix" ]] || die "staged user prefix is missing: $stage_user_prefix"
[[ -x "$stage_user_prefix/bin/vibepollo" ]] || die "staged Vibepollo executable is missing"

required_system_files=(
  "$stage_dir/usr/lib/udev/rules.d/60-sunshine.rules"
  "$stage_dir/usr/lib/systemd/user/app-io.github.Nonary.vibepollo.service"
  "$stage_dir/usr/lib/modules-load.d/60-sunshine.conf"
  "$stage_dir/usr/libexec/vibeshine/vibeshine-drm-install"
  "$stage_dir/usr/libexec/vibeshine/vibeshine-vkms"
  "$stage_dir/usr/lib/vibeshine/libvibeshine-kwin-gpu.so"
  "$stage_dir/usr/libexec/vibeshine/kwin-preload/kwin_wayland"
  "$stage_dir/usr/lib/systemd/user/plasma-kwin_wayland.service.d/vibeshine-kwin-gpu.conf"
  "$stage_dir/usr/lib/systemd/system/vibeshine-drm-setup.service"
  "$stage_dir/usr/lib/systemd/system/vibeshine-vkms.service"
  "$stage_dir/usr/lib/systemd/system/vibeshine-vkms-control.socket"
  "$stage_dir/usr/lib/systemd/system/vibeshine-vkms-control@.service"
)

for staged_file in "${required_system_files[@]}"; do
  [[ -f "$staged_file" && ! -L "$staged_file" ]] || die "missing or unsafe staged system file: $staged_file"
done

shopt -s nullglob
drm_source_dirs=("$stage_dir"/usr/src/vibeshine-drm-*)
shopt -u nullglob
if (( ${#drm_source_dirs[@]} != 1 )) || [[ ! -d ${drm_source_dirs[0]} || -L ${drm_source_dirs[0]} ]]; then
  die "expected exactly one regular staged Vibepollo DRM source directory"
fi

drm_repo_source="$repo_root/third-party/libvirtualdisplay/linux/vibeshine-drm"
staged_drm_version="$(sed -n 's/^MODULE_VERSION("\([^"]*\)");$/\1/p' "${drm_source_dirs[0]}/vkms_drv.c")"
[[ -n "$staged_drm_version" ]] || die "could not read the staged DRM module version"
configured_source_id="$(sed -n 's/^MODULE_SOURCE_ID="\([0-9A-Fa-f]\{64\}\)"$/\1/p' \
  "$stage_dir/usr/libexec/vibeshine/vibeshine-drm-install")"
[[ -n "$configured_source_id" ]] || die "could not read the staged DRM installer source identity"
expected_source_id="$(calculate_drm_source_id "$drm_repo_source")"
[[ "$configured_source_id" == "$expected_source_id" ]] || \
  die "the build contains a stale DRM installer; run cmake --build build -j10 and retry"

log "validated DRM ${staged_drm_version} with source identity ${configured_source_id}"

if (( dry_run )); then
  cat <<EOF

[safe-reboot] Dry run passed. A real run will:
  - deploy DRM ${staged_drm_version} from ${drm_source_dirs[0]#"$stage_dir"}
  - set capabilities on the resolved ${expected_home}/.local/bin/vibepollo binary
  - require a working Intel TCO or ACPI hardware watchdog
  - enable and stop ${sunshine_service}
  - sync and reboot
EOF
  exit 0
fi

log "requesting administrator authentication"
sudo -v

log "installing user-local Vibepollo files"
cp -a --remove-destination "$stage_user_prefix/." "$expected_home/.local/"

log "installing root-owned system integration"
sudo install -Dm644 \
  "$stage_dir/usr/lib/udev/rules.d/60-sunshine.rules" \
  /usr/lib/udev/rules.d/60-sunshine.rules
sudo install -Dm644 \
  "$stage_dir/usr/lib/systemd/user/app-io.github.Nonary.vibepollo.service" \
  /usr/lib/systemd/user/app-io.github.Nonary.vibepollo.service
sudo install -Dm644 \
  "$stage_dir/usr/lib/modules-load.d/60-sunshine.conf" \
  /usr/lib/modules-load.d/60-sunshine.conf
drm_source_name=${drm_source_dirs[0]##*/}
drm_source_target="/usr/src/$drm_source_name"
sudo install -d -m 0755 "$drm_source_target"
sudo cp -a "${drm_source_dirs[0]}/." "$drm_source_target/"
sudo install -Dm755 \
  "$stage_dir/usr/libexec/vibeshine/vibeshine-drm-install" \
  /usr/libexec/vibeshine/vibeshine-drm-install
sudo install -Dm755 \
  "$stage_dir/usr/libexec/vibeshine/vibeshine-vkms" \
  /usr/libexec/vibeshine/vibeshine-vkms
sudo install -Dm755 \
  "$stage_dir/usr/lib/vibeshine/libvibeshine-kwin-gpu.so" \
  /usr/lib/vibeshine/libvibeshine-kwin-gpu.so
sudo install -Dm755 \
  "$stage_dir/usr/libexec/vibeshine/kwin-preload/kwin_wayland" \
  /usr/libexec/vibeshine/kwin-preload/kwin_wayland
sudo install -Dm644 \
  "$stage_dir/usr/lib/systemd/user/plasma-kwin_wayland.service.d/vibeshine-kwin-gpu.conf" \
  /usr/lib/systemd/user/plasma-kwin_wayland.service.d/vibeshine-kwin-gpu.conf

for unit in \
  vibeshine-drm-setup.service \
  vibeshine-vkms.service \
  vibeshine-vkms-control.socket \
  'vibeshine-vkms-control@.service'; do
  sudo install -Dm644 \
    "$stage_dir/usr/lib/systemd/system/$unit" \
    "/usr/lib/systemd/system/$unit"
done

sudo systemctl daemon-reload
if ! sudo systemctl restart vibeshine-drm-setup.service; then
  drm_setup_status="$(sudo systemctl show vibeshine-drm-setup.service -p ExecMainStatus --value)"
  if [[ "$drm_setup_status" == 4 ]]; then
    log "the new DRM module is installed and requires this reboot to replace the loaded module"
  else
    sudo systemctl --no-pager --full status vibeshine-drm-setup.service >&2 || true
    die "Vibepollo DRM setup failed with status ${drm_setup_status:-unknown}"
  fi
fi
sudo udevadm control --reload-rules

sunshine_link="$expected_home/.local/bin/vibepollo"
sunshine_binary="$(readlink -f -- "$sunshine_link")"
[[ -n "$sunshine_binary" && -f "$sunshine_binary" && -x "$sunshine_binary" ]] || \
  die "installed Vibepollo link does not resolve to an executable"
case "$sunshine_binary" in
  "$expected_home"/.local/bin/vibepollo-*) ;;
  *) die "refusing to set capabilities on unexpected path: $sunshine_binary" ;;
esac

log "restoring Vibepollo capabilities on $sunshine_binary"
sudo setcap cap_sys_admin,cap_sys_nice+p "$sunshine_binary"
sunshine_caps="$(getcap "$sunshine_binary")"
[[ $sunshine_caps == *cap_sys_admin* && $sunshine_caps == *cap_sys_nice* ]] || \
  die "Vibepollo capabilities did not verify: ${sunshine_caps:-none}"
log "$sunshine_caps"

installed_drm_version="$(modinfo -F version vibeshine_drm 2>/dev/null || true)"
loaded_drm_version="$(cat /sys/module/vibeshine_drm/version 2>/dev/null || true)"
[[ "$installed_drm_version" == "$staged_drm_version" ]] || \
  die "installed DRM version ${installed_drm_version:-missing} does not match staged ${staged_drm_version}; refusing to reboot"
log "DRM versions: installed=${installed_drm_version:-missing} loaded=${loaded_drm_version:-not-loaded}"

log "loading a hardware watchdog"
if ! sudo modprobe iTCO_wdt; then
  log "Intel TCO module did not load"
fi
if [[ ! -c "$watchdog_device" ]]; then
  log "Intel TCO did not expose ${watchdog_device}; trying the ACPI hardware watchdog"
  if ! sudo modprobe wdat_wdt; then
    log "ACPI hardware-watchdog module did not load"
  fi
fi
[[ -c "$watchdog_device" ]] || die "no hardware watchdog appeared; refusing an unprotected reboot"
sudo wdctl "$watchdog_device"

watchdog_config_stage="$stage_dir/50-vibepollo-reboot-watchdog.conf"
cat >"$watchdog_config_stage" <<EOF
[Manager]
RuntimeWatchdogSec=2min
RebootWatchdogSec=2min
WatchdogDevice=$watchdog_device
EOF
sudo install -Dm644 "$watchdog_config_stage" "$watchdog_config"

log "arming the runtime and reboot watchdogs"
sudo systemctl daemon-reexec
runtime_watchdog="$(systemctl show -p RuntimeWatchdogUSec --value)"
reboot_watchdog="$(systemctl show -p RebootWatchdogUSec --value)"
active_watchdog_device="$(systemctl show -p WatchdogDevice --value)"
[[ -n "$runtime_watchdog" && "$runtime_watchdog" != 0 ]] || die "runtime watchdog did not arm"
[[ -n "$reboot_watchdog" && "$reboot_watchdog" != 0 ]] || die "reboot watchdog did not arm"
[[ "$active_watchdog_device" == "$watchdog_device" ]] || \
  die "systemd selected an unexpected watchdog: ${active_watchdog_device:-none}"
log "watchdog armed: runtime=$runtime_watchdog reboot=$reboot_watchdog device=$active_watchdog_device"

log "enabling the emergency SysRq sync/remount/reboot fallback for this boot"
sudo sysctl -q -w kernel.sysrq=176

log "ensuring ${sunshine_service} starts after reboot"
systemctl --user enable "$sunshine_service"

log "stopping ${sunshine_service}; active streams will disconnect now"
systemctl --user stop "$sunshine_service"
if systemctl --user is-active --quiet "$sunshine_service"; then
  die "${sunshine_service} is still active"
fi

cleanup_stage
trap - EXIT

log "syncing filesystems"
sync

cat <<'EOF'
[safe-reboot] Rebooting now. If the screen freezes and the watchdog has not
[safe-reboot] reset the host after two minutes, use Alt+PrintScreen+S, then U,
[safe-reboot] then B, pausing briefly between keys.
EOF

sudo systemctl reboot
die "the reboot request returned unexpectedly"
