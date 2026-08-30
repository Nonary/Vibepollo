#!/usr/bin/env bash

set -Eeuo pipefail
IFS=$'\n\t'

readonly expected_user="chasep"
readonly expected_home="/home/chasep"
readonly system_assets_dir="/usr/share/vibepollo"
readonly prelogin_service="vibepollo-prelogin.service"
readonly prelogin_binary="/usr/local/libexec/vibepollo-prelogin/vibepollo"
readonly prelogin_config="/var/lib/vibepollo-prelogin-static/vibepollo.conf"

assume_yes=0
dry_run=0
stage_dir=""

usage() {
  cat <<EOF
Repair the installed Vibepollo pre-login host without rebooting.

Usage: $(basename "$0") [--yes] [--dry-run]

  --yes      Skip the final interactive confirmation.
  --dry-run  Stage and validate the repair without changing the host.
  -h, --help Show this help.

Run this script as ${expected_user}, without sudo. It asks for sudo once.
EOF
}

log() {
  printf '[prelogin-repair] %s\n' "$*"
}

die() {
  printf '[prelogin-repair] ERROR: %s\n' "$*" >&2
  exit 1
}

cleanup_stage() {
  if [[ -n "$stage_dir" && -d "$stage_dir" ]]; then
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
  basename
  chown
  cmake
  cp
  find
  getcap
  id
  install
  journalctl
  mktemp
  readlink
  rg
  sed
  setcap
  sha256sum
  sleep
  sudo
  systemctl
  tail
  test
  wc
)
for command_name in "${required_commands[@]}"; do
  command -v "$command_name" >/dev/null 2>&1 || die "required command not found: $command_name"
done

[[ -f "$build_dir/cmake_install.cmake" ]] || die "missing configured build directory: $build_dir"
[[ -x "$build_dir/vibepollo" ]] || die "missing built Vibepollo executable; run cmake --build build -j10 first"

stage_dir="$(mktemp -d "${TMPDIR:-/tmp}/vibepollo-prelogin-repair.XXXXXXXX")"
[[ -d "$stage_dir" ]] || die "failed to create deployment staging directory"

log "staging the current build"
install_log="$stage_dir/cmake-install.log"
if ! DESTDIR="$stage_dir" cmake --install "$build_dir" >"$install_log" 2>&1; then
  tail -n 200 "$install_log" >&2
  die "CMake staging install failed"
fi

stage_user_prefix="$stage_dir$expected_home/.local"
stage_binary="$stage_user_prefix/bin/vibepollo"
stage_system_assets="$stage_dir$system_assets_dir"
[[ -x "$stage_binary" ]] || die "staged Vibepollo executable is missing"
stage_binary_resolved="$(readlink -f -- "$stage_binary")"
case "$stage_binary_resolved" in
  "$stage_user_prefix"/bin/vibepollo-*) ;;
  *) die "staged Vibepollo link resolves outside its versioned binary directory: ${stage_binary_resolved:-missing}" ;;
esac
[[ -f "$stage_binary_resolved" && ! -L "$stage_binary_resolved" ]] || \
  die "staged Vibepollo target is not a regular file: $stage_binary_resolved"
[[ -d "$stage_system_assets" && ! -L "$stage_system_assets" ]] || \
  die "staged system asset tree is missing: $stage_system_assets"
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

if ! rg -a -Fq "$system_assets_dir/shaders/opengl/Scene.vert" "$stage_binary_resolved"; then
  die "the build does not use the system asset tree; reconfigure with -DSUNSHINE_ASSETS_DIR=$system_assets_dir and rebuild"
fi
if rg -a -Fq "$expected_home/.local/assets/shaders/opengl/Scene.vert" "$stage_binary_resolved"; then
  die "the build still embeds the private shader path"
fi

if (( dry_run )); then
  log "dry run passed: the staged binary and root-readable asset tree match"
  exit 0
fi

if (( !assume_yes )); then
  cat <<EOF

This will replace the pre-login Vibepollo executable and its root-owned runtime
assets, then restart ${prelogin_service}. Any current pre-login stream will disconnect.

EOF
  read -r -p "Type REPAIR to continue: " confirmation
  [[ "$confirmation" == REPAIR ]] || die "repair cancelled"
fi

log "requesting administrator authentication"
sudo -v

systemctl cat "$prelogin_service" >/dev/null 2>&1 || \
  die "required service is missing: $prelogin_service"
prelogin_exec_start="$(systemctl show "$prelogin_service" -p ExecStart --value)"
[[ "$prelogin_exec_start" == *"path=${prelogin_binary}"* ]] || \
  die "${prelogin_service} uses an unexpected executable: ${prelogin_exec_start:-missing}"
sudo systemctl enable "$prelogin_service"
prelogin_unit_state="$(systemctl show "$prelogin_service" -p UnitFileState --value)"
[[ "$prelogin_unit_state" == enabled ]] || \
  die "required pre-login service is not enabled: ${prelogin_unit_state:-unknown}"
sudo test -f "$prelogin_config" || die "required pre-login configuration is missing: $prelogin_config"
sudo test ! -L "$prelogin_config" || die "refusing symlinked pre-login configuration: $prelogin_config"

log "installing the matching root-owned runtime assets"
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
  staged_hash="$(sha256sum "$staged_asset" | sed 's/[[:space:]].*$//')"
  installed_hash="$(sha256sum "$installed_asset" | sed 's/[[:space:]].*$//')"
  [[ "$installed_hash" == "$staged_hash" ]] || \
    die "installed runtime asset does not match the build: $installed_asset"
done
sudo -u plasmalogin test -r "$system_assets_dir/shaders/opengl/Scene.vert" || \
  die "the pre-login account cannot read the installed shaders"

log "installing the corrected pre-login executable"
sudo install -Dm755 "$stage_binary_resolved" "$prelogin_binary"
sudo setcap cap_sys_admin,cap_sys_nice=ep "$prelogin_binary"
prelogin_caps="$(getcap "$prelogin_binary")"
[[ "$prelogin_caps" == *cap_sys_admin* && "$prelogin_caps" == *cap_sys_nice* ]] || \
  die "pre-login capabilities did not verify: ${prelogin_caps:-none}"
stage_binary_hash="$(sha256sum "$stage_binary_resolved" | sed 's/[[:space:]].*$//')"
installed_binary_hash="$(sha256sum "$prelogin_binary" | sed 's/[[:space:]].*$//')"
[[ "$installed_binary_hash" == "$stage_binary_hash" ]] || \
  die "installed pre-login executable does not match the build"

encoder_count="$(sudo sed -n '/^[[:space:]]*encoder[[:space:]]*=/p' "$prelogin_config" | wc -l)"
[[ "$encoder_count" == 1 ]] || die "expected exactly one encoder setting in $prelogin_config"
sudo sed -i -E \
  's/^[[:space:]]*encoder[[:space:]]*=.*/encoder = nvenc/' \
  "$prelogin_config"
capture_count="$(sudo sed -n '/^[[:space:]]*capture[[:space:]]*=/p' "$prelogin_config" | wc -l)"
[[ "$capture_count" == 1 ]] || die "expected exactly one capture setting in $prelogin_config"
sudo sed -i -E \
  's/^[[:space:]]*capture[[:space:]]*=.*/capture = kms/' \
  "$prelogin_config"
virtual_mode_count="$(sudo sed -n '/^[[:space:]]*virtual_display_mode[[:space:]]*=/p' "$prelogin_config" | wc -l)"
[[ "$virtual_mode_count" == 1 ]] || \
  die "expected exactly one virtual_display_mode setting in $prelogin_config"
sudo sed -i -E \
  's/^[[:space:]]*virtual_display_mode[[:space:]]*=.*/virtual_display_mode = shared/' \
  "$prelogin_config"

log "restarting ${prelogin_service}"
sudo systemctl restart "$prelogin_service"
prelogin_invocation_id="$(systemctl show "$prelogin_service" -p InvocationID --value)"
[[ "$prelogin_invocation_id" =~ ^[0-9a-fA-F]{32}$ ]] || \
  die "could not resolve the restarted service invocation"
systemctl is-active --quiet "$prelogin_service" || die "the restarted pre-login service is not active"

encoder_ready=0
for _ in {1..20}; do
  service_log="$(journalctl -u "$prelogin_service" _SYSTEMD_INVOCATION_ID="$prelogin_invocation_id" --no-pager 2>/dev/null || true)"
  if [[ "$service_log" =~ Linux\ private\ display:\ ready\ with\ [1-9][0-9]*\ private\ output\(s\)\. ]] &&
     [[ "$service_log" == *"config: 'capture' = kms"* &&
        "$service_log" == *"config: 'virtual_display_mode' = shared"* &&
        "$service_log" == *"Found H.264 encoder: h264_nvenc [nvenc]"* &&
        "$service_log" == *"Found HEVC encoder: hevc_nvenc [nvenc]"* &&
        "$service_log" == *"Found AV1 encoder: av1_nvenc [nvenc]"* ]]; then
    encoder_ready=1
    break
  fi
  sleep 1
done

if (( !encoder_ready )); then
  systemctl --no-pager --full status "$prelogin_service" >&2 || true
  journalctl -u "$prelogin_service" _SYSTEMD_INVOCATION_ID="$prelogin_invocation_id" --no-pager -n 200 >&2 || true
  die "pre-login encoder readiness did not verify"
fi

sleep 2
systemctl is-active --quiet "$prelogin_service" || \
  die "the pre-login service did not remain active after encoder validation"
stable_invocation_id="$(systemctl show "$prelogin_service" -p InvocationID --value)"
[[ "$stable_invocation_id" == "$prelogin_invocation_id" ]] || \
  die "the pre-login service restarted after encoder validation"
stable_main_pid="$(systemctl show "$prelogin_service" -p MainPID --value)"
[[ "$stable_main_pid" =~ ^[1-9][0-9]*$ ]] || \
  die "the pre-login service has no live main process after encoder validation"

log "verified the private-display pool and H.264, HEVC, and AV1 native NVENC startup readiness"
log "an actual client stream is still required to verify KMS capture and first-frame delivery"
