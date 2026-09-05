#!/usr/bin/env bash
set -euo pipefail

repository=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd -P)
# tests/ -> steamos/ -> linux/ -> packaging/ -> repository
build_root=$(mktemp -d /tmp/vibepollo-steamos-profile.XXXXXXXX)
trap 'rm -rf -- "$build_root"' EXIT

cmake -S "$repository" -B "$build_root/profile" \
  -DSUNSHINE_BUILD_STEAMOS=ON -DSUNSHINE_CONFIGURE_ONLY=ON > "$build_root/configure.log" 2>&1 || {
  cat "$build_root/configure.log"
  exit 1
}
test ! -e "$build_root/profile/vibeshine-vkms.service"
test ! -e "$build_root/profile/vibeshine-drm-install"
test ! -e "$build_root/profile/postinst"
grep -qx 'BUILD_VIBESHINE_KWIN_GPU_BRIDGE:BOOL=OFF' "$build_root/profile/CMakeCache.txt"
grep -qx 'SUNSHINE_ENABLE_CUDA:BOOL=OFF' "$build_root/profile/CMakeCache.txt"
grep -qx 'SUNSHINE_ENABLE_DRM:BOOL=OFF' "$build_root/profile/CMakeCache.txt"

# Exercise the shared Unix path handling: an absolute build prefix must not
# leak into either the installed assets or the SteamOS runtime definition.
cat > "$build_root/assets.cmake" <<'EOF'
set(CMAKE_INSTALL_PREFIX "/temporary/build/prefix")
set(SUNSHINE_ASSETS_DIR "share/vibepollo")
set(SUNSHINE_BUILD_STEAMOS ON)
include("${REPOSITORY}/cmake/compile_definitions/unix.cmake")
if(NOT SUNSHINE_ASSETS_DIR STREQUAL "share/vibepollo")
    message(FATAL_ERROR "SteamOS assets must remain bundle-relative")
endif()
set(SUNSHINE_BUILD_STEAMOS OFF)
include("${REPOSITORY}/cmake/compile_definitions/unix.cmake")
if(NOT SUNSHINE_ASSETS_DIR STREQUAL "/temporary/build/prefix/share/vibepollo")
    message(FATAL_ERROR "Native Linux assets must retain the install prefix")
endif()
EOF
cmake -DREPOSITORY="$repository" -P "$build_root/assets.cmake"

if cmake -S "$repository" -B "$build_root/conflicting" \
  -DSUNSHINE_BUILD_STEAMOS=ON -DSUNSHINE_BUILD_APPIMAGE=ON \
  -DSUNSHINE_CONFIGURE_ONLY=ON > "$build_root/conflicting.log" 2>&1; then
  printf 'ERROR: incompatible packaging profiles were accepted\n' >&2
  exit 1
fi
grep -q 'cannot be combined' "$build_root/conflicting.log"
printf 'SteamOS profile tests passed\n'
