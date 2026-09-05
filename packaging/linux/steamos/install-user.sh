#!/usr/bin/env bash
# Install a prebuilt Vibepollo payload without modifying SteamOS's read-only OS.
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

usage() {
  cat <<'EOF'
Usage: install-user.sh --payload DIR [--no-start | --no-enable]

DIR must contain an executable bin/vibepollo and any runtime assets beside it.
The payload is copied into the invoking user's XDG data directory and activated
atomically. This script must not be run with sudo.
--no-start enables the service without starting it.
--no-enable installs the files without enabling or starting the service.
EOF
}

die() {
  printf 'install-user.sh: %s\n' "$*" >&2
  exit 1
}

payload=
start=yes
enable=yes
while (($#)); do
  case "$1" in
    --payload)
      (($# >= 2)) || die "--payload requires a directory"
      payload=$2
      shift 2
      ;;
    --no-start)
      start=no
      shift
      ;;
    --no-enable)
      enable=no
      start=no
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "unknown option: $1"
      ;;
  esac
done

[[ $(id -u) -ne 0 ]] || die "run this as the SteamOS desktop user, not root"
[[ -n "$payload" && -d "$payload" && ! -L "$payload" ]] || die "--payload must name a real directory"
[[ -f "$payload/bin/vibepollo" && -x "$payload/bin/vibepollo" && ! -L "$payload/bin/vibepollo" ]] || \
  die "payload has no regular executable bin/vibepollo"

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
config_home=${XDG_CONFIG_HOME:-"$HOME/.config"}
install_root="$data_home/vibepollo-steamos"
release_root="$install_root/releases"
unit_dir="$config_home/systemd/user"
launcher="$HOME/.local/bin/vibepollo-steamos-session"

[[ "$HOME" == /* && "$data_home" == /* && "$config_home" == /* ]] || \
  die "HOME and XDG base directories must be absolute paths"

install -d -- "$data_home"
exec 9> "$data_home/.vibepollo-steamos.lock"
flock -n 9 || die "another install or uninstall is running"

install -d -m 0700 -- "$release_root"
stage=$(mktemp -d -- "$release_root/.install.XXXXXXXX")
transaction=$(mktemp -d -- "$install_root/.transaction.XXXXXXXX")
previous=$(readlink -- "$install_root/current" || true)
activated=no
[[ ! -e "$install_root/current" || -L "$install_root/current" ]] || die "current must be a release symlink"
[[ ! -e "$launcher" ]] || cp -a -- "$launcher" "$transaction/launcher"
[[ ! -e "$unit_dir/vibepollo-steamos.service" ]] || cp -a -- "$unit_dir/vibepollo-steamos.service" "$transaction/unit"
cleanup() {
  local status=$?
  if [[ "$status" -ne 0 && "$activated" == yes ]]; then
    printf 'Installation failed; restoring the previous release.\n' >&2
    if [[ -n "$previous" ]]; then
      ln -s -- "$previous" "$install_root/.rollback.$$"
      mv -Tf -- "$install_root/.rollback.$$" "$install_root/current"
    else
      rm -f -- "$install_root/current"
    fi
    if [[ -e "$transaction/launcher" ]]; then
      cp -a -- "$transaction/launcher" "$launcher"
    else
      rm -f -- "$launcher"
    fi
    if [[ -e "$transaction/unit" ]]; then
      cp -a -- "$transaction/unit" "$unit_dir/vibepollo-steamos.service"
    else
      systemctl --user disable vibepollo-steamos.service 2>/dev/null || true
      rm -f -- "$unit_dir/vibepollo-steamos.service"
    fi
    systemctl --user daemon-reload || true
    if [[ "$start" == yes && -n "$previous" ]]; then
      systemctl --user restart vibepollo-steamos.service || true
    fi
  fi
  if [[ -n ${stage:-} && -d "$stage" ]]; then
    rm -rf -- "$stage"
  fi
  if [[ -n ${link_tmp:-} && -L "$link_tmp" ]]; then
    rm -f -- "$link_tmp"
  fi
  rm -rf -- "$transaction"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

cp -a -- "$payload"/. "$stage"/
[[ -f "$stage/bin/vibepollo" && -x "$stage/bin/vibepollo" && ! -L "$stage/bin/vibepollo" ]] || \
  die "copied payload failed validation"
for asset in apps.json web/index.html web/v2/index.html; do
  [[ -f "$stage/share/vibepollo/$asset" ]] || die "payload is missing share/vibepollo/$asset"
done
if ! "$stage/bin/vibepollo" --help > "$transaction/preflight.log" 2>&1; then
  head -c 4096 "$transaction/preflight.log" >&2
  die "payload cannot run on this SteamOS version; the previous installation was preserved"
fi

release_name="release-$(date -u +%Y%m%dT%H%M%SZ)-$$"
release_dir="$release_root/$release_name"
mv -T -- "$stage" "$release_dir"
stage=

link_tmp="$install_root/.current.$$"
ln -s -- "releases/$release_name" "$link_tmp"
mv -Tf -- "$link_tmp" "$install_root/current"
link_tmp=
activated=yes

# The user manager need not inherit the install shell's XDG overrides. Pin
# them in the launcher with Bash quoting, never in an evaluated env file.
{
  printf '#!/usr/bin/env bash\n'
  printf 'export XDG_DATA_HOME=%q\n' "$data_home"
  printf 'export XDG_CONFIG_HOME=%q\n' "$config_home"
  tail -n +2 -- "$script_dir/vibepollo-steamos-session"
} > "$transaction/new-launcher"
install -Dm755 -- "$transaction/new-launcher" "$launcher"
install -Dm644 -- "$script_dir/vibepollo-steamos.service" \
  "$unit_dir/vibepollo-steamos.service"

systemctl --user daemon-reload
if [[ "$start" == yes ]]; then
  systemctl --user enable vibepollo-steamos.service
  # enable --now does not restart an already running host on upgrade.
  systemctl --user restart vibepollo-steamos.service
elif [[ "$enable" == yes ]]; then
  systemctl --user enable vibepollo-steamos.service
fi

printf 'Installed Vibepollo for %s. Previous release directories were preserved.\n' "${USER:-$(id -un)}"
