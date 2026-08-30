#!/usr/bin/env bash

set -Eeuo pipefail
IFS=$'\n\t'

readonly expected_user="chasep"
readonly expected_home="/home/chasep"
readonly sunshine_service="app-io.github.Nonary.vibepollo.service"
readonly prelogin_service="vibepollo-prelogin.service"
readonly prelogin_binary="/usr/local/libexec/vibepollo-prelogin/vibepollo"
readonly prelogin_config="/var/lib/vibepollo-prelogin-static/vibepollo.conf"
readonly system_assets_dir="/usr/share/vibepollo"
readonly watchdog_device="/dev/watchdog0"
readonly watchdog_sysfs="/sys/class/watchdog/watchdog0"
readonly watchdog_config="/etc/systemd/system.conf.d/50-vibepollo-reboot-watchdog.conf"
readonly watchdog_modules_config="/etc/modules-load.d/50-vibepollo-reboot-watchdog.conf"
readonly watchdog_modprobe_config="/etc/modprobe.d/50-vibepollo-reboot-watchdog.conf"
readonly reboot_timeout_config="/etc/systemd/system/reboot.target.d/50-vibepollo-hard-deadline.conf"
readonly limine_config="/etc/default/limine"
readonly limine_generated_config="/boot/limine.conf"
readonly orderly_reboot_timeout_sec=30
readonly hard_reboot_deadline_sec=90
readonly countdown_probe_seconds=10

assume_yes=0
dry_run=0
force_reboot=0
stage_dir=""
watchdog_module=""

usage() {
  cat <<EOF
Safely deploy the current Vibepollo build and reboot this Linux host.

Usage: $(basename "$0") [--yes] [--dry-run] [--force-reboot]

  --yes      Skip the final interactive confirmation.
  --dry-run  Stage and validate the install without changing the host or rebooting.
  --force-reboot
             Restart through SysRq instead of systemd-shutdown, skipping the
             watchdog handoff entirely. Filesystems are synced and remounted
             read-only first, but no service stops cleanly. Use this when an
             orderly reboot is known to wedge.
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
    --force-reboot)
      force_reboot=1
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
  basename
  busctl
  cat
  chown
  cmake
  cp
  dirname
  find
  fuser
  getcap
  id
  install
  limine-entry-tool
  limine-update
  mktemp
  modinfo
  modprobe
  readlink
  rg
  sed
  setcap
  sha256sum
  sleep
  sort
  sudo
  sync
  systemctl
  sysctl
  tail
  tee
  test
  true
  udevadm
  wc
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

validate_watchdog_cmdline() {
  local label=$1
  local cmdline=$2
  local watchdog_token_count=0
  local word
  local -a cmdline_words=()

  IFS=' ' read -r -a cmdline_words <<<"$cmdline"
  for word in "${cmdline_words[@]}"; do
    case "$word" in
      watchdog.stop_on_reboot=0)
        ((watchdog_token_count += 1))
        ;;
      watchdog.stop_on_reboot=*)
        die "$label contains a conflicting watchdog argument: $word"
        ;;
    esac
  done
  [[ $watchdog_token_count -eq 1 ]] || \
    die "$label must contain watchdog.stop_on_reboot=0 exactly once"
}

if (( !dry_run && !assume_yes )); then
  cat <<EOF

This will:
  1. Install the current build from:
       $build_dir
  2. Replace the installed Vibepollo DRM source and privileged helpers.
  3. Update the pre-login host and restore both hosts' KMS/NVENC capabilities.
  4. Make the hardware watchdog non-stoppable across kernel reboot notifiers.
  5. Arm a non-disarmable watchdog; the hardware timeout begins after the
     kernel argument has been activated by its one-time bootstrap reboot.
  6. Stop ${sunshine_service}, disconnecting any active stream.
  7. Reboot the machine.

EOF
  if (( force_reboot )); then
    cat <<'EOF'
  NOTE: --force-reboot restarts through SysRq. Filesystems are synced and
        remounted read-only first, but services do not stop cleanly and the
        virtual-display release path does not run.

EOF
  fi
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
stage_vibepollo_binary="$(readlink -f -- "$stage_user_prefix/bin/vibepollo")"
case "$stage_vibepollo_binary" in
  "$stage_user_prefix"/bin/vibepollo-*) ;;
  *) die "staged Vibepollo link resolves outside its versioned binary directory: ${stage_vibepollo_binary:-missing}" ;;
esac
[[ -f "$stage_vibepollo_binary" && ! -L "$stage_vibepollo_binary" ]] || \
  die "staged Vibepollo target is not a regular file: $stage_vibepollo_binary"

stage_system_assets="$stage_dir$system_assets_dir"
[[ -d "$stage_system_assets" && ! -L "$stage_system_assets" ]] || \
  die "staged root-owned runtime assets are missing: $stage_system_assets"
staged_asset_symlink="$(find "$stage_system_assets" -type l -print -quit)"
[[ -z "$staged_asset_symlink" ]] || \
  die "staged runtime asset tree contains a symlink: $staged_asset_symlink"
staged_writable_asset="$(find "$stage_system_assets" -perm /022 -print -quit)"
[[ -z "$staged_writable_asset" ]] || \
  die "staged runtime asset is group- or world-writable: $staged_writable_asset"
required_runtime_assets=(
  "$stage_system_assets/apps.json"
  "$stage_system_assets/web/index.html"
  "$stage_system_assets/web/v2/index.html"
  "$stage_system_assets/shaders/opengl/ConvertUV.frag"
  "$stage_system_assets/shaders/opengl/ConvertUV.vert"
  "$stage_system_assets/shaders/opengl/ConvertY.frag"
  "$stage_system_assets/shaders/opengl/Scene.vert"
  "$stage_system_assets/shaders/opengl/Scene.frag"
)
for staged_asset in "${required_runtime_assets[@]}"; do
  [[ -f "$staged_asset" && ! -L "$staged_asset" ]] || \
    die "missing or unsafe staged runtime asset: $staged_asset"
done
if ! rg -a -Fq "$system_assets_dir/shaders/opengl/Scene.vert" "$stage_vibepollo_binary"; then
  die "the build embeds a private or stale asset path; reconfigure with -DSUNSHINE_ASSETS_DIR=$system_assets_dir and rebuild"
fi

required_system_files=(
  "$stage_dir/usr/lib/udev/rules.d/60-sunshine.rules"
  "$stage_dir/usr/lib/systemd/user/app-io.github.Nonary.vibepollo.service"
  "$stage_dir/usr/lib/modules-load.d/60-sunshine.conf"
  "$stage_dir/usr/libexec/vibeshine/vibeshine-drm-install"
  "$stage_dir/usr/libexec/vibeshine/vibeshine-vkms"
  "$stage_dir/usr/libexec/vibeshine/vibeshine-vkms-quiesce"
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
  - install the matching root-owned runtime assets under ${system_assets_dir}
  - set capabilities on the resolved ${expected_home}/.local/bin/vibepollo binary
  - update the required installed pre-login host and select native NVENC
  - add watchdog.stop_on_reboot=0 to the generated Limine kernel command line
EOF
  if (( force_reboot )); then
    cat <<EOF
  - skip the hardware watchdog setup and the whole orderly reboot path
  - enable and stop ${sunshine_service}
  - sync, remount read-only, and restart the kernel through SysRq
EOF
  else
    cat <<EOF
  - require this host's validated Intel TCO hardware watchdog
  - arm a ${hard_reboot_deadline_sec}s no-feeder hardware timeout once the new kernel argument is active
  - otherwise perform the one-time ordered bootstrap reboot that activates it
  - force the reboot transaction forward after ${orderly_reboot_timeout_sec}s if orderly shutdown stalls
  - enable and stop ${sunshine_service}
  - sync and reboot
EOF
  fi
  exit 0
fi

log "requesting administrator authentication"
sudo -v

sudo test -f "$limine_config" || die "required Limine configuration is missing: $limine_config"
sudo test ! -L "$limine_config" || die "refusing symlinked Limine configuration: $limine_config"
limine_watchdog_assignments="$(
  (sudo sed '/^[[:space:]]*#/d' "$limine_config" | \
    rg -o 'watchdog\.stop_on_reboot=[^[:space:]"]+' || true) | sort -u
)"
case "$limine_watchdog_assignments" in
  "")
    log "adding watchdog.stop_on_reboot=0 to the Limine kernel command line"
    printf '\n%s\n' 'KERNEL_CMDLINE[default]+=" watchdog.stop_on_reboot=0"' | \
      sudo tee -a "$limine_config" >/dev/null
    ;;
  watchdog.stop_on_reboot=0)
    log "Limine already preserves the hardware watchdog across reboot notifiers"
    ;;
  *)
    die "conflicting Limine watchdog arguments: $limine_watchdog_assignments"
    ;;
esac
limine_watchdog_assignments="$(
  (sudo sed '/^[[:space:]]*#/d' "$limine_config" | \
    rg -o 'watchdog\.stop_on_reboot=[^[:space:]"]+' || true) | sort -u
)"
[[ "$limine_watchdog_assignments" == watchdog.stop_on_reboot=0 ]] || \
  die "failed to persist watchdog.stop_on_reboot=0 in $limine_config"

shopt -s nullglob
kernel_pkgbase_files=(/usr/lib/modules/*/pkgbase)
shopt -u nullglob
(( ${#kernel_pkgbase_files[@]} > 0 )) || die "no installed Limine kernel entries were found"
for kernel_pkgbase_file in "${kernel_pkgbase_files[@]}"; do
  kernel_name="$(<"$kernel_pkgbase_file")"
  [[ "$kernel_name" =~ ^[A-Za-z0-9._-]+$ ]] || \
    die "unsafe kernel entry name in $kernel_pkgbase_file: $kernel_name"
  kernel_cmdline_output="$(
    sudo limine-entry-tool --get-cmdline "$kernel_name" --no-mutex --no-hooks
  )" || die "could not resolve the effective command line for $kernel_name"
  kernel_cmdline="$(printf '%s\n' "$kernel_cmdline_output" | tail -n 1)"
  validate_watchdog_cmdline "Limine entry $kernel_name" "$kernel_cmdline"
done

sudo limine-update
sudo test -f "$limine_generated_config" || \
  die "Limine did not generate $limine_generated_config"
mapfile -t generated_limine_cmdlines < <(
  sudo sed -n -E \
    's/^[[:space:]]*(kernel_)?cmdline:[[:space:]]*//p' \
    "$limine_generated_config"
)
generated_current_root_entries=0
for generated_limine_cmdline in "${generated_limine_cmdlines[@]}"; do
  generated_limine_words=()
  IFS=' ' read -r -a generated_limine_words <<<"$generated_limine_cmdline"
  for generated_limine_word in "${generated_limine_words[@]}"; do
    if [[ "$generated_limine_word" == rootflags=subvol=/@ ]]; then
      validate_watchdog_cmdline "generated current-root Limine command line" "$generated_limine_cmdline"
      ((generated_current_root_entries += 1))
      break
    fi
  done
done
(( generated_current_root_entries >= ${#kernel_pkgbase_files[@]} )) || \
  die "generated Limine configuration hardened only ${generated_current_root_entries}/${#kernel_pkgbase_files[@]} installed current-root entries"
log "verified watchdog.stop_on_reboot=0 in ${generated_current_root_entries} generated current-root Limine entries"

watchdog_stop_on_reboot="$(cat /sys/module/watchdog/parameters/stop_on_reboot 2>/dev/null || true)"
if [[ "$watchdog_stop_on_reboot" == 0 ]]; then
  watchdog_survives_reboot=1
else
  watchdog_survives_reboot=0
  log "this is the one-time bootstrap boot (live watchdog.stop_on_reboot=${watchdog_stop_on_reboot:-missing})"
  log "the new kernel argument becomes enforceable only after this reboot"
fi

log "installing user-local Vibepollo files"
cp -a --remove-destination "$stage_user_prefix/." "$expected_home/.local/"

log "installing root-owned Vibepollo runtime assets"
sudo test ! -L "$system_assets_dir" || die "refusing symlinked runtime asset root: $system_assets_dir"
sudo install -d -m 0755 "$system_assets_dir"
installed_asset_symlink="$(sudo find "$system_assets_dir" -type l -print -quit)"
[[ -z "$installed_asset_symlink" ]] || \
  die "refusing an existing runtime asset symlink: $installed_asset_symlink"
sudo cp -a --no-preserve=ownership --remove-destination \
  "$stage_system_assets/." "$system_assets_dir/"
sudo chown -R root:root "$system_assets_dir"
for staged_asset in "${required_runtime_assets[@]}"; do
  asset_relative_path=${staged_asset#"$stage_system_assets/"}
  installed_asset="$system_assets_dir/$asset_relative_path"
  sudo test -f "$installed_asset" || die "runtime asset was not installed: $installed_asset"
  sudo test ! -L "$installed_asset" || die "installed runtime asset is a symlink: $installed_asset"
  staged_asset_hash="$(sha256sum "$staged_asset" | sed 's/[[:space:]].*$//')"
  installed_asset_hash="$(sha256sum "$installed_asset" | sed 's/[[:space:]].*$//')"
  [[ "$installed_asset_hash" == "$staged_asset_hash" ]] || \
    die "installed runtime asset does not match the build: $installed_asset"
done
sudo -u plasmalogin test -r "$system_assets_dir/shaders/opengl/Scene.vert" || \
  die "the pre-login account cannot read the installed shaders"
log "verified the deployed runtime assets used by both Vibepollo hosts"

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
  "$stage_dir/usr/libexec/vibeshine/vibeshine-vkms-quiesce" \
  /usr/libexec/vibeshine/vibeshine-vkms-quiesce
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

vibepollo_link="$expected_home/.local/bin/vibepollo"
vibepollo_binary="$(readlink -f -- "$vibepollo_link")"
[[ -n "$vibepollo_binary" && -f "$vibepollo_binary" && -x "$vibepollo_binary" ]] || \
  die "installed Vibepollo link does not resolve to an executable"
case "$vibepollo_binary" in
  "$expected_home"/.local/bin/vibepollo-*) ;;
  *) die "refusing to set capabilities on unexpected path: $vibepollo_binary" ;;
esac

log "restoring Vibepollo capabilities on $vibepollo_binary"
sudo setcap cap_sys_admin,cap_sys_nice+p "$vibepollo_binary"
vibepollo_caps="$(getcap "$vibepollo_binary")"
[[ $vibepollo_caps == *cap_sys_admin* && $vibepollo_caps == *cap_sys_nice* ]] || \
  die "Vibepollo capabilities did not verify: ${vibepollo_caps:-none}"
log "$vibepollo_caps"

systemctl cat "$prelogin_service" >/dev/null 2>&1 || \
  die "required pre-login service is missing: $prelogin_service"
prelogin_exec_start="$(systemctl show "$prelogin_service" -p ExecStart --value)"
[[ "$prelogin_exec_start" == *"path=${prelogin_binary}"* ]] || \
  die "${prelogin_service} uses an unexpected executable: ${prelogin_exec_start:-missing}"
sudo systemctl enable "$prelogin_service"
prelogin_unit_state="$(systemctl show "$prelogin_service" -p UnitFileState --value)"
[[ "$prelogin_unit_state" == enabled ]] || \
  die "required pre-login service is not enabled: ${prelogin_unit_state:-unknown}"
sudo test -f "$prelogin_config" || die "required pre-login configuration is missing: $prelogin_config"
sudo test ! -L "$prelogin_config" || die "refusing symlinked pre-login configuration: $prelogin_config"

log "updating the pre-login Vibepollo host at $prelogin_binary"
sudo install -Dm755 "$vibepollo_binary" "$prelogin_binary"
sudo setcap cap_sys_admin,cap_sys_nice=ep "$prelogin_binary"
prelogin_caps="$(getcap "$prelogin_binary")"
[[ "$prelogin_caps" == *cap_sys_admin* && "$prelogin_caps" == *cap_sys_nice* ]] || \
  die "pre-login Vibepollo capabilities did not verify: ${prelogin_caps:-none}"
[[ "$(sha256sum "$prelogin_binary" | sed 's/[[:space:]].*$//')" == \
   "$(sha256sum "$vibepollo_binary" | sed 's/[[:space:]].*$//')" ]] || \
  die "pre-login Vibepollo binary does not match the deployed build"
log "$prelogin_caps"

prelogin_encoder_count="$(sudo sed -n '/^[[:space:]]*encoder[[:space:]]*=/p' "$prelogin_config" | wc -l)"
[[ "$prelogin_encoder_count" == 1 ]] || \
  die "expected exactly one encoder setting in $prelogin_config"
sudo sed -i -E \
  's/^[[:space:]]*encoder[[:space:]]*=.*/encoder = nvenc_experimental/' \
  "$prelogin_config"
sudo rg -q '^[[:space:]]*encoder[[:space:]]*=[[:space:]]*nvenc_experimental[[:space:]]*$' \
  "$prelogin_config" || die "failed to select native NVENC for the pre-login host"
log "configured the pre-login host for the known-good native NVENC DMA-BUF path"

installed_drm_version="$(modinfo -F version vibeshine_drm 2>/dev/null || true)"
loaded_drm_version="$(cat /sys/module/vibeshine_drm/version 2>/dev/null || true)"
[[ "$installed_drm_version" == "$staged_drm_version" ]] || \
  die "installed DRM version ${installed_drm_version:-missing} does not match staged ${staged_drm_version}; refusing to reboot"
log "DRM versions: installed=${installed_drm_version:-missing} loaded=${loaded_drm_version:-not-loaded}"

if (( force_reboot )); then
  log "skipping the hardware watchdog setup; --force-reboot does not rely on it"
else
  log "loading a hardware watchdog"
  if sudo modprobe iTCO_wdt nowayout=1 heartbeat=60 && [[ -c "$watchdog_device" ]]; then
    watchdog_module="iTCO_wdt"
  else
    die "the validated Intel TCO watchdog did not expose $watchdog_device"
  fi
  [[ -n "$watchdog_module" && -c "$watchdog_device" ]] || \
    die "the Intel TCO hardware watchdog is unavailable; refusing an unprotected reboot"

  watchdog_identity="$(cat "$watchdog_sysfs/identity" 2>/dev/null || true)"
  [[ "$watchdog_identity" == "$watchdog_module" ]] || \
    die "unexpected watchdog identity: ${watchdog_identity:-missing}"

  watchdog_modprobe_stage="$stage_dir/50-vibepollo-reboot-watchdog-modprobe.conf"
  printf 'options %s nowayout=1\n' "$watchdog_module" >"$watchdog_modprobe_stage"
  sudo install -Dm644 "$watchdog_modprobe_stage" "$watchdog_modprobe_config"
  log "configured ${watchdog_module} with nowayout=1 for future boots"

  watchdog_modules_stage="$stage_dir/50-vibepollo-reboot-watchdog-modules.conf"
  printf '%s\n' "$watchdog_module" >"$watchdog_modules_stage"
  sudo install -Dm644 "$watchdog_modules_stage" "$watchdog_modules_config"
  log "configured ${watchdog_module} to load automatically on future boots"

  watchdog_config_stage="$stage_dir/50-vibepollo-reboot-watchdog.conf"
  cat >"$watchdog_config_stage" <<EOF
  [Manager]
  RuntimeWatchdogSec=60s
  RebootWatchdogSec=60s
  WatchdogDevice=$watchdog_device
EOF
  sudo install -Dm644 "$watchdog_config_stage" "$watchdog_config"

  reboot_timeout_stage="$stage_dir/50-vibepollo-hard-reboot-deadline.conf"
  cat >"$reboot_timeout_stage" <<EOF
  [Unit]
  JobTimeoutSec=${orderly_reboot_timeout_sec}s
  JobTimeoutAction=reboot-force
EOF
  sudo install -Dm644 "$reboot_timeout_stage" "$reboot_timeout_config"
  log "limited the orderly reboot transaction to ${orderly_reboot_timeout_sec}s before reboot-force"

  log "making the live hardware watchdog non-disarmable"
  printf '1\n' | sudo tee "$watchdog_sysfs/nowayout" >/dev/null
  watchdog_nowayout="$(cat "$watchdog_sysfs/nowayout" 2>/dev/null || true)"
  [[ "$watchdog_nowayout" == 1 ]] || \
    die "hardware watchdog did not accept nowayout=1; refusing to reboot"

  log "arming the runtime and reboot watchdogs"
  sudo systemctl daemon-reexec
  runtime_watchdog="$(systemctl show -p RuntimeWatchdogUSec --value)"
  reboot_watchdog="$(systemctl show -p RebootWatchdogUSec --value)"
  active_watchdog_device="$(systemctl show -p WatchdogDevice --value)"
  watchdog_state="$(cat "$watchdog_sysfs/state" 2>/dev/null || true)"
  watchdog_timeout="$(cat "$watchdog_sysfs/timeout" 2>/dev/null || true)"
  [[ -n "$runtime_watchdog" && "$runtime_watchdog" != 0 ]] || die "runtime watchdog did not arm"
  [[ -n "$reboot_watchdog" && "$reboot_watchdog" != 0 ]] || die "reboot watchdog did not arm"
  [[ "$active_watchdog_device" == "$watchdog_device" ]] || \
    die "systemd selected an unexpected watchdog: ${active_watchdog_device:-none}"
  [[ "$watchdog_state" == active ]] || \
    die "hardware watchdog is not active: ${watchdog_state:-missing}"
  [[ "$watchdog_timeout" == 60 ]] || \
    die "hardware watchdog did not accept the 60s timeout: ${watchdog_timeout:-missing}"
  sudo wdctl "$watchdog_device"
  log "non-disarmable watchdog armed: runtime=$runtime_watchdog reboot=$reboot_watchdog device=$active_watchdog_device"
fi

vkms_quiesce_helper="/usr/libexec/vibeshine/vibeshine-vkms-quiesce"
vkms_exec_stop_post="$(systemctl show vibeshine-vkms.service -p ExecStopPost --value)"
vkms_before="$(systemctl show vibeshine-vkms.service -p Before --value)"
[[ "$vkms_exec_stop_post" == *"path=${vkms_quiesce_helper}"* ]] || \
  die "vibeshine-vkms.service does not have the required early-quiesce stop hook"
[[ " $vkms_before " == *" systemd-user-sessions.service "* ]] || \
  die "vibeshine-vkms.service is not ordered after graphical sessions during shutdown"
if (( !force_reboot )); then
  sudo test -w "$watchdog_sysfs/nowayout" || \
    die "hardware watchdog state is no longer writable by root"
fi
sudo test -w /sys/bus/faux/devices/vibeshine/quiesce || \
  die "Vibepollo DRM early-quiesce control is unavailable"
log "verified normal-shutdown early quiesce: $vkms_quiesce_helper"

if (( !force_reboot )); then
  reboot_job_timeout="$(systemctl show reboot.target -p JobTimeoutUSec --value)"
  reboot_job_action="$(systemctl show reboot.target -p JobTimeoutAction --value)"
  [[ "$reboot_job_timeout" == "${orderly_reboot_timeout_sec}s" ]] || \
    die "reboot.target has an unexpected job timeout: ${reboot_job_timeout:-missing}"
  [[ "$reboot_job_action" == reboot-force ]] || \
    die "reboot.target has an unexpected timeout action: ${reboot_job_action:-missing}"
  log "verified reboot.target hard progress limit: ${reboot_job_timeout}/${reboot_job_action}"
fi

log "enabling the emergency SysRq sync/remount/reboot fallback for this boot"
sudo sysctl -q -w kernel.sysrq=176

log "ensuring ${sunshine_service} starts after reboot"
systemctl --user enable "$sunshine_service"

log "stopping ${sunshine_service}; active streams will disconnect now"
systemctl --user stop "$sunshine_service"
if systemctl --user is-active --quiet "$sunshine_service"; then
  die "${sunshine_service} is still active"
fi

log "refreshing administrator authentication before the irreversible watchdog handoff"
sudo -v

cleanup_stage
trap - EXIT

log "syncing filesystems"
sync

if (( force_reboot )); then
  cat <<EOF
[safe-reboot] Restarting through SysRq. emergency_restart() skips both the
[safe-reboot] reboot-notifier chain and device_shutdown(), so nothing in the
[safe-reboot] shutdown path can wedge this -- unlike systemctl reboot --force
[safe-reboot] --force or reboot -f, which still run device_shutdown().
[safe-reboot] Filesystems are synced and remounted read-only first.
EOF

  # One root shell for the whole sequence: after the read-only remount, a
  # fresh sudo may be unable to write its timestamp, and there is no second
  # chance to issue the restart. SysRq bits: 16 sync, 32 remount, 128 reboot.
  log "sync, remount read-only, restart"
  sudo -n bash -c '
    set -u
    sysctl -q -w kernel.sysrq=176
    sync
    printf "s\n" >/proc/sysrq-trigger
    sleep 3
    printf "u\n" >/proc/sysrq-trigger
    sleep 3
    printf "b\n" >/proc/sysrq-trigger
  '

  # emergency_restart() does not return.
  sleep 30
  die "the kernel did not restart after the SysRq reboot request"
fi

if (( !watchdog_survives_reboot )); then
  cat <<EOF
[safe-reboot] Starting the one-time bootstrap reboot with the corrected KWin ->
[safe-reboot] DRM shutdown order. The kernel will activate watchdog.stop_on_reboot=0
[safe-reboot] on the next boot; this bootstrap reboot cannot honestly be called
[safe-reboot] hardware-guaranteed while the live kernel still reports ${watchdog_stop_on_reboot:-missing}.
EOF
  if ! sudo -n systemctl reboot --no-block; then
    die "the bootstrap reboot request failed"
  fi
  log "the bootstrap reboot request was accepted"
  exit 0
fi

cat <<EOF
[safe-reboot] Starting an orderly systemd reboot so KWin exits before Vibepollo
[safe-reboot] DRM quiesces. The transaction gets ${orderly_reboot_timeout_sec}s to make orderly
[safe-reboot] progress while the script arms a ${hard_reboot_deadline_sec}s no-feeder hardware timeout.
EOF

watchdog_bus="org.freedesktop.systemd1"
watchdog_object="/org/freedesktop/systemd1"
watchdog_interface="org.freedesktop.systemd1.Manager"
hard_reboot_deadline_usec=$((hard_reboot_deadline_sec * 1000000))

IFS=' ' read -r runtime_signature original_runtime_watchdog_usec <<<"$(
  busctl get-property "$watchdog_bus" "$watchdog_object" "$watchdog_interface" RuntimeWatchdogUSec
)"
IFS=' ' read -r reboot_signature original_reboot_watchdog_usec <<<"$(
  busctl get-property "$watchdog_bus" "$watchdog_object" "$watchdog_interface" RebootWatchdogUSec
)"
[[ "$runtime_signature" == t && "$original_runtime_watchdog_usec" =~ ^[1-9][0-9]*$ ]] || \
  die "could not read the active runtime-watchdog setting"
[[ "$reboot_signature" == t && "$original_reboot_watchdog_usec" =~ ^[1-9][0-9]*$ ]] || \
  die "could not read the active reboot-watchdog setting"

restore_manager_watchdogs() {
  sudo -n busctl set-property \
    "$watchdog_bus" "$watchdog_object" "$watchdog_interface" \
    RuntimeWatchdogUSec t "$original_runtime_watchdog_usec" >/dev/null 2>&1 || true
  sudo -n busctl set-property \
    "$watchdog_bus" "$watchdog_object" "$watchdog_interface" \
    RebootWatchdogUSec t "$original_reboot_watchdog_usec" >/dev/null 2>&1 || true
}

watchdog_restore_needed=1
restore_watchdogs_on_exit() {
  if (( watchdog_restore_needed )); then
    restore_manager_watchdogs
  fi
}
abort_watchdog_preparation() {
  trap - EXIT INT TERM HUP
  restore_manager_watchdogs
  die "watchdog handoff interrupted before the hardware countdown began"
}
trap restore_watchdogs_on_exit EXIT
trap abort_watchdog_preparation INT TERM HUP

log "programming the one-shot hardware deadline before PID 1 releases the watchdog"
if ! sudo -n busctl set-property \
  "$watchdog_bus" "$watchdog_object" "$watchdog_interface" \
  RuntimeWatchdogUSec t "$hard_reboot_deadline_usec"; then
  die "could not program the hard reboot deadline"
fi

watchdog_timeout="$(cat "$watchdog_sysfs/timeout" 2>/dev/null || true)"
if [[ "$watchdog_timeout" != "$hard_reboot_deadline_sec" ]]; then
  die "hardware watchdog rejected the ${hard_reboot_deadline_sec}s timeout: ${watchdog_timeout:-missing}"
fi

# Reversible pre-flight. Everything below this point trades systemd's
# reboot-phase watchdog for a bet that the hardware counter fires on its own,
# and that bet is unrecoverable if the counter is halted: the box wedges in
# systemd-shutdown with no feeder, no RebootWatchdogSec, and no reboot.target
# job left to time out, so the only way back is the power button.
#
# PID 1 still feeds the device in this window (every
# hard_reboot_deadline_sec/2), so timeleft must visibly fall before the next
# keepalive. A timeleft pinned at the reload value means the TCO timer is not
# advancing -- refuse the handoff instead of arming a deadline that will
# never expire.
log "probing the hardware countdown before the irreversible handoff"
countdown_probe_start="$(cat "$watchdog_sysfs/timeleft" 2>/dev/null || true)"
countdown_advanced=0
if [[ "$countdown_probe_start" =~ ^[0-9]+$ ]]; then
  for _ in $(seq 1 "$countdown_probe_seconds"); do
    sleep 1
    countdown_probe_now="$(cat "$watchdog_sysfs/timeleft" 2>/dev/null || true)"
    [[ "$countdown_probe_now" =~ ^[0-9]+$ ]] || break
    if (( countdown_probe_now < countdown_probe_start )); then
      countdown_advanced=1
      break
    fi
  done
fi
hitman_verified=0
if (( countdown_advanced )); then
  log "hardware countdown advances (${countdown_probe_start}s -> ${countdown_probe_now}s); the deadline can fire"
else
  # Measured dead on this host: iTCO reports state=active while TCO_RLD never
  # decrements, and a ceded 90s deadline still left the box wedged for 206s.
  # Never trade systemd's reboot-phase watchdog for a counter that cannot fire
  # -- doing that is what turned an ordinary shutdown hang into a power-button
  # recovery. Put back what we touched and reboot with the cover systemd has.
  log "WARNING: ${watchdog_module:-the hardware watchdog} timeleft stuck at ${countdown_probe_start:-missing}s over ${countdown_probe_seconds}s while PID 1 feeds it every $((hard_reboot_deadline_sec / 2))s"
  log "WARNING: the hardware countdown cannot fire; skipping the handoff and keeping RebootWatchdogSec armed"
  log "WARNING: this reboot has no hardware backstop -- if it wedges, use Alt+SysRq+S, U, B rather than the power button"
  restore_manager_watchdogs
fi

if (( countdown_advanced )); then
  if ! sudo -n busctl set-property \
    "$watchdog_bus" "$watchdog_object" "$watchdog_interface" \
    RebootWatchdogUSec t 0; then
    die "could not disable second-phase watchdog feeding"
  fi

  log "ceding the non-disarmable watchdog to start the ${hard_reboot_deadline_sec}s hardware countdown"
  trap '' INT TERM HUP
  if ! sudo -n busctl set-property \
    "$watchdog_bus" "$watchdog_object" "$watchdog_interface" \
    RuntimeWatchdogUSec t 0; then
    restore_manager_watchdogs
    watchdog_restore_needed=0
    trap - EXIT INT TERM HUP
    die "PID 1 did not release the hardware watchdog"
  fi
  watchdog_restore_needed=0
  trap - EXIT INT TERM HUP

  hitman_verified=1
  runtime_watchdog_property="$(
    busctl get-property "$watchdog_bus" "$watchdog_object" "$watchdog_interface" RuntimeWatchdogUSec 2>/dev/null || true
  )"
  reboot_watchdog_property="$(
    busctl get-property "$watchdog_bus" "$watchdog_object" "$watchdog_interface" RebootWatchdogUSec 2>/dev/null || true
  )"
  watchdog_nowayout="$(cat "$watchdog_sysfs/nowayout" 2>/dev/null || true)"
  watchdog_state="$(cat "$watchdog_sysfs/state" 2>/dev/null || true)"
  watchdog_nodes=("$watchdog_device")
  if [[ -c /dev/watchdog ]]; then
    watchdog_nodes+=(/dev/watchdog)
  fi
  if ! sudo -n true; then
    watchdog_fuser_status=125
  elif sudo -n fuser -s "${watchdog_nodes[@]}"; then
    watchdog_fuser_status=0
  else
    watchdog_fuser_status=$?
  fi
  watchdog_timeleft_before="$(cat "$watchdog_sysfs/timeleft" 2>/dev/null || true)"
  sleep 2
  watchdog_timeleft_after="$(cat "$watchdog_sysfs/timeleft" 2>/dev/null || true)"

  [[ "$runtime_watchdog_property" == "t 0" ]] || hitman_verified=0
  [[ "$reboot_watchdog_property" == "t 0" ]] || hitman_verified=0
  [[ "$watchdog_nowayout" == 1 && "$watchdog_state" == active ]] || hitman_verified=0
  [[ "$watchdog_fuser_status" == 1 ]] || hitman_verified=0
  [[ "$watchdog_timeleft_before" =~ ^[0-9]+$ && "$watchdog_timeleft_after" =~ ^[0-9]+$ ]] || hitman_verified=0
  if (( hitman_verified )) && (( watchdog_timeleft_after >= watchdog_timeleft_before )); then
    hitman_verified=0
  fi

  if (( hitman_verified )); then
    log "verified watchdog handoff: no feeder and ${watchdog_timeleft_after}s remain on the hardware countdown"
  else
    log "ERROR: watchdog handoff verification was incomplete; the reboot transaction timeout remains armed"
  fi
fi

if ! sudo -n systemctl reboot --no-block; then
  log "ERROR: the orderly reboot request failed; wait for the non-disarmable hardware deadline"
  exit 1
fi
if (( hitman_verified )); then
  log "the orderly reboot request was accepted with the verified hardware countdown active"
else
  log "the orderly reboot request was accepted without a fully verified watchdog handoff"
fi
