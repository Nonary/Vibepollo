#!/usr/bin/env bash

set -Eeuo pipefail
IFS=$'\n\t'

readonly expected_user="chasep"
readonly expected_home="/home/chasep"
readonly sunshine_service="vibeshine.service"
readonly hot_reload_unit="vibeshine-drm-hot-reload.service"
readonly legacy_signing_dropin="/etc/systemd/system/vibeshine-drm-setup.service.d/secure-boot-signing.conf"

assume_yes=0
dry_run=0
stage_dir=""

usage() {
  cat <<EOF
Deploy the current Vibepollo build and hot-reload its vibeshine_drm driver without rebooting.

Usage: $(basename "$0") [--yes] [--dry-run]

  --yes      Skip the final interactive confirmation.
  --dry-run  Stage and validate the install without changing the host.
  -h, --help Show this help.

Run this script as ${expected_user}, without sudo. It asks for sudo once.
EOF
}

log() {
  printf '[hot-reload] %s\n' "$*"
}

die() {
  printf '[hot-reload] ERROR: %s\n' "$*" >&2
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
    --yes) assume_yes=1 ;;
    --dry-run) dry_run=1 ;;
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

for command_name in bash cmake cp find getcap install mktemp modinfo mv readlink sed setcap sudo systemctl tail udevadm; do
  command -v "$command_name" >/dev/null 2>&1 || die "required command not found: $command_name"
done

[[ -f "$build_dir/cmake_install.cmake" ]] || die "missing configured build directory: $build_dir"
[[ -x "$build_dir/sunshine-0.0.0" ]] || die "missing built Vibepollo executable; run cmake --build build -j2 first"

if (( !dry_run && !assume_yes )); then
  cat <<EOF

This will install the current build and reload the virtual DRM driver without
rebooting. It will:

  - disconnect active streams and stop games launched by Vibepollo;
  - briefly stop KWin/Xwayland, blanking the desktop and closing Wayland apps;
  - reload only vibeshine_drm, then recreate the virtual displays;
  - restart KWin and Vibepollo from a detached recovery-capable system unit.

The desktop and streaming service should return automatically within a minute.
EOF
  read -r -p "Type HOT-RELOAD to continue: " confirmation
  [[ $confirmation == "HOT-RELOAD" ]] || die "hot reload cancelled"
fi

stage_dir="$(mktemp -d "${TMPDIR:-/tmp}/vibeshine-hot-reload.XXXXXXXX")"
[[ -d "$stage_dir" ]] || die "failed to create deployment staging directory"

log "staging the current build"
install_log="$stage_dir/cmake-install.log"
if ! DESTDIR="$stage_dir" cmake --install "$build_dir" >"$install_log" 2>&1; then
  tail -n 200 "$install_log" >&2
  die "CMake staging install failed"
fi

stage_user_prefix="$stage_dir$expected_home/.local"
[[ -d "$stage_user_prefix" ]] || die "staged user prefix is missing"
[[ -x "$stage_user_prefix/bin/sunshine-0.0.0" ]] || die "staged Vibepollo executable is missing"

required_system_files=(
  "$stage_dir/usr/lib/udev/rules.d/60-sunshine.rules"
  "$stage_dir/usr/lib/systemd/user/app-dev.lizardbyte.app.Sunshine.service"
  "$stage_dir/usr/lib/modules-load.d/60-sunshine.conf"
  "$stage_dir/usr/libexec/vibeshine/vibeshine-drm-install"
  "$stage_dir/usr/libexec/vibeshine/vibeshine-drm-hot-reload"
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
  die "expected exactly one regular staged vibeshine_drm source directory"
fi
drm_source_name="${drm_source_dirs[0]##*/}"
[[ "$drm_source_name" == vibeshine-drm-* ]] || die "unexpected DRM source directory name"

bash -n "$stage_dir/usr/libexec/vibeshine/vibeshine-drm-hot-reload"
log "validated staged user files, system files, driver source, and detached worker"

if ((dry_run)); then
  log "dry run passed; no files, services, or drivers were changed"
  exit 0
fi

log "requesting administrator authentication"
sudo -v

log "installing user-local Vibepollo files"
cp -a --remove-destination "$stage_user_prefix/." "$expected_home/.local/"

log "installing root-owned driver and system integration"
sudo install -Dm644 "$stage_dir/usr/lib/udev/rules.d/60-sunshine.rules" /usr/lib/udev/rules.d/60-sunshine.rules
sudo install -Dm644 "$stage_dir/usr/lib/systemd/user/app-dev.lizardbyte.app.Sunshine.service" /usr/lib/systemd/user/app-dev.lizardbyte.app.Sunshine.service
sudo install -Dm644 "$stage_dir/usr/lib/modules-load.d/60-sunshine.conf" /usr/lib/modules-load.d/60-sunshine.conf
sudo install -d -m 0755 "/usr/src/$drm_source_name"
sudo cp -a "${drm_source_dirs[0]}/." "/usr/src/$drm_source_name/"
sudo install -Dm755 "$stage_dir/usr/libexec/vibeshine/vibeshine-drm-install" /usr/libexec/vibeshine/vibeshine-drm-install
sudo install -Dm755 "$stage_dir/usr/libexec/vibeshine/vibeshine-drm-hot-reload" /usr/libexec/vibeshine/vibeshine-drm-hot-reload
sudo install -Dm755 "$stage_dir/usr/libexec/vibeshine/vibeshine-vkms" /usr/libexec/vibeshine/vibeshine-vkms
sudo install -Dm755 "$stage_dir/usr/lib/vibeshine/libvibeshine-kwin-gpu.so" /usr/lib/vibeshine/libvibeshine-kwin-gpu.so
sudo install -Dm755 "$stage_dir/usr/libexec/vibeshine/kwin-preload/kwin_wayland" /usr/libexec/vibeshine/kwin-preload/kwin_wayland
sudo install -Dm644 "$stage_dir/usr/lib/systemd/user/plasma-kwin_wayland.service.d/vibeshine-kwin-gpu.conf" /usr/lib/systemd/user/plasma-kwin_wayland.service.d/vibeshine-kwin-gpu.conf
for unit in vibeshine-drm-setup.service vibeshine-vkms.service \
  vibeshine-vkms-control.socket 'vibeshine-vkms-control@.service'; do
  sudo install -Dm644 "$stage_dir/usr/lib/systemd/system/$unit" "/usr/lib/systemd/system/$unit"
done

if sudo test -f "$legacy_signing_dropin"; then
  legacy_dropin_body="$(sudo sed '/^[[:space:]]*$/d' "$legacy_signing_dropin")"
  expected_dropin_body=$'[Service]\nExecStartPost=/usr/local/sbin/sign-vibeshine-drm-module'
  if [[ "$legacy_dropin_body" == "$expected_dropin_body" ]]; then
    log "disabling the obsolete post-signing override; the installer now signs and verifies modules itself"
    sudo mv --no-clobber "$legacy_signing_dropin" "${legacy_signing_dropin}.disabled"
  else
    die "refusing to alter an unrecognized DRM setup-service override: $legacy_signing_dropin"
  fi
fi

sudo systemctl daemon-reload
sudo udevadm control --reload-rules

sunshine_binary="$(readlink -f -- "$expected_home/.local/bin/sunshine")"
case "$sunshine_binary" in
  "$expected_home"/.local/bin/sunshine-*) ;;
  *) die "refusing to set capabilities on unexpected path: $sunshine_binary" ;;
esac
sudo setcap cap_sys_admin,cap_sys_nice+p "$sunshine_binary"
sunshine_caps="$(getcap "$sunshine_binary")"
[[ "$sunshine_caps" == *cap_sys_admin* && "$sunshine_caps" == *cap_sys_nice* ]] || \
  die "Vibepollo capabilities did not verify"

log "building and installing the staged kernel module"
set +e
sudo /usr/libexec/vibeshine/vibeshine-drm-install install
drm_install_rc=$?
set -e
if ((drm_install_rc != 0 && drm_install_rc != 4)); then
  die "vibeshine_drm installation failed with status $drm_install_rc"
fi

installed_version="$(modinfo -F version vibeshine_drm 2>/dev/null || true)"
installed_srcversion="$(modinfo -F srcversion vibeshine_drm 2>/dev/null || true)"
[[ -n "$installed_version" && -n "$installed_srcversion" ]] || die "installed DRM module did not verify"
log "installed DRM module version $installed_version"

if systemctl is-active --quiet "$hot_reload_unit"; then
  die "$hot_reload_unit is already active"
fi

cleanup_stage
trap - EXIT

log "handing the disruptive reload to a detached system service"
sudo systemd-run \
  --unit="$hot_reload_unit" \
  --collect \
  --property=Type=exec \
  /usr/libexec/vibeshine/vibeshine-drm-hot-reload "$(id -u)" "$expected_user"

cat <<EOF

[hot-reload] Handoff complete. The stream and desktop will disconnect shortly.
[hot-reload] They should return automatically within about one minute.
[hot-reload] Progress: sudo journalctl -fu $hot_reload_unit
EOF
