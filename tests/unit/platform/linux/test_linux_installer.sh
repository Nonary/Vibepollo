#!/usr/bin/env bash
set -euo pipefail
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd -P)
source "$repo/scripts/linux_install.sh"
workdir=$(mktemp -d /tmp/vibepollo-installer-test.XXXXXXXX)
local_package="$workdir/vibepollo.pkg.tar.zst"
touch "$local_package"
calls="$workdir/calls"
package_name=vibepollo
pacman() {
  printf '%s\n' "$*" >> "$calls"
  if [[ "$1" == -Qp ]]; then printf '%s 1.0-1\n' "$package_name"; fi
}
parse_args --yes --source-profile vibeshine
[[ "$VIBEPOLLO_IMPORT_SOURCE" == vibeshine ]]
install_from_package
grep -Fx -- '-Syu --noconfirm' "$calls"
grep -Fx -- "-U --noconfirm --ask=4 -- $local_package" "$calls"
[[ $(wc -l < "$calls") == 3 ]]
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
