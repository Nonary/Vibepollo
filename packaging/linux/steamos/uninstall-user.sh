#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

die() {
  printf 'uninstall-user.sh: %s\n' "$*" >&2
  exit 1
}

[[ $# -eq 0 ]] || die "this command takes no arguments"
[[ $(id -u) -ne 0 ]] || die "run this as the SteamOS desktop user, not root"

data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
config_home=${XDG_CONFIG_HOME:-"$HOME/.config"}
install_root="$data_home/vibepollo-steamos"
unit="$config_home/systemd/user/vibepollo-steamos.service"
launcher="$HOME/.local/bin/vibepollo-steamos-session"

[[ "$HOME" == /* && "$data_home" == /* && "$config_home" == /* ]] || \
  die "HOME and XDG base directories must be absolute paths"
install -d -- "$data_home"
exec 9> "$data_home/.vibepollo-steamos.lock"
flock -n 9 || die "another install or uninstall is running"

# Refuse an unexpectedly broad target even if the caller supplied hostile XDG
# variables. The exact leaf is the only recursively removed directory.
case "$install_root" in
  /*/vibepollo-steamos) ;;
  *) die "refusing unsafe install root: $install_root" ;;
esac

systemctl --user disable --now vibepollo-steamos.service
rm -f -- "$unit" "$launcher"
rm -rf -- "$install_root"
systemctl --user daemon-reload
systemctl --user reset-failed vibepollo-steamos.service 2>/dev/null || true

printf 'Removed the SteamOS user installation. Vibepollo configuration and pairing state were preserved.\n'
