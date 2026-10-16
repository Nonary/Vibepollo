#!/usr/bin/env bash
# Seed makepkg's VCS mirror only from the verified immutable controller checkout.
set -euo pipefail
checkout=$1
mirror=$2
canonical_origin=$3
expected_commit=$4
[[ "$expected_commit" =~ ^[0-9a-f]{40}$ ]] || { echo 'Expected a full immutable source commit.' >&2; exit 1; }
[[ "$canonical_origin" =~ ^https://github\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+(\.git)?$ ]] || { echo 'Expected canonical GitHub HTTPS origin.' >&2; exit 1; }
checkout=$(cd "$checkout" && pwd -P)
actual_origin=$(git -c safe.directory="$checkout" -C "$checkout" remote get-url origin)
[[ "${actual_origin%.git}" == "${canonical_origin%.git}" ]] || { echo 'Controller origin mismatch.' >&2; exit 1; }
[[ $(git -c safe.directory="$checkout" -C "$checkout" rev-parse HEAD) == "$expected_commit" ]] || { echo 'Controller commit mismatch.' >&2; exit 1; }
git -c safe.directory="$checkout" -C "$checkout" diff --quiet
git -c safe.directory="$checkout" -C "$checkout" diff --cached --quiet
expected_tree=$(git -c safe.directory="$checkout" -C "$checkout" rev-parse "$expected_commit^{tree}")
[[ ! -e "$mirror" ]] || { echo 'Source mirror must be fresh.' >&2; exit 1; }
mkdir -p "$(dirname "$mirror")"
staging=$(mktemp -d "${mirror}.partial.XXXXXX")
trap 'rm -rf -- "$staging"' EXIT
# File transport, depth one and no local optimizations: no hardlinks/alternates.
git -c safe.directory="$checkout" clone --bare --depth 1 --no-local --no-hardlinks "file://$checkout" "$staging"
git --git-dir="$staging" remote set-url origin "$canonical_origin"
[[ $(git --git-dir="$staging" rev-parse --is-bare-repository) == true ]]
[[ $(git --git-dir="$staging" rev-parse HEAD) == "$expected_commit" ]]
[[ $(git --git-dir="$staging" rev-parse 'HEAD^{tree}') == "$expected_tree" ]]
[[ $(git --git-dir="$staging" remote get-url origin) == "$canonical_origin" ]]
[[ ! -e "$staging/objects/info/alternates" ]]
mv -- "$staging" "$mirror"
trap - EXIT
printf 'Seeded exact source commit=%s tree=%s origin=%s\n' "$expected_commit" "$expected_tree" "$canonical_origin"
