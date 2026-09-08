#!/usr/bin/env bash
set -euo pipefail
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd -P)
source "$repo/scripts/linux_install.sh"
workdir=$(mktemp -d /tmp/vibepollo-installer-test.XXXXXXXX)
local_package="$workdir/vibepollo.pkg.tar.zst"
touch "$local_package"
calls="$workdir/calls"
source_root="$workdir/usr/src"
backup_root="$workdir/backups"
mkdir -p "$source_root" "$backup_root"
# Route only the fixture through alternate roots; never inspect real driver files.
eval "$(declare -f prepare_driver_replacement | sed '1s/prepare_driver_replacement/prepare_driver_replacement_fixture/')"
prepare_driver_replacement() { prepare_driver_replacement_fixture "$source_root" "$backup_root"; }
stat() {
  # The production /usr/src ancestry is root-owned and not world-writable.
  # Our fixture's /tmp ancestor is intentionally the only mocked metadata.
  if [[ "${!#}" == /tmp ]]; then printf '%s 755\n' "$EUID"; else command stat "$@"; fi
}
package_name=vibepollo
directory_owner=vibeshine
ownership_error=0
repository_metadata_available=1
pacman() {
  printf '%s\n' "$*" >> "$calls"
  if [[ "$1" == -Si ]]; then ((repository_metadata_available)); return; fi
  if [[ "$1" == -Qp ]]; then printf '%s 1.0-1\n' "$package_name"; fi
  if [[ "$1" == -Qoq ]]; then
    local path=${!#}
    if [[ -d "$path" ]]; then
      printf '%s\n' "$directory_owner"
    elif [[ ${path##*/} == owned.h ]]; then
      printf 'unrelated-driver\n'
    else
      if ((ownership_error)); then printf 'error: database unavailable\n' >&2; return 1; fi
      printf 'error: No package owns %s\n' "$path" >&2
      return 1
    fi
  fi
}
parse_args --yes --source-profile vibeshine
[[ "$VIBEPOLLO_IMPORT_SOURCE" == vibeshine ]]
install_from_package
! grep -Eq -- '^-S' "$calls"
grep -Fx -- "-U --noconfirm --ask=4 -- $local_package" "$calls"
[[ $(wc -l < "$calls") == 2 ]]
: > "$calls"
package_name=unrelated
if (install_from_package); then exit 1; fi
[[ $(wc -l < "$calls") == 1 ]]
! grep -Eq -- '^-R|^-U|^-S' "$calls"
: > "$calls"
package_name=vibepollo
pacman_confirm=(); replacement_confirm=()
install_from_package
grep -Fx -- "-U -- $local_package" "$calls"
printf 'Native installer validates package identity and confines conflict answers to replacement.\n'

directory="$source_root/vibeshine-drm-1.19.0"
mkdir "$directory"
printf 'old driver header\n' > "$directory/vibeshine_drm_vrr.h"
printf 'owned\n' > "$directory/owned.h"
printf 'unrelated\n' > "$workdir/other.h"
ln -s "$workdir/other.h" "$directory/linked.h"
printf 'literal wildcard\n' > "$directory/unsafe*.h"
: > "$calls"
install_from_package
expected='--overwrite usr/src/vibeshine-drm-1.19.0/vibeshine_drm_vrr.h'
! grep -Eq -- '^-S' "$calls"
grep -Fx -- "-U $expected -- $local_package" "$calls"
[[ ${#driver_overwrite[@]} == 2 ]]
backups=("$backup_root"/vibepollo-driver-backup.*/vibeshine-drm-1.19.0/vibeshine_drm_vrr.h)
[[ ${#backups[@]} == 1 ]]
cmp "$directory/vibeshine_drm_vrr.h" "${backups[0]}"
[[ $(command stat -c %a "${backups[0]%/vibeshine-drm-1.19.0/*}") == 700 ]]
! grep -- '^-U.*owned.h\|^-U.*linked.h\|^-U.*unsafe' "$calls"

# No adoption from source directories owned by unknown packages.
directory_owner=unrelated
prepare_driver_replacement
[[ ${#driver_overwrite[@]} == 0 ]]
directory_owner=vibeshine

# Ownership-query failures are not evidence of an unowned file.
ownership_error=1
: > "$calls"
if (install_from_package); then exit 1; fi
! grep -Eq -- '^-S|^-U' "$calls"
ownership_error=0

# Do not follow source-directory symlinks or trust writable parents.
mkdir "$source_root/vibeshine-drm-2.0.0"
chmod 777 "$source_root/vibeshine-drm-2.0.0"
if (prepare_driver_replacement); then exit 1; fi
chmod 700 "$source_root/vibeshine-drm-2.0.0"
ln -s "$directory" "$source_root/vibeshine-drm-3.0.0"
prepare_driver_replacement
[[ ${#driver_overwrite[@]} == 2 ]]

# Repository installs use the same exact-path preflight, including --yes.
configure_pacman_repo() { :; }
parse_args --yes
: > "$calls"
install_from_repo
grep -Fx -- "-S --noconfirm --ask=4 $expected vibepollo" "$calls"
! grep -Eq -- '^-S[^ ]*[yu]' "$calls"
download_release_package() { printf 'download-release\n' >> "$calls"; }
repository_metadata_available=0
: > "$calls"
install_from_repo
grep -Fx -- 'download-release' "$calls"
grep -Fx -- "-U --noconfirm --ask=4 $expected -- $local_package" "$calls"
! grep -Eq -- '^-S[^ ]*[yu]' "$calls"
repository_metadata_available=1

# A failed package transaction leaves both the originals and private backups.
eval "$(declare -f pacman | sed '1s/pacman/pacman_fixture/')"
pacman() { if [[ "$1" == -U ]]; then return 1; fi; pacman_fixture "$@"; }
if (install_from_package); then exit 1; fi
cmp "$directory/vibeshine_drm_vrr.h" "${backups[0]}"

# A backup failure must stop before either pacman transaction starts.
cp() { return 1; }
: > "$calls"
if (install_from_package); then exit 1; fi
! grep -Eq -- '^-S|^-U' "$calls"
unset -f cp
printf 'Legacy driver leftovers are backed up and narrowly adopted without changing owned files.\n'

# Check the installed driver's status and build it if the package hook failed.
driver_calls="$workdir/driver-calls"
driver_helper="$workdir/driver-helper"
printf '%s\n' '#!/usr/bin/env bash' 'printf "%s\n" "$*" >> "$driver_calls"' \
  'if [[ "$1" == status ]]; then exit "$driver_status"; fi' \
  '[[ "$1" == install-package ]] || exit 99' 'exit "$driver_install_status"' > "$driver_helper"
chmod 700 "$driver_helper"
export driver_calls driver_status=3 driver_install_status=0
kernel_release=7.2.2-1-cachyos
modinfo() {
  [[ "$*" == "-k $kernel_release -F version vibeshine_drm" ]] || return 1
  ((module_present)) || return 1
  printf '1.19.0\n'
}
module_present=1
eval "$(declare -f install_kernel_headers | sed '1s/install_kernel_headers/install_kernel_headers_fixture/')"
install_kernel_headers() { printf 'headers\n' >> "$driver_calls"; }
install_virtual_driver "$driver_helper"
[[ $(<"$driver_calls") == $'status\nheaders\ninstall-package' ]]
for driver_install_status in 4 5; do
  reboot_required=0
  install_virtual_driver "$driver_helper"
  [[ $reboot_required == 1 ]]
done
driver_install_status=1
if (install_virtual_driver "$driver_helper"); then exit 1; fi
driver_install_status=0; module_present=0
if (install_virtual_driver "$driver_helper"); then exit 1; fi
module_present=1
for driver_status in 0 4; do
  : > "$driver_calls"
  install_virtual_driver "$driver_helper"
  [[ $(<"$driver_calls") == status ]]
done
if (install_virtual_driver "$workdir/missing-helper"); then exit 1; fi
headers_package=''
if (install_kernel_headers_fixture); then exit 1; fi
headers_package=missing-fixture-headers
if (install_kernel_headers_fixture); then exit 1; fi
driver_status=3
install_kernel_headers() { die 'fixture: matching headers unavailable'; }
: > "$driver_calls"
if (install_virtual_driver "$driver_helper"); then exit 1; fi
[[ $(<"$driver_calls") == status ]]
printf 'Driver installation is required; genuine build failures cannot report installer success.\n'

# Reproduce an upgraded kernel package while the removed old kernel still runs.
(
  modules_root="$workdir/modules"
  mkdir -p "$modules_root/7.2.3-1-cachyos/build"
  touch "$modules_root/7.2.3-1-cachyos/pkgbase" "$modules_root/7.2.3-1-cachyos/build/Makefile"
  uname() { printf '7.2.2-1-cachyos\n'; }
  pacman() {
    printf '%s\n' "$*" >> "$calls"
    [[ "$1" == -Qqo && "${!#}" == "$modules_root/7.2.3-1-cachyos/pkgbase" ]] || return 1
    printf 'linux-cachyos\n'
  }
  : > "$calls"
  if (check_kernel "$modules_root") > "$workdir/kernel-error" 2>&1; then exit 1; fi
  grep -F 'Reboot required: running kernel 7.2.2-1-cachyos' "$workdir/kernel-error"
  grep -F '7.2.3-1-cachyos (linux-cachyos)' "$workdir/kernel-error"
  ! grep -Eq -- '^-S|^-U' "$calls"

  # A relocated image still maps through package-owned pkgbase metadata.
  uname() { printf '7.2.3-1-cachyos\n'; }
  check_kernel "$modules_root"
  [[ "$kernel_package" == linux-cachyos && "$headers_package" == linux-cachyos-headers ]]

  # Unknown/orphan directories are not evidence of an installed boot target.
  pacman() { return 1; }
  kernel_release=7.2.2-1-cachyos
  check_retired_kernel "$modules_root"
  if kernel_package_for 7.2.3-1-cachyos "$modules_root"; then exit 1; fi
  # Reject ambiguous ownership instead of constructing an invalid package name.
  pacman() { printf 'linux-cachyos\nother-package\n'; }
  if kernel_package_for 7.2.3-1-cachyos "$modules_root"; then exit 1; fi
)
printf 'Retired running kernels require a reboot, not mismatched header installation.\n'

# First install must request a reboot even if driver status later reports loaded.
(
  pacman() { [[ "$*" == '-Q vibepollo' ]] && return "$package_status"; }
  package_status=1; reboot_required=0
  check_session_restart
  [[ $reboot_required == 1 ]]
  package_status=0
  check_session_restart
  [[ $reboot_required == 1 ]]
  reboot_required=0
  check_session_restart
  [[ $reboot_required == 0 ]]
)
printf 'Fresh installs require a compositor restart even when the driver loads immediately.\n'
