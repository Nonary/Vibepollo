#!/usr/bin/env bash

set -Eeuo pipefail
IFS=$'\n\t'

readonly expected_user="chasep"
readonly expected_home="/home/chasep"
readonly sysrq_config="/etc/sysctl.d/50-vibeshine-shutdown-debug.conf"
readonly netconsole_port=6666

target=""
revert=0

usage() {
  cat <<EOF
Instrument this host so the next shutdown hang is diagnosable.

The safe-deploy reboot wedges inside systemd-shutdown, after journald has
already stopped, so nothing about the hang reaches the on-disk journal. This
script routes both kernel and systemd messages to places that survive that
window, and enables the SysRq keys needed to dump and escape a wedge.

Usage: $(basename "$0") [--netconsole TARGET_IP] [--revert]

  --netconsole IP  Stream kmsg to IP:${netconsole_port} over UDP. On that
                   machine run:  nc -u -l ${netconsole_port} | tee hang.log
  --revert         Undo everything this script changed.
  -h, --help       Show this help.

Run this as ${expected_user}, without sudo. It asks for sudo once.
EOF
}

log() {
  printf '[shutdown-debug] %s\n' "$*"
}

die() {
  printf '[shutdown-debug] ERROR: %s\n' "$*" >&2
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --netconsole)
      [[ $# -ge 2 ]] || die "--netconsole requires a target IP"
      target="$2"
      shift 2
      ;;
    --revert)
      revert=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      usage >&2
      die "unknown argument: $1"
      ;;
  esac
done

[[ $EUID -ne 0 ]] || die "run this as ${expected_user}, not with sudo"
[[ $(id -un) == "$expected_user" ]] || die "this host script must be run as ${expected_user}"
[[ $HOME == "$expected_home" ]] || die "expected HOME=${expected_home}, got ${HOME}"

for command_name in ip modprobe sudo sysctl systemd-analyze; do
  command -v "$command_name" >/dev/null 2>&1 || die "required command not found: $command_name"
done

if (( revert )); then
  log "restoring systemd log level/target"
  sudo systemd-analyze log-level info
  sudo systemd-analyze log-target journal
  log "removing ${sysrq_config}"
  sudo rm -f -- "$sysrq_config"
  sudo sysctl --system >/dev/null
  if [[ -d /sys/module/netconsole ]]; then
    log "unloading netconsole"
    sudo modprobe -r netconsole || log "netconsole was busy; it will be gone after the next boot"
  fi
  log "reverted"
  exit 0
fi

# SysRq bit 8 (debugging dumps) is what makes Alt+SysRq+W usable, and the
# safe-deploy script's runtime-only value of 176 leaves it out. Persist the
# full mask so the keys are live during the shutdown window too, not just
# until the next boot.
log "enabling the full SysRq mask (persisted in ${sysrq_config})"
printf '# Vibeshine shutdown-hang diagnosis. Remove with --revert.\nkernel.sysrq = 1\nkernel.printk = 7 4 1 7\n' | \
  sudo tee "$sysrq_config" >/dev/null
sudo sysctl -q -p "$sysrq_config"

# systemd-shutdown inherits PID 1's log level and target. Pointing it at kmsg
# is what makes its final progress visible on the console and to netconsole
# after journald is gone.
log "routing systemd (and systemd-shutdown) logging to kmsg at debug level"
sudo systemd-analyze log-level debug
sudo systemd-analyze log-target kmsg

if [[ -n "$target" ]]; then
  [[ "$target" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ ]] || die "not an IPv4 address: $target"

  read -r source_ip source_dev <<<"$(
    ip -o -4 route get "$target" | sed -n 's/.* dev \([^ ]*\) src \([^ ]*\).*/\2 \1/p'
  )"
  [[ -n "${source_ip:-}" && -n "${source_dev:-}" ]] || die "no route to $target"

  # netconsole does not ARP; it needs the destination link address up front.
  ping -c 1 -W 2 "$target" >/dev/null 2>&1 || true
  target_mac="$(ip neigh show "$target" dev "$source_dev" | sed -n 's/.*lladdr \([0-9a-f:]*\).*/\1/p')"
  [[ -n "$target_mac" ]] || die "could not resolve the MAC for $target; is it reachable on $source_dev?"

  log "arming netconsole ${source_ip}/${source_dev} -> ${target}:${netconsole_port} (${target_mac})"
  sudo modprobe -r netconsole 2>/dev/null || true
  sudo modprobe netconsole \
    "netconsole=@${source_ip}/${source_dev},${netconsole_port}@${target}/${target_mac}"

  cat <<EOF

[shutdown-debug] On ${target}, start the collector BEFORE rebooting:

    nc -u -l ${netconsole_port} | tee vibeshine-hang.log

EOF
fi

cat <<EOF
[shutdown-debug] Instrumentation is live. Reproduce protocol:

  1. Run the safe deploy reboot as usual.
  2. When it wedges, the console now shows systemd-shutdown's own progress.
     Note the LAST line printed -- that names the phase that hung.
  3. Press Alt+SysRq+W  (dump tasks blocked in uninterruptible sleep).
     Then Alt+SysRq+T   (dump all task stacks) if W is not conclusive.
     Both land on the console and in the netconsole log.
  4. Escape without cutting power:  Alt+SysRq+S, then U, then B.

[shutdown-debug] Bisect worth running once, to split kernel from userspace:

    sudo systemctl stop vibeshine-vkms.service
    sudo modprobe -r vibeshine_drm
    sudo systemctl reboot

  Reboots cleanly  -> the wedge is in the vibeshine DRM module's shutdown path.
  Still wedges     -> the wedge is below it (NVIDIA device_shutdown, or btrfs).

[shutdown-debug] Undo everything with: $(basename "$0") --revert
EOF
