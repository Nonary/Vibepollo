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
#   --version VERSION     Install this exact release (for example 2.0.0).
#   --package FILE        Install a local vibepollo-*.pkg.tar.zst instead of downloading.
#   --stable              Ignore pre-releases when picking the newest GitHub release.
#   --no-repo             Skip the signed pacman repository and use GitHub releases.
#   --skip-checks         Continue past failed requirement checks (not recommended).
#   --yes                 Answer yes to pacman prompts.
#   --force-stop          Explicitly allow bounded forced shutdown during replacement.
#   --remove              Uninstall using --package FILE as the incoming shutdown policy.
#   -h, --help            Show this help.
#
# Re-running the script is safe; it only installs what is missing.

set -euo pipefail

# Requirement and package-ownership checks parse command output.
export LC_ALL=C

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
readonly DS5_INSTALL='/usr/libexec/vibeshine/vibeshine-ds5-install'
readonly MACHINE_HOST='/usr/libexec/vibeshine/vibepollo-machine-host'
readonly PRIVATE_HOST='/usr/libexec/vibeshine/vibepollo-host'
readonly SESSION_CONTROLLER='/usr/libexec/vibeshine/vibepollo-session-controller'

requested_version=''
local_package=''
allow_prerelease=1
use_repo=1
skip_checks=0
pacman_confirm=()
replacement_confirm=()
driver_overwrite=()
package_hook_args=()
check_failures=0
warnings=()
workdir=''
package_force_stop=0
package_transaction_pending=0
package_transaction_returned=0
package_transaction_pid=''
package_transaction_start_ticks=''
package_transaction_command=''
package_transaction_cancelled=0
package_transaction_spawn_pending=0
package_transaction_cancel_signal=''
package_transaction_run_id=''
package_database_lock=/var/lib/pacman/db.lck
remove_requested=0

usage() {
  sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
ok() { printf '    \033[1;32mOK\033[0m   %s\n' "$*"; }
warn() { printf '    \033[1;33mWARN\033[0m %s\n' "$*" >&2; warnings+=("$*"); }
fail() { printf '    \033[1;31mFAIL\033[0m %s\n' "$*" >&2; check_failures=$((check_failures + 1)); }
die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

run_from_root() (
  # Old package hooks and configure helpers drop UID without changing cwd.
  # Keep their children out of the invoking user's private home.
  cd / || die 'cannot enter a safe working directory'
  if [[ $(type -t -- "$1") = function ]]; then "$@"; else exec "$@"; fi
)

can_offer_package_force() {
  [[ -t 0 && -t 1 && ${#pacman_confirm[@]} == 0 ]] && package_force_is_applicable
}

package_force_is_applicable() (
  [[ ! -e "$package_database_lock" && ! -L "$package_database_lock" ]] || return 1
  [[ "$package_transaction_run_id" =~ ^[0-9a-f]{32}$ ]] || return 1
  local library="$workdir/arch-package-hooks"
  [[ -f "$library" && ! -L "$library" ]] || return 1
  export VIBEPOLLO_PACKAGE_RUN_ID="$package_transaction_run_id"
  source "$library" || return 1
  declare -F vibepollo_package_force_is_applicable >/dev/null || return 1
  vibepollo_package_force_is_applicable
)

new_package_transaction_run_id() {
  local value
  builtin read -r value < /proc/sys/kernel/random/uuid || return 1
  value=${value//-/}
  [[ "$value" =~ ^[0-9a-f]{32}$ ]] || return 1
  package_transaction_run_id=$value
}

package_transaction_exec() {
  # Called in the background shell owned by the transaction wrapper. Avoid
  # another subshell between its recorded PID and the package-manager client.
  cd / || return 1
  if [[ $(type -t -- "$1") = function ]]; then
    "$@"
  else
    [[ -x /usr/bin/setsid ]] || { warn 'Trusted setsid is required to isolate the package transaction from terminal cancellation.'; return 1; }
    # Caller disables job control before fork, so setsid execs the native
    # client in this same PID. Stdin remains inherited for package prompts;
    # tty Ctrl-C can reach only the installer, never its native scriptlets.
    exec /usr/bin/setsid -- "$@"
  fi
}


record_package_transaction_identity() {
  local process_stat process_tail
  local -a process_fields=()
  package_transaction_start_ticks=''
  [[ "$package_transaction_pid" =~ ^[1-9][0-9]*$ ]] || return 1
  builtin read -r process_stat 2>/dev/null < "/proc/$package_transaction_pid/stat" || return 1
  process_tail=${process_stat##*) }
  [[ "$process_tail" != "$process_stat" ]] || return 1
  builtin read -r -a process_fields <<< "$process_tail"
  # stat fields after the final comm delimiter start at state (field 3).
  [[ ${process_fields[1]:-} == "$BASHPID" &&
     ${process_fields[19]:-} =~ ^[0-9]+$ ]] || return 1
  package_transaction_start_ticks=${process_fields[19]}
}

package_transaction_signal() {
  package_transaction_cancelled=$2
  package_transaction_cancel_signal=$1
  local child_signal=$1
  if [[ "$package_transaction_command" = pacman ]]; then
    # Pacman's pre-transaction hook runs before its interruptible commit state;
    # even INT/HUP may exit the client while that hook is still running. Keep
    # the exact client alive and wait for all native hooks before recovery.
    warn 'Cancellation requested; the package transaction and its hooks must finish before recovery. The pending package replacement may complete.'
    return 0
  fi
  [[ -n "$package_transaction_pid" ]] || return 0
  if [[ -z "$package_transaction_start_ticks" ]]; then
    if ((package_transaction_spawn_pending == 0)) && [[ -d /proc/$package_transaction_pid ]]; then
      warn 'Cancellation target identity could not be established; waiting for the package-manager child before recovery.'
    fi
    return 0
  fi
  # Bash can reap a child between a jobs-table check and numeric kill. A pidfd
  # binds the signal to the validated original kernel process across PID reuse.
  # If trusted pidfd support is missing, keep waiting; never fall back to kill.
  if [[ ! -x /usr/bin/python3 ]] || ! /usr/bin/python3 -I - \
    "$BASHPID" "$package_transaction_pid" "$package_transaction_start_ticks" "$child_signal" <<'PY'
import os
import pathlib
import select
import signal
import sys

parent, target, start = map(int, sys.argv[1:4])
number = {"TERM": signal.SIGTERM, "INT": signal.SIGINT, "HUP": signal.SIGHUP}[sys.argv[4]]
if not hasattr(os, "pidfd_open") or not hasattr(signal, "pidfd_send_signal"):
    sys.exit(1)
if os.getppid() != parent:
    sys.exit(1)
descriptor = None
try:
    descriptor = os.pidfd_open(target)
    if select.select([descriptor], [], [], 0)[0]:
        sys.exit(0)
    fields = pathlib.Path(f"/proc/{target}/stat").read_text().rpartition(") ")[2].split()
    if len(fields) < 20 or int(fields[1]) != parent or int(fields[19]) != start:
        sys.exit(1)
    # A process can exit while its proc identity is being read. Poll the pinned
    # process again before signalling; a reused numeric PID cannot retarget it.
    if not select.select([descriptor], [], [], 0)[0]:
        signal.pidfd_send_signal(descriptor, number)
except ProcessLookupError:
    pass
except (OSError, ValueError):
    sys.exit(1)
finally:
    if descriptor is not None:
        os.close(descriptor)
PY
  then
    warn 'Cancellation could not safely signal the recorded package-manager child; waiting for it to exit before recovery.'
  fi
}

wait_package_transaction_child() {
  local result
  while :; do
    if wait -f "$package_transaction_pid"; then result=0; else result=$?; fi
    # A caught signal interrupts wait before the child exits. Reap the exact
    # child before the recovery callback can remove its package barriers.
    if jobs -pr | grep -qx "$package_transaction_pid"; then continue; fi
    break
  done
  package_transaction_pid=''
  package_transaction_start_ticks=''
  package_transaction_returned=1
  if ((package_transaction_cancelled)); then return "$package_transaction_cancelled"; fi
  return "$result"
}

run_package_transaction() {
  local status answer monitor_was_set=0
  new_package_transaction_run_id || return 1
  package_transaction_pid=''
  package_transaction_start_ticks=''
  package_transaction_command=$1
  package_transaction_cancelled=0
  package_transaction_pending=1
  package_transaction_returned=0
  if ((package_force_stop)); then
    warn 'Forced shutdown is enabled: active streams and applications may terminate, and cleanup may be incomplete.'
  fi
  package_transaction_spawn_pending=1
  monitor_was_set=0
  [[ $- != *m* ]] || monitor_was_set=1
  set +m
  VIBEPOLLO_PACKAGE_RUN_ID="$package_transaction_run_id" VIBEPOLLO_PACKAGE_FORCE="$package_force_stop" package_transaction_exec "$@" <&0 &
  package_transaction_pid=$!
  ((monitor_was_set == 0)) || set -m
  record_package_transaction_identity || true
  package_transaction_spawn_pending=0
  if ((package_transaction_cancelled)); then
    package_transaction_signal "$package_transaction_cancel_signal" "$package_transaction_cancelled"
  fi
  if wait_package_transaction_child; then
    package_transaction_returned=1
    if ((remove_requested)); then recover_failed_package_transaction; fi
    package_transaction_pending=0
    return 0
  else status=$?; fi
  package_transaction_returned=1
  if ((package_transaction_cancelled)); then
    recover_failed_package_transaction
    return "$status"
  fi
  if ! ((package_force_stop == 0)) || ! package_force_is_applicable || ! can_offer_package_force; then
    recover_failed_package_transaction
    return "$status"
  fi
  warn 'The package operation failed. Forced shutdown will terminate active streams and applications; cleanup may be incomplete.'
  if ! read -r -p 'Retry this package operation once with forced shutdown? [y/N] ' answer; then
    recover_failed_package_transaction
    return "$status"
  fi
  case "$answer" in y|Y|yes|YES) ;; *) recover_failed_package_transaction; return "$status" ;; esac
  package_transaction_returned=0
  package_transaction_spawn_pending=1
  monitor_was_set=0
  [[ $- != *m* ]] || monitor_was_set=1
  set +m
  VIBEPOLLO_PACKAGE_RUN_ID="$package_transaction_run_id" VIBEPOLLO_PACKAGE_FORCE=1 package_transaction_exec "$@" <&0 &
  package_transaction_pid=$!
  ((monitor_was_set == 0)) || set -m
  record_package_transaction_identity || true
  package_transaction_spawn_pending=0
  if ((package_transaction_cancelled)); then
    package_transaction_signal "$package_transaction_cancel_signal" "$package_transaction_cancelled"
  fi
  if wait_package_transaction_child; then
    package_transaction_returned=1
    if ((remove_requested)); then recover_failed_package_transaction; fi
    package_transaction_pending=0
    return 0
  else status=$?; fi
  package_transaction_returned=1
  recover_failed_package_transaction
  return "$status"
}

recover_failed_package_transaction() {
  ((package_transaction_pending)) || return 0
  # A cancellation while the package-manager child is still running cannot
  # release its barriers. Retry remains safe, and ownership evidence is kept.
  ((package_transaction_returned)) || return 0
  package_transaction_pending=0
  if [[ -e "$package_database_lock" || -L "$package_database_lock" ]]; then
    warn 'Package database is locked by another operation; its shutdown barriers were preserved.'
    return 0
  fi
  [[ "$package_transaction_run_id" =~ ^[0-9a-f]{32}$ ]] || return 0
  local library="$workdir/arch-package-hooks"
  [[ -f "$library" && ! -L "$library" ]] || return 0
  (
    export VIBEPOLLO_PACKAGE_RUN_ID="$package_transaction_run_id"
    source "$library" || exit 1
    declare -F vibepollo_package_run_is_owned >/dev/null || exit 0
    vibepollo_package_run_is_owned || exit 0
    if declare -F vibepollo_package_recover_failed_transaction >/dev/null; then
      vibepollo_package_recover_failed_transaction
    fi
  ) || warn 'Package shutdown recovery could not prove a safe release; retry retains its ownership evidence.'
}

cleanup() {
  if [[ -n "$package_transaction_pid" ]]; then
    wait_package_transaction_child || true
  fi
  recover_failed_package_transaction
  if [[ -n "$workdir" && -d "$workdir" ]]; then
    rm -rf -- "$workdir"
  fi
}
trap cleanup EXIT

installer_signal() {
  if [[ -n "$package_transaction_pid" ]]; then
    package_transaction_signal "$@"
  elif ((package_transaction_spawn_pending)); then
    # A signal between fork and $! assignment must not discard the only
    # ownership record. Deliver it after the exact child PID is recorded.
    package_transaction_cancelled=$2
    package_transaction_cancel_signal=$1
  else
    exit "$2"
  fi
}
trap 'installer_signal HUP 129' HUP
trap 'installer_signal INT 130' INT
trap 'installer_signal TERM 143' TERM

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
      --yes) pacman_confirm=(--noconfirm); replacement_confirm=(--noconfirm --ask=4); shift ;;
      --force-stop) package_force_stop=1; shift ;;
      --remove) remove_requested=1; shift ;;
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

prepare_package_hook_dirs() {
  # Pacman applies the default /etc hook directory only when no --hookdir or
  # configured HookDir exists. Preserve the effective configuration explicitly;
  # configured directories still append in Pacman and retain admin precedence.
  command -v pacman-conf >/dev/null 2>&1 ||
    die 'pacman-conf is required to preserve administrator transaction hooks'
  local resolved_hooks hook_dir hook_count=0
  resolved_hooks=$(pacman-conf HookDir) ||
    die 'could not read effective pacman hook directories; no transaction started'
  [[ -n "$resolved_hooks" ]] || die 'effective pacman hook directories are empty'
  local -a private_guards=("$workdir/hooks/"*.hook)
  [[ ${#private_guards[@]} == 1 && -f "${private_guards[0]}" && ! -L "${private_guards[0]}" ]] ||
    die 'private package transaction guard is missing or ambiguous'
  local guard_name=${private_guards[0]##*/}
  package_hook_args=()
  while IFS= read -r hook_dir; do
    [[ "$hook_dir" == /* && "$hook_dir" != *$'\r'* ]] ||
      die 'effective pacman hook directory must be absolute and contain no carriage return'
    # A configured same-name dummy hook can disable AbortOnFail, while the
    # default directory would be shadowed by our CLI guard. Preserve admin
    # policy in both cases by refusing the collision before any transaction.
    [[ ! -e "$hook_dir/$guard_name" && ! -L "$hook_dir/$guard_name" ]] ||
      die "administrator hook $hook_dir/$guard_name conflicts with the required shutdown guard; no transaction started"
    hook_count=$((hook_count + 1))
    ((hook_count <= 256)) || die 'too many effective pacman hook directories'
    package_hook_args+=(--hookdir "$hook_dir")
  done <<< "$resolved_hooks"
  package_hook_args+=(--hookdir "$workdir/hooks")
}

stage_upgrade_guard() {
  local candidate=$1 library="$workdir/arch-package-hooks" preflight="$workdir/preflight"
  local hook_dir="$workdir/hooks"
  local outgoing_library=${2:-/usr/share/vibepollo/arch-package-hooks}
  local hook_name=00-vibepollo-incoming-quiesce.hook
  vibepollo_compat_override_hook=0
  [[ -f "$candidate" ]] || die "package preflight candidate is missing: $candidate"
  command -v bsdtar >/dev/null 2>&1 || die 'bsdtar is required for the safe package preflight'
  mkdir -p -m 700 -- "$hook_dir" || die 'could not stage the package transaction guard'
  bsdtar -xOf "$candidate" .INSTALL > "$library" || die 'candidate has no Arch package hooks'
  chmod 0600 "$library" || die 'could not protect the staged package hooks'
  bash -n "$library" || die 'candidate package hooks have invalid shell syntax'
  grep -q '^vibepollo_quiesce_or_abort() [({]' "$library" ||
    die 'candidate lacks the required quiescence check'
  # Only these named, version/condition-scoped incoming steps may select an
  # exact outgoing-hook override. Unknown scripts in an archive never execute.
  local migration_entry migration migration_name installed_identity=''
  local archive_entries
  archive_entries=$(bsdtar -tf "$candidate") || die 'could not inspect candidate compatibility scripts'
  for migration_name in upgrade-2.0-lifecycle.sh upgrade-2.0.1-lifecycle.sh; do
    migration_entry=$(printf '%s\n' "$archive_entries" | grep -Fx \
      -e "usr/share/vibepollo/installer-migrations/$migration_name" \
      -e "./usr/share/vibepollo/installer-migrations/$migration_name" || true)
    [[ -n "$migration_entry" ]] || continue
    migration="$workdir/$migration_name"
    [[ "$migration_entry" != *$'\n'* ]] || die 'candidate has duplicate compatibility scripts'
    bsdtar -xOf "$candidate" "$migration_entry" > "$migration" || die 'could not extract incoming compatibility script'
    chmod 0600 "$migration" || die 'could not protect incoming compatibility script'
    bash -n "$migration" || die 'incoming compatibility script has invalid syntax'
    source "$migration" || die 'could not load incoming compatibility script'
    if [[ -z "$installed_identity" ]]; then installed_identity=$(pacman -Q vibepollo 2>/dev/null || true); fi
    case "$migration_name" in
      upgrade-2.0-lifecycle.sh)
        vibepollo_upgrade_2_0_lifecycle_compat "$installed_identity" "$outgoing_library" "$library" || die 'outgoing 2.0.0 lifecycle compatibility check failed' ;;
      upgrade-2.0.1-lifecycle.sh)
        vibepollo_upgrade_2_0_1_lifecycle_compat "$installed_identity" "$outgoing_library" "$library" || die 'outgoing 2.0.1 lifecycle compatibility check failed' ;;
    esac
  done
  if ((vibepollo_compat_override_hook)); then hook_name=00-vibepollo-quiesce.hook; fi
  # Re-staging in the same private directory must retire our previous choice,
  # especially an override whose legacy version/condition no longer matches.
  rm -f -- "$hook_dir/00-vibepollo-quiesce.hook" "$hook_dir/00-vibepollo-incoming-quiesce.hook" ||
    die 'could not retire the previous private transaction guard'
  printf '#!/usr/bin/bash\n. %q || exit 1\nvibepollo_quiesce_or_abort\n' "$library" > "$preflight"
  chmod 0700 "$preflight" || die 'could not protect the transaction preflight'
  cat > "$hook_dir/$hook_name" <<EOF
[Trigger]
Operation = Install
Operation = Upgrade
Operation = Remove
Type = Package
Target = vibepollo

[Action]
Description = Safely stopping Vibepollo before package replacement
When = PreTransaction
Exec = $preflight
AbortOnFail
EOF
  chmod 0600 "$hook_dir/$hook_name" || die 'could not protect the transaction hook'
}

install_from_repo() {
  configure_pacman_repo
  if ! pacman -Si vibepollo >/dev/null 2>&1; then
    warn 'Vibepollo repository metadata is unavailable locally; using a release package without refreshing system databases.'
    download_release_package
    install_from_package
    return
  fi
  local -a repo_target candidates
  if [[ -n "$requested_version" ]]; then
    local arch_version="${requested_version//-/}"
    arch_version="${arch_version//+/.}"
    repo_target=("vibepollo=${arch_version}-1")
  else
    repo_target=(vibepollo)
  fi
  mkdir -p -m 700 -- "$workdir/cache" || die 'could not stage the signed repository package'
  # Download and verify the exact signed candidate before executing its hook
  # under the package manager's transaction lock.
  pacman -Sw "${replacement_confirm[@]}" --cachedir "$workdir/cache" "${repo_target[@]}"
  mapfile -d '' -t candidates < <(find "$workdir/cache" -maxdepth 1 -type f \
    -name 'vibepollo-*.pkg.tar.*' ! -name '*.sig' -print0)
  [[ ${#candidates[@]} == 1 ]] || die 'expected exactly one verified Vibepollo package in the private cache'
  [[ $(pacman -Qp -- "${candidates[0]}") == 'vibepollo '* ]] ||
    die 'cached package identity is not Vibepollo'
  stage_upgrade_guard "${candidates[0]}"
  prepare_package_hook_dirs
  prepare_driver_replacement
  log 'Installing Vibepollo and its dependencies; no full system upgrade is requested'
  run_package_transaction pacman -S "${replacement_confirm[@]}" "${driver_overwrite[@]}" \
    --cachedir "$workdir/cache" "${package_hook_args[@]}" "${repo_target[@]}"
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

  # This is an explicit installation, never an availability check.
  local sums checksum_expected checksum_actual
  sums=$(curl -fsSL --max-time 60 "${REPO_URL}/releases/download/${tag}/SHA256SUMS" 2>/dev/null || true)
  if [[ -n "$sums" ]]; then
    checksum_expected=$(awk -v name="$asset_name" '$2 == name {print $1}' <<<"$sums")
    [[ "$checksum_expected" =~ ^[0-9a-f]{64}$ ]] || die "missing or ambiguous checksum for ${asset_name}"
    checksum_actual=$(sha256sum "$local_package" | awk '{print $1}')
    [[ "$checksum_actual" == "$checksum_expected" ]] || die "checksum mismatch for ${asset_name}; refusing to install"
    ok "Package checksum matches SHA256SUMS."
  elif [[ "$tag" == daily-* || "$tag" == *-daily.d* ]]; then
    die "nightly checksums unavailable; refusing to install"
  fi

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
  local source_root=${1:-/usr/src} backup_root=${2:-/var/tmp} install_root=${3:-/}
  local directory file owner attributes cursor status backup='' relative parent
  local -a package_files=(
    usr/lib/modules-load.d/70-vibeshine-ds5.conf
    usr/libexec/vibeshine/vibeshine-ds5-install
  )
  driver_overwrite=()
  for directory in "$source_root"/vibeshine-drm-* "$source_root"/vibeshine-ds5-*; do
    [[ ${directory##*/} =~ ^vibeshine-(drm|ds5)-[1-9][0-9]*\.[0-9]+\.[0-9]+$ ]] || continue
    [[ -d "$directory" && ! -L "$directory" ]] || continue
    # A manual driver repair can precede the first package containing it.
    # Accept a known host package or an explicitly unowned exact driver tree;
    # both still require trusted ancestry and per-file ownership checks below.
    if owner=$(LC_ALL=C pacman -Qoq -- "$directory" 2>"$workdir/driver-owner-error"); then
      case "$owner" in sunshine|vibeshine|vibepollo) ;; *) continue ;; esac
    else
      status=$?
      [[ $status == 1 ]] && grep -Fxq -- "error: No package owns $directory" "$workdir/driver-owner-error" ||
        die "could not establish package ownership of $directory; refusing overwrite"
    fi
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
        backup=$(mktemp -d "$backup_root/vibeshine-driver-backup.XXXXXXXX") || die 'cannot create driver backup'
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
  # Early DS5 development installs placed these package payloads manually.
  # Adopt only the two exact paths introduced by the native package, subject to
  # the same ownership, ancestry, backup, and byte-verification checks above.
  for relative in "${package_files[@]}"; do
    file="${install_root%/}/$relative"
    [[ -e "$file" || -L "$file" ]] || continue
    [[ -f "$file" && ! -L "$file" ]] || die "unsafe package replacement target: $file"
    if owner=$(LC_ALL=C pacman -Qoq -- "$file" 2>"$workdir/driver-owner-error"); then
      continue
    else
      status=$?
      [[ $status == 1 ]] && grep -Fxq -- "error: No package owns $file" "$workdir/driver-owner-error" ||
        die "could not establish package ownership of $file; refusing overwrite"
    fi
    cursor=${file%/*}
    while [[ "$cursor" != / ]]; do
      [[ -d "$cursor" && ! -L "$cursor" ]] || die "unsafe package replacement parent: $cursor"
      attributes=$(stat -c '%u %a' -- "$cursor") || die "cannot inspect $cursor"
      read -r owner status <<<"$attributes"
      [[ "$owner" == "$EUID" && "$status" =~ ^[0-7]{3,4}$ ]] &&
        (( (8#$status & 0022) == 0 )) || die "untrusted package replacement parent: $cursor"
      cursor=${cursor%/*}; [[ -n "$cursor" ]] || cursor=/
    done
    if [[ -z "$backup" ]]; then
      backup=$(mktemp -d "$backup_root/vibeshine-driver-backup.XXXXXXXX") || die 'cannot create driver backup'
      chmod 700 "$backup" || die 'cannot protect driver backup'
      log "Preserving unowned legacy install files in $backup (retained even if installation fails)"
    fi
    parent=${relative%/*}
    mkdir -p -m 700 -- "$backup/$parent" || die 'cannot create package-file backup directory'
    cp -a -- "$file" "$backup/$relative" || die "cannot back up $file"
    cmp -s -- "$file" "$backup/$relative" || die "package file changed during backup: $file"
    driver_overwrite+=(--overwrite "$relative")
  done
}

install_from_package() {
  [[ -f "$local_package" ]] || die "package file not found: ${local_package}"
  local identity package_path
  identity=$(pacman -Qp -- "$local_package") || die 'could not inspect the local package'
  [[ "$identity" == 'vibepollo '* && "$identity" != *$'\n'* ]] || die 'local package is not Vibepollo'
  package_path=$(readlink -f -- "$local_package") || die 'cannot resolve the local package path'
  stage_upgrade_guard "$local_package"
  prepare_package_hook_dirs
  prepare_driver_replacement
  log "Installing ${local_package##*/} with pacman"
  # Installing a local build must not also upgrade the operating system.
  run_package_transaction pacman -U "${replacement_confirm[@]}" "${driver_overwrite[@]}" \
    "${package_hook_args[@]}" -- "$package_path"
}

install_vibepollo() {
  workdir=$(mktemp -d /tmp/vibepollo-installer.XXXXXXXX)
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

remove_vibepollo() {
  [[ -n "$local_package" && -f "$local_package" ]] ||
    die '--remove requires --package FILE containing the incoming shutdown policy'
  local identity
  identity=$(pacman -Qp -- "$local_package") || die 'could not inspect the removal policy package'
  [[ "$identity" == 'vibepollo '* && "$identity" != *$'\n'* ]] || die 'removal policy package is not Vibepollo'
  workdir=$(mktemp -d /tmp/vibepollo-removal.XXXXXXXX)
  stage_upgrade_guard "$local_package"
  prepare_package_hook_dirs
  log 'Removing Vibepollo with the incoming shutdown guard; the policy package will not be installed'
  run_package_transaction pacman -R "${pacman_confirm[@]}" "${package_hook_args[@]}" -- vibepollo || return $?
  ok 'Vibepollo was removed; existing machine profile data is preserved.'
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

install_dualsense_driver() {
  local helper=${1:-$DS5_INSTALL} result=0
  [[ -x "$helper" ]] || die "the package is missing the DualSense USB installer: $helper"
  if "$helper" status; then result=0; else result=$?; fi
  if [[ $result != 0 && $result != 4 ]]; then
    install_kernel_headers
    log "Installing the DualSense USB driver for ${kernel_release}"
    if "$helper" install; then result=0; else result=$?; fi
  fi
  case "$result" in
    0) ok 'DualSense USB driver is installed and loaded.' ;;
    4) reboot_required=1; warn 'The DualSense USB driver is installed; reboot to use the updated module.' ;;
    *) die "DualSense USB driver installation failed (status $result); installation is not complete." ;;
  esac
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

repair_machine_setup() {
  local helper=${1:-$MACHINE_HOST} controller=${2:-$SESSION_CONTROLLER}
  local unit_root=${3:-/etc/systemd/system} unit=vibepollo-session-controller.service
  local path attributes owner group mode cursor temporary
  for path in "$helper" "$controller"; do
    [[ -f "$path" && ! -L "$path" && -x "$path" &&
       $(stat -c '%u:%g:%a:%h' -- "$path") == '0:0:755:1' ]] ||
      die "missing or unsafe machine setup helper: $path"
  done
  log 'Repairing machine setup from earlier installs; active streams disconnect during setup.'
  # systemd does not inherit the installer's LC_ALL. Older controllers need a
  # service-local override even though package hooks now run in the C locale.
  if ! head -n 12 -- "$controller" | grep -qx 'export LC_ALL=C'; then
    path="$unit_root/$unit.d/99-vibepollo-machine-locale.conf"
    [[ "$unit_root" == /* && "$unit_root" != */../* && "$unit_root" != */.. ]] ||
      die 'unsafe system unit directory'
    cursor=${path%/*}
    while [[ "$cursor" != / ]]; do
      if [[ -e "$cursor" || -L "$cursor" ]]; then
        [[ -d "$cursor" && ! -L "$cursor" ]] || die "unsafe service directory: $cursor"
        attributes=$(stat -c '%u:%g:%a' -- "$cursor") || die "cannot inspect $cursor"
        IFS=: read -r owner group mode <<< "$attributes"
        [[ "$owner:$group" == 0:0 && "$mode" =~ ^[0-7]{3,4}$ ]] &&
          (((8#$mode & 0022) == 0)) || die "unsafe service directory: $cursor"
      fi
      cursor=${cursor%/*}; [[ -n "$cursor" ]] || cursor=/
    done
    if [[ -e "$path" || -L "$path" ]]; then
      [[ -f "$path" && ! -L "$path" &&
         $(stat -c '%u:%g:%a:%h' -- "$path") == '0:0:644:1' ]] ||
        die "unsafe locale recovery file: $path"
    fi
    if [[ ! -d "${path%/*}" ]]; then
      install -d -m755 -- "${path%/*}" || die 'cannot create the service recovery directory'
    fi
    temporary=$(mktemp "${path%/*}/.locale.XXXXXXXX") || die 'cannot stage the service locale recovery'
    if ! printf '[Service]\nEnvironment=LC_ALL=C\n' > "$temporary" ||
       ! chmod 644 "$temporary" || ! mv -fT -- "$temporary" "$path"; then
      rm -f -- "$temporary"
      die 'cannot install the service locale recovery'
    fi
  fi
  systemctl daemon-reload || die 'cannot reload the repaired machine service'
  timeout --signal=KILL 40 systemctl stop "$unit" || die 'cannot stop the machine controller for setup'
  run_from_root timeout --signal=KILL 40 "$controller" cleanup ||
    die 'cannot safely drain the previous machine host; installation is not complete'
  run_from_root "$helper" configure-auto ||
    die 'automatic machine profile setup failed; installation is not complete'
  systemctl enable "$unit" || die 'cannot enable the configured machine controller'
  systemctl start "$unit" || die 'cannot start the configured machine controller'
  ok 'Machine profile is configured and the session controller is enabled.'
}

check_services() {
  if systemctl is-enabled --quiet vibepollo-session-controller.service 2>/dev/null; then
    ok 'vibepollo-session-controller.service is enabled.'
  else
    die 'the configured session controller is not enabled; installation is not complete'
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
  printf '    %d. Open https://localhost:47990 on this machine, create the Web UI login, then pair Moonlight with the PIN.\n' "$step"; step=$((step + 1))
  printf '       Pairing works at the login screen; enter the PIN in the Web UI from another device.\n'
  printf '    %d. Log in to your KDE Plasma (Wayland) desktop to stream the desktop.\n' "$step"; step=$((step + 1))
  printf '    %d. Log out and back in once (or restart PipeWire) so the audio quantum drop-in takes effect.\n' "$step"; step=$((step + 1))
  printf '\n    Status:  sudo systemctl status vibepollo-session-controller.service vibepollo.service\n'
  printf '    Logs:    sudo journalctl -u vibepollo-session-controller.service -u vibepollo.service -b\n'
  printf '    Guide:   %s/blob/master/docs/linux/install.md\n' "$REPO_URL"
  if ((${#warnings[@]} > 0)); then
    printf '\n    Warnings raised during installation:\n'
    local w
    for w in "${warnings[@]}"; do
      printf '      - %s\n' "$w"
    done
  fi
}

check_private_host_security() {
  local expected='cap_sys_admin,cap_sys_nice=p' actual
  [[ -e "$PRIVATE_HOST" ]] || return 0
  actual=$(getcap -n "$PRIVATE_HOST" 2>/dev/null || true)
  [[ "$actual" == "$PRIVATE_HOST $expected" ]] ||
    die 'installed private host executable has incorrect capabilities'
  ok 'Private host executable capabilities verified.'
}

main() {
  parse_args "$@"
  require_root
  require_pacman
  if ((remove_requested)); then
    remove_vibepollo
    return
  fi
  run_checks
  install_kernel_headers
  check_session_restart
  install_vibepollo
  check_private_host_security
  install_virtual_driver
  install_dualsense_driver
  repair_machine_setup || die 'machine setup recovery failed; installation is not complete'
  open_firewall
  check_driver_state
  check_services
  print_summary
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
  main "$@"
fi
