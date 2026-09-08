#!/usr/bin/env bash
# Vibepollo installer for Arch Linux and CachyOS.
#
# Vibepollo's Linux beta is developed and tested on CachyOS with KDE Plasma 6
# on Wayland. Other Arch-based distributions are supported on a best-effort
# basis. This script:
#
#   1. checks the machine against the documented requirements,
#   2. installs the kernel headers the virtual-display driver needs,
#   3. installs the Vibepollo package from the signed Nonary repository, or
#      from the newest GitHub release when the repository is unavailable,
#   4. opens the firewall (firewalld or ufw) when one is active, and
#   5. prints whether a reboot is required and what to do next.
#
# Usage:
#   sudo bash linux_install.sh [options]
#
# Options:
#   --version VERSION     Install this exact release (for example 1.19.0-beta.5).
#   --package FILE        Install a local vibepollo-*.pkg.tar.zst instead of downloading.
#   --stable              Ignore pre-releases when picking the newest GitHub release.
#   --no-repo             Skip the signed pacman repository and use GitHub releases.
#   --skip-checks         Continue past failed requirement checks (not recommended).
#   --yes                 Answer yes to pacman prompts.
#   --source-profile HOST Choose vibepollo, vibeshine, sunshine, or machine-vibeshine
#                         when more than one legacy profile exists.
#   -h, --help            Show this help.
#
# Re-running the script is safe; it only installs what is missing.

set -euo pipefail

readonly REPO_OWNER='Nonary'
readonly REPO_NAME='Vibepollo'
readonly REPO_URL="https://github.com/${REPO_OWNER}/${REPO_NAME}"
readonly API_URL="https://api.github.com/repos/${REPO_OWNER}/${REPO_NAME}"
readonly PACMAN_REPO_NAME='vibepollo'
readonly PACMAN_REPO_SERVER='https://nonary.github.io/Vibepollo/arch/x86_64'
readonly PACMAN_REPO_CONF='/etc/pacman.d/vibepollo.conf'
readonly MIN_KERNEL_MAJOR=6
readonly MIN_KERNEL_MINOR=16
readonly DRM_INSTALL='/usr/libexec/vibeshine/vibeshine-drm-install'
readonly MACHINE_HOST='/usr/libexec/vibeshine/vibepollo-machine-host'

requested_version=''
local_package=''
allow_prerelease=1
use_repo=1
skip_checks=0
pacman_confirm=()
replacement_confirm=()
driver_overwrite=()
check_failures=0
warnings=()
workdir=''

usage() {
  sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
ok() { printf '    \033[1;32mOK\033[0m   %s\n' "$*"; }
warn() { printf '    \033[1;33mWARN\033[0m %s\n' "$*" >&2; warnings+=("$*"); }
fail() { printf '    \033[1;31mFAIL\033[0m %s\n' "$*" >&2; check_failures=$((check_failures + 1)); }
die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

cleanup() {
  if [[ -n "$workdir" && -d "$workdir" ]]; then
    rm -rf -- "$workdir"
  fi
}
trap cleanup EXIT

parse_args() {
  while (($# > 0)); do
    case "$1" in
      --version)
        [[ $# -ge 2 ]] || die '--version requires a value'
        requested_version="${2#v}"
        shift 2
        ;;
      --package)
        [[ $# -ge 2 ]] || die '--package requires a path'
        local_package="$2"
        shift 2
        ;;
      --stable) allow_prerelease=0; shift ;;
      --no-repo) use_repo=0; shift ;;
      --skip-checks) skip_checks=1; shift ;;
      --yes)
        pacman_confirm=(--noconfirm)
        # libalpm ALPM_QUESTION_CONFLICT_PKG is 1 << 2. With --noconfirm,
        # pacman normally rejects removal of conflicting installed hosts.
        # Invert that answer only for the explicit Vibepollo transaction.
        replacement_confirm=(--noconfirm --ask=4)
        shift ;;
      --source-profile)
        [[ $# -ge 2 ]] || die '--source-profile requires a host'
        case "$2" in vibepollo|vibeshine|sunshine|machine-vibeshine) ;; *) die 'invalid --source-profile host' ;; esac
        export VIBEPOLLO_IMPORT_SOURCE=$2
        shift 2 ;;
      -h | --help) usage; exit 0 ;;
      *) die "unknown option: $1 (see --help)" ;;
    esac
  done
}

require_root() {
  if [[ $EUID -ne 0 ]]; then
    die 'run this script with sudo: sudo bash linux_install.sh'
  fi
}

require_pacman() {
  command -v pacman >/dev/null 2>&1 ||
    die 'pacman was not found. The Linux beta ships as a native package for Arch Linux and CachyOS only.'
}

check_distribution() {
  local id='' name='' id_like=''
  if [[ -r /etc/os-release ]]; then
    # shellcheck disable=SC1091
    id=$(. /etc/os-release && printf '%s' "${ID:-}")
    name=$(. /etc/os-release && printf '%s' "${NAME:-}")
    id_like=$(. /etc/os-release && printf '%s' "${ID_LIKE:-}")
  fi
  case "$id" in
    cachyos)
      ok "CachyOS detected. This is the distribution Vibepollo is tuned and tested on."
      ;;
    arch)
      ok "Arch Linux detected. Vibepollo is optimized for CachyOS; Arch is supported on a best-effort basis."
      ;;
    *)
      if [[ " $id_like " == *' arch '* ]]; then
        warn "${name:-$id} is an Arch derivative. Vibepollo is tested on CachyOS; expect rough edges here."
      else
        warn "${name:-unknown distribution} is not Arch-based. Continuing because pacman exists, but this is untested."
      fi
      ;;
  esac
  if [[ "$(uname -m)" != x86_64 ]]; then
    fail "Only x86_64 packages are published; this machine is $(uname -m)."
  fi
}

kernel_release=''
kernel_package=''
headers_package=''

kernel_package_for() {
  local release=$1 modules_root=${2:-/usr/lib/modules} path owner
  # Some kernel packages move the image to /boot. Ask pacman about other
  # kernel-owned metadata instead of guessing a package from uname's suffix.
  for path in "$modules_root/$release/vmlinuz" "$modules_root/$release/pkgbase"; do
    if owner=$(pacman -Qqo -- "$path" 2>/dev/null) &&
       [[ "$owner" =~ ^[A-Za-z0-9@._+:-]+$ ]]; then
      printf '%s\n' "$owner"
      return 0
    fi
  done
  return 1
}

check_retired_kernel() {
  local modules_root=${1:-/usr/lib/modules} directory release owner installed=''
  [[ ! -d "$modules_root/$kernel_release" ]] || return 0
  for directory in "$modules_root"/*; do
    [[ -d "$directory" ]] || continue
    release=${directory##*/}
    [[ "$release" != "$kernel_release" ]] || continue
    owner=$(kernel_package_for "$release" "$modules_root") || continue
    installed+=" ${release} (${owner})"
  done
  if [[ -n "$installed" ]]; then
    die "Reboot required: running kernel ${kernel_release} no longer has its module tree installed. Installed kernels:${installed}. Reboot into an installed kernel, then retry this installer. Do not install newer headers for the old running kernel; no system upgrade or reboot was performed."
  fi
}

check_kernel() {
  local major minor modules_root=${1:-/usr/lib/modules}
  kernel_release=$(uname -r)
  kernel_package=''; headers_package=''
  if [[ "$kernel_release" =~ ^([0-9]+)\.([0-9]+) ]]; then
    major=${BASH_REMATCH[1]}
    minor=${BASH_REMATCH[2]}
  else
    fail "could not parse the running kernel version '${kernel_release}'"
    return
  fi
  if ((major > MIN_KERNEL_MAJOR || (major == MIN_KERNEL_MAJOR && minor >= MIN_KERNEL_MINOR))); then
    ok "Linux ${kernel_release} meets the ${MIN_KERNEL_MAJOR}.${MIN_KERNEL_MINOR} minimum for managed virtual displays."
  else
    fail "Linux ${kernel_release} is older than ${MIN_KERNEL_MAJOR}.${MIN_KERNEL_MINOR}; the virtual-display driver will not build."
  fi

  check_retired_kernel "$modules_root"
  if kernel_package=$(kernel_package_for "$kernel_release" "$modules_root"); then
    headers_package="${kernel_package}-headers"
    ok "Running kernel package: ${kernel_package} (headers: ${headers_package})"
  else
    warn "could not map the running kernel to a pacman package; you may need to install its headers manually."
  fi
}

install_kernel_headers() {
  local build_dir="/usr/lib/modules/${kernel_release}/build"
  if [[ -f "${build_dir}/Makefile" ]]; then
    ok "Kernel headers for ${kernel_release} are already installed."
    return
  fi
  check_retired_kernel
  if [[ -z "$headers_package" ]]; then
    die "Kernel headers for ${kernel_release} are missing and the package name could not be determined. Install matching headers before retrying."
  fi
  log "Installing ${headers_package} so DKMS can build the virtual-display driver"
  pacman -S --needed "${pacman_confirm[@]}" "$headers_package"
  if [[ -f "${build_dir}/Makefile" ]]; then
    ok "Kernel headers installed."
  else
    die "${headers_package} does not provide headers for the running kernel ${kernel_release}. Install matching headers, or reboot into the kernel matching the installed headers, then retry. No system upgrade was requested."
  fi
}

check_desktop() {
  if pacman -Qq plasma-workspace >/dev/null 2>&1 || pacman -Qq plasma-desktop >/dev/null 2>&1; then
    ok "KDE Plasma is installed."
  else
    fail "KDE Plasma is not installed. Vibepollo streams only a KDE Plasma 6 Wayland desktop (install plasma-meta or plasma-desktop)."
  fi
  if pacman -Qq sddm >/dev/null 2>&1 || pacman -Qq plasma-login-manager >/dev/null 2>&1; then
    ok "A supported login manager (SDDM or Plasma Login Manager) is installed."
  else
    warn "Neither sddm nor plasma-login-manager is installed. Vibepollo only attaches to Plasma sessions started by one of them."
  fi
  if [[ -x /usr/bin/kwin_wayland ]]; then
    ok "KWin Wayland is available."
  else
    fail "/usr/bin/kwin_wayland is missing; the Plasma Wayland session is required."
  fi
}

check_gpu() {
  local nvidia=0 amd=0 intel=0 line
  if command -v lspci >/dev/null 2>&1; then
    while IFS= read -r line; do
      case "$line" in
        *NVIDIA*) nvidia=1 ;;
        *AMD* | *ATI* | *Advanced\ Micro*) amd=1 ;;
        *Intel*) intel=1 ;;
      esac
    done < <(lspci -nn 2>/dev/null | grep -Ei 'vga|3d|display' || true)
  fi
  [[ -d /sys/module/nvidia ]] && nvidia=1

  if ((nvidia)); then
    if [[ -d /sys/module/nvidia ]]; then
      ok "NVIDIA GPU with the proprietary driver loaded (NVENC and pre-login streaming supported)."
    else
      warn "NVIDIA GPU detected but the nvidia kernel module is not loaded. Install nvidia-utils and the matching driver package before streaming."
    fi
    if [[ -r /sys/module/nvidia_drm/parameters/modeset ]] &&
       [[ "$(cat /sys/module/nvidia_drm/parameters/modeset)" != Y ]]; then
      warn "nvidia_drm.modeset is disabled. Plasma Wayland and KMS capture need nvidia_drm.modeset=1 on the kernel command line."
    fi
  fi
  if ((amd)); then
    if pacman -Qq libva-mesa-driver >/dev/null 2>&1; then
      ok "AMD GPU with libva-mesa-driver (VAAPI encoding)."
    else
      warn "AMD GPU detected without libva-mesa-driver; install it for hardware H.264/HEVC encoding."
    fi
  fi
  if ((intel)) && ! ((nvidia)) && ! ((amd)); then
    if pacman -Qq intel-media-driver >/dev/null 2>&1; then
      ok "Intel GPU with intel-media-driver (VAAPI encoding)."
    else
      warn "Intel GPU detected without intel-media-driver; install it for hardware encoding."
    fi
  fi
  if ! ((nvidia || amd || intel)); then
    warn "Could not identify a GPU vendor. Vibepollo needs a GPU with a hardware H.264 encoder."
  fi
  if ((nvidia == 0)); then
    warn "Pre-login (greeter) streaming is only supported on NVIDIA GPUs; this machine streams after login."
  fi
}

check_secure_boot() {
  if command -v mokutil >/dev/null 2>&1; then
    if LC_ALL=C mokutil --sb-state 2>/dev/null | grep -q 'SecureBoot enabled'; then
      warn "Secure Boot is enabled. The package signs its kernel module; you may be asked to approve a one-time MOK enrollment on the next reboot. Do not disable Secure Boot."
    else
      ok "Secure Boot is not enforcing."
    fi
  fi
}

run_checks() {
  log 'Checking this machine against the Vibepollo Linux requirements'
  check_distribution
  check_kernel
  check_desktop
  check_gpu
  check_secure_boot
  if ((check_failures > 0)); then
    if ((skip_checks)); then
      warn "${check_failures} requirement check(s) failed; continuing because --skip-checks was given."
    else
      die "${check_failures} requirement check(s) failed. Fix them or re-run with --skip-checks."
    fi
  fi
}

repo_is_available() {
  curl -fsSIL --max-time 15 "${PACMAN_REPO_SERVER}/${PACMAN_REPO_NAME}.db" >/dev/null 2>&1
}

configure_pacman_repo() {
  local keyfile fingerprint
  log 'Importing the Nonary repository signing key'
  keyfile="${workdir}/nonary-vibepollo.gpg"
  curl -fsSL --max-time 60 -o "$keyfile" "${PACMAN_REPO_SERVER}/nonary-vibepollo.gpg"
  fingerprint=$(curl -fsSL --max-time 60 "${PACMAN_REPO_SERVER}/nonary-vibepollo-fingerprint.txt" | tr -d '[:space:]')
  [[ "$fingerprint" =~ ^[0-9A-Fa-f]{40}$ ]] || die 'the published key fingerprint is malformed; refusing to trust it'
  pacman-key --add "$keyfile"
  pacman-key --lsign-key "$fingerprint"

  log "Adding the [${PACMAN_REPO_NAME}] repository to pacman"
  install -Dm644 /dev/stdin "$PACMAN_REPO_CONF" <<EOF
[${PACMAN_REPO_NAME}]
SigLevel = Required
Server = ${PACMAN_REPO_SERVER}
EOF
  if ! grep -qxF "Include = ${PACMAN_REPO_CONF}" /etc/pacman.conf; then
    printf '\nInclude = %s\n' "$PACMAN_REPO_CONF" >>/etc/pacman.conf
  fi
}

install_from_repo() {
  configure_pacman_repo
  if ! pacman -Si vibepollo >/dev/null 2>&1; then
    warn 'Vibepollo repository metadata is unavailable locally; using a release package without refreshing system databases.'
    download_release_package
    install_from_package
    return
  fi
  prepare_driver_replacement
  log 'Installing Vibepollo and its dependencies; no full system upgrade is requested'
  if [[ -n "$requested_version" ]]; then
    local arch_version="${requested_version//-/}"
    arch_version="${arch_version//+/.}"
    pacman -S "${replacement_confirm[@]}" "${driver_overwrite[@]}" "vibepollo=${arch_version}-1"
  else
    pacman -S "${replacement_confirm[@]}" "${driver_overwrite[@]}" vibepollo
  fi
}

download_release_package() {
  local releases tag asset_url asset_name selector
  log 'Looking up the newest Vibepollo release on GitHub'
  command -v curl >/dev/null 2>&1 || die 'curl is required to download the release'
  if ! command -v jq >/dev/null 2>&1; then
    pacman -S --needed "${pacman_confirm[@]}" jq
  fi
  if [[ -n "$requested_version" ]]; then
    releases=$(curl -fsSL --max-time 60 -H 'Accept: application/vnd.github+json' \
      "${API_URL}/releases/tags/${requested_version}") ||
      die "release ${requested_version} was not found at ${REPO_URL}/releases"
    releases="[${releases}]"
  else
    releases=$(curl -fsSL --max-time 60 -H 'Accept: application/vnd.github+json' \
      "${API_URL}/releases?per_page=30") || die 'could not query GitHub releases'
  fi
  if ((allow_prerelease)); then
    selector='.[] | select(.draft == false)'
  else
    selector='.[] | select(.draft == false and .prerelease == false)'
  fi
  # Releases are listed newest first; pick the first one that carries an Arch package.
  read -r tag asset_name asset_url < <(jq -r "
    [${selector} | . as \$r | .assets[]
      | select(.name | test(\"^vibepollo-.*\\\\.pkg\\\\.tar\\\\.zst\$\"))
      | select(.name | test(\"-debug-\") | not)
      | [\$r.tag_name, .name, .browser_download_url] | @tsv] | first // empty" <<<"$releases")
  [[ -n "${asset_url:-}" ]] ||
    die "no release with an Arch package was found (see ${REPO_URL}/releases)"
  log "Downloading ${asset_name} from release ${tag}"
  local_package="${workdir}/${asset_name}"
  curl -fL --progress-bar --max-time 900 -o "$local_package" "$asset_url"

  # Verify against the release provenance manifest when the release ships one.
  local provenance expected actual
  provenance=$(curl -fsSL --max-time 60 \
    "${REPO_URL}/releases/download/${tag}/release-provenance.json" 2>/dev/null || true)
  if [[ -n "$provenance" ]]; then
    expected=$(jq -r --arg n "$asset_name" '.assets[$n] // empty' <<<"$provenance" | awk '{print $1}')
    if [[ "$expected" =~ ^[0-9a-f]{64}$ ]]; then
      actual=$(sha256sum "$local_package" | awk '{print $1}')
      [[ "$actual" == "$expected" ]] || die "checksum mismatch for ${asset_name}; refusing to install"
      ok "Package checksum matches the release provenance."
    fi
  fi
}

# Local driver updates can add source files that the installed package does not
# own. Pacman cannot remove those files when replacing that package. Preserve
# them before allowing exact-path replacement; never disable conflict checking
# for the whole driver tree (or for files another package owns).
prepare_driver_replacement() {
  # Optional roots are for isolated fixtures; production callers use no args.
  local source_root=${1:-/usr/src} backup_root=${2:-/var/tmp}
  local directory file owner attributes cursor status backup='' relative
  driver_overwrite=()
  for directory in "$source_root"/vibeshine-drm-*; do
    [[ ${directory##*/} =~ ^vibeshine-drm-[1-9][0-9]*\.[0-9]+\.[0-9]+$ ]] || continue
    [[ -d "$directory" && ! -L "$directory" ]] || continue
    # Require the directory itself to belong to a known host package. This is
    # not permission to adopt arbitrary files elsewhere in /usr/src.
    owner=$(LC_ALL=C pacman -Qoq -- "$directory" 2>/dev/null) || continue
    case "$owner" in sunshine|vibeshine|vibepollo) ;; *) continue ;; esac
    cursor=$directory
    while [[ "$cursor" != / ]]; do
      [[ -d "$cursor" && ! -L "$cursor" ]] || die "unsafe driver source parent: $cursor"
      attributes=$(stat -c '%u %a' -- "$cursor") || die "cannot inspect $cursor"
      read -r owner status <<<"$attributes"
      [[ "$owner" == "$EUID" && "$status" =~ ^[0-7]{3,4}$ ]] &&
        (( (8#$status & 0022) == 0 )) || die "untrusted driver source parent: $cursor"
      cursor=${cursor%/*}; [[ -n "$cursor" ]] || cursor=/
    done
    for file in "$directory"/*; do
      [[ ${file##*/} =~ ^[A-Za-z0-9_][A-Za-z0-9_.-]*$ ]] || continue
      [[ -f "$file" && ! -L "$file" ]] || continue
      if owner=$(LC_ALL=C pacman -Qoq -- "$file" 2>"$workdir/driver-owner-error"); then
        continue
      else
        status=$?
        [[ $status == 1 ]] && grep -Fxq -- "error: No package owns $file" "$workdir/driver-owner-error" ||
          die "could not establish package ownership of $file; refusing overwrite"
      fi
      if [[ -z "$backup" ]]; then
        backup=$(mktemp -d "$backup_root/vibepollo-driver-backup.XXXXXXXX") || die 'cannot create driver backup'
        chmod 700 "$backup" || die 'cannot protect driver backup'
        log "Preserving unowned legacy driver sources in $backup (retained even if installation fails)"
      fi
      relative="${directory##*/}/${file##*/}"
      mkdir -p -m 700 -- "$backup/${directory##*/}" || die 'cannot create driver backup directory'
      cp -a -- "$file" "$backup/$relative" || die "cannot back up $file"
      cmp -s -- "$file" "$backup/$relative" || die "driver source changed during backup: $file"
      driver_overwrite+=(--overwrite "usr/src/$relative")
    done
  done
}

install_from_package() {
  [[ -f "$local_package" ]] || die "package file not found: ${local_package}"
  local identity
  identity=$(pacman -Qp -- "$local_package") || die 'could not inspect the local package'
  [[ "$identity" == 'vibepollo '* && "$identity" != *$'\n'* ]] || die 'local package is not Vibepollo'
  prepare_driver_replacement
  log "Installing ${local_package##*/} with pacman"
  # Installing a local build must not also upgrade the operating system.
  pacman -U "${replacement_confirm[@]}" "${driver_overwrite[@]}" -- "$local_package"
}

install_vibepollo() {
  log 'Vibepollo replaces conflicting Sunshine and Vibeshine packages in the same package transaction.'
  log 'Original legacy profiles are retained. Migration preserves identity, credentials, pairings and applications.'
  log 'Active streams disconnect during replacement; no existing host is removed before its replacement is available.'
  workdir=$(mktemp -d)
  if [[ -n "$local_package" ]]; then
    install_from_package
    return
  fi
  if ((use_repo)) && repo_is_available; then
    install_from_repo
    return
  fi
  if ((use_repo)); then
    warn "The signed repository at ${PACMAN_REPO_SERVER} is not reachable; falling back to GitHub releases."
  fi
  download_release_package
  install_from_package
}

open_firewall() {
  if systemctl is-active --quiet firewalld 2>/dev/null; then
    log 'Opening the Vibepollo ports in firewalld'
    firewall-cmd --permanent --add-service=vibepollo >/dev/null && firewall-cmd --reload >/dev/null &&
      ok 'firewalld: service "vibepollo" allowed.' ||
      warn 'firewalld: could not add the vibepollo service; run: sudo firewall-cmd --permanent --add-service=vibepollo && sudo firewall-cmd --reload'
  elif command -v ufw >/dev/null 2>&1 && LC_ALL=C ufw status 2>/dev/null | grep -q '^Status: active'; then
    log 'Opening the Vibepollo ports in ufw'
    ufw allow Vibepollo >/dev/null && ok 'ufw: application profile "Vibepollo" allowed.' ||
      warn 'ufw: could not allow the Vibepollo profile; run: sudo ufw allow Vibepollo'
  else
    ok 'No active firewalld or ufw detected; nothing to open.'
  fi
}

reboot_required=0

install_virtual_driver() {
  local helper=${1:-$DRM_INSTALL} result=0 installed
  [[ -x "$helper" ]] || die "the package is missing the virtual-display installer: $helper"
  # Package hooks cannot reliably fail the package transaction. Check their
  # result here and repair a missing/stale module instead of only warning.
  if "$helper" status; then result=0; else result=$?; fi
  if [[ $result != 0 && $result != 4 ]]; then
    install_kernel_headers
    log "Installing the virtual-display driver for ${kernel_release}"
    if "$helper" install-package; then result=0; else result=$?; fi
  fi
  case "$result" in
    0) ;;
    4) reboot_required=1; warn 'The virtual-display driver is installed; reboot to use the updated module.' ;;
    5) reboot_required=1; warn 'The virtual-display driver is installed; reboot and approve its pending Secure Boot key enrollment.' ;;
    *) die "virtual-display driver installation failed for ${kernel_release} (status $result). See the driver error above; installation is not complete." ;;
  esac
  installed=$(modinfo -k "$kernel_release" -F version vibeshine_drm 2>/dev/null) && [[ -n "$installed" ]] ||
    die "the virtual-display driver is still missing for ${kernel_release}; installation is not complete"
  ok "Virtual-display driver ${installed} is installed for ${kernel_release}."
}

check_driver_state() {
  local installed loaded
  [[ -x "$DRM_INSTALL" ]] || return
  installed=$(modinfo -F version vibeshine_drm 2>/dev/null || true)
  loaded=$(cat /sys/module/vibeshine_drm/version 2>/dev/null || true)
  if [[ -z "$installed" ]]; then
    warn "The virtual-display driver is not installed for ${kernel_release}. Run: sudo ${DRM_INSTALL} install"
  elif [[ -z "$loaded" ]]; then
    reboot_required=1
    warn "The virtual-display driver ${installed} is installed but not loaded; reboot before streaming."
  elif [[ "$installed" != "$loaded" ]]; then
    reboot_required=1
    warn "Driver ${installed} is installed but ${loaded} is still loaded; reboot before streaming."
  else
    ok "Virtual-display driver ${installed} is installed and loaded."
  fi
  if command -v mokutil >/dev/null 2>&1 && LC_ALL=C mokutil --list-new 2>/dev/null | grep -q 'Subject'; then
    reboot_required=1
    warn 'A Secure Boot key enrollment is pending. Reboot, choose "Enroll MOK" in the blue MOK Manager screen, and confirm once.'
  fi
}

check_session_restart() {
  # First installation adds KWin startup hooks. A running greeter/desktop
  # predates those hooks even when the newly installed DRM module loads.
  if ! pacman -Q vibepollo >/dev/null 2>&1; then
    reboot_required=1
    warn 'First installation requires a reboot so the login screen and desktop load the Vibepollo display and session integration.'
  fi
}

check_services() {
  if systemctl is-enabled --quiet vibepollo-session-controller.service 2>/dev/null; then
    ok 'vibepollo-session-controller.service is enabled.'
  else
    warn "The session controller is not enabled. If the install printed an ACTION REQUIRED line about choosing a user, run: sudo ${MACHINE_HOST} configure YOUR_USER && sudo systemctl enable --now vibepollo-session-controller.service"
    reboot_required=1
  fi
}

print_summary() {
  printf '\n'
  log 'Vibepollo is installed. Next steps:'
  local step=1
  if ((reboot_required)); then
    printf '    %d. Reboot now. The display/session integration, virtual-display driver, Secure Boot key, or service state requires it.\n' "$step"
    step=$((step + 1))
  fi
  printf '    %d. Log in to your KDE Plasma (Wayland) desktop.\n' "$step"; step=$((step + 1))
  printf '    %d. Open https://localhost:47990 on this machine, create the Web UI login, then pair Moonlight with the PIN.\n' "$step"; step=$((step + 1))
  printf '       Pairing also works at the login screen; enter the PIN in the Web UI from another device.\n'
  printf '    %d. Log out and back in once (or restart PipeWire) so the audio quantum drop-in takes effect.\n' "$step"; step=$((step + 1))
  printf '\n    Status:  sudo systemctl status vibepollo-session-controller.service vibepollo.service\n'
  printf '    Logs:    sudo journalctl -u vibepollo-session-controller.service -u vibepollo.service -b\n'
  printf '    Guide:   %s/blob/vibe-test/docs/linux/install.md\n' "$REPO_URL"
  if ((${#warnings[@]} > 0)); then
    printf '\n    Warnings raised during installation:\n'
    local w
    for w in "${warnings[@]}"; do
      printf '      - %s\n' "$w"
    done
  fi
}

main() {
  parse_args "$@"
  require_root
  require_pacman
  run_checks
  install_kernel_headers
  check_session_restart
  install_vibepollo
  install_virtual_driver
  open_firewall
  check_driver_state
  check_services
  print_summary
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then main "$@"; fi
