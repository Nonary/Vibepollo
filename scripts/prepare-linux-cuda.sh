#!/bin/bash
set -euo pipefail
# makepkg already verifies the pinned installer checksum. Re-check it here to
# bind reuse to the actual archive, patch and host architecture, never a build dir.
installer=$1
destination=$2
patch_file=$(realpath "$3")
recipe=$( { sha256sum "$installer" "$patch_file" | cut -d' ' -f1; uname -m; } | sha256sum | cut -d' ' -f1)
toolkit=${CUDA_CACHE_DIR:-$destination}
if [[ -n "${CUDA_CACHE_DIR:-}" ]]; then
  toolkit="$CUDA_CACHE_DIR/toolkit"
fi
if [[ -f "$toolkit/.verified-recipe" ]] &&
   [[ "$(cat "$toolkit/.verified-recipe")" == "$recipe" ]] &&
   "$toolkit/bin/nvcc" --version | grep -Fq 'release 12.9,'; then
  echo 'Reusing exact verified CUDA toolkit'
else
  # A mismatched entry fails instead of silently reusing incompatible headers.
  if [[ -e "$toolkit/.verified-recipe" ]]; then
    echo 'CUDA cache receipt mismatch; use a fresh cache namespace' >&2
    exit 1
  fi
  sh "$installer" --silent --toolkit --override \
    --toolkitpath="$toolkit" --no-opengl-libs --no-man-page --no-drm
  patch -p2 --forward --directory="$toolkit" < "$patch_file"
  "$toolkit/bin/nvcc" --version | grep -Fq 'release 12.9,'
  printf '%s\n' "$recipe" > "$toolkit/.verified-recipe"
fi
if [[ "$toolkit" != "$destination" ]]; then
  if [[ ! -L "$destination" ]] || [[ "$(readlink "$destination")" != "$toolkit" ]]; then
    ln -s "$toolkit" "$destination"
  fi
fi
