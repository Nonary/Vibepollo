#!/usr/bin/env bash
# Apply one reviewed app-only commit to the original qualified cached checkout.
# Build and inspect locally; never install, change services, sign drivers or reboot.
set -euo pipefail
(( EUID != 0 )) || { echo 'Run without sudo.' >&2; exit 1; }
revision=${1:?Pass the full reviewed runtime-fix commit SHA}
[[ "$revision" =~ ^[0-9a-f]{40}$ ]] || { echo 'Expected a full commit SHA.' >&2; exit 2; }
state="${XDG_DATA_HOME:-$HOME/.local/share}/vibepollo-bazzite"
source_dir=${2:-"$state/source-efd8d7341dba879174529f7319a89d8d0332cb74"}
for tool in podman git curl python3 flock; do command -v "$tool" >/dev/null; done
[[ -f "$source_dir/build-local/CMakeCache.txt" ]] || { echo "Cached build missing: $source_dir" >&2; exit 1; }
image=localhost/vibepollo-bazzite-builder:f44-gcc14-v1
podman image exists "$image" || { echo 'Existing Fedora build image is missing.' >&2; exit 1; }
mkdir -p "$state/logs"
exec 9>"$state/build.lock"
flock -n 9 || { echo 'Another local build is running.' >&2; exit 1; }
output=$(mktemp -d "$state/runtime-fix-XXXXXXXX")
log="$output/build.log"
echo "Log: $log"
(
  curl --fail --location --proto '=https' --proto-redir '=https' \
    "https://github.com/Jalawoud/Vibepollo/commit/$revision.patch" -o "$output/update.patch"
  curl --fail --location --proto '=https' --proto-redir '=https' \
    "https://raw.githubusercontent.com/Jalawoud/Vibepollo/$revision/cmake/packaging/bazzite.cmake" -o "$output/bazzite.cmake"
  includes=(
    --include=packaging/linux/vibepollo-display-power.h
    --include=packaging/linux/vibepollo-tray.py
    --include=src/confighttp.cpp
    --include=src/platform/linux/display_power.cpp
    --include=src/platform/linux/display_power.h
    --include=src/platform/linux/private_display.cpp
    --include=tests/CMakeLists.txt
    --include=tests/unit/test_app_close_behavior.py
    --include=tests/unit/platform/linux/test_desktop_tray.py
    --include=tests/unit/platform/linux/test_display_power.c
    --include=tests/unit/platform/linux/test_display_power_lease.cpp
    --include=tests/unit/platform/linux/test_machine_service_contract.py
  )
  if git -C "$source_dir" apply --reverse --check "${includes[@]}" "$output/update.patch" 2>/dev/null; then
    echo 'Runtime changes already applied.'
  else
    git -C "$source_dir" apply --check "${includes[@]}" "$output/update.patch"
    git -C "$source_dir" apply "${includes[@]}" "$output/update.patch"
  fi
  # This replaces only the experimental package profile, retaining all runtime
  # account, tray-path and metadata fixes already present in the cached tree.
  cp "$source_dir/cmake/packaging/bazzite.cmake" "$output/bazzite.cmake.before"
  cp "$output/bazzite.cmake" "$source_dir/cmake/packaging/bazzite.cmake"
  podman run --rm --userns=keep-id --user "$(id -u):$(id -g)" \
    --ulimit nofile=16384:16384 --env HOME=/workspace/.container-home \
    --env BUILD_VERSION=2.0.2-bazzite.local1 --env BRANCH=bazzite-port \
    --env "BUILD_JOBS=${BUILD_JOBS:-4}" \
    --volume "$source_dir:/workspace:Z" --workdir /workspace "$image" \
    bash -euo pipefail -c '
      cmake -S . -B build-local -DSUNSHINE_BUILD_BAZZITE=ON
      cmake --build build-local --parallel "$BUILD_JOBS"
      ctest --test-dir build-local --output-on-failure --no-tests=error \
        -R "^(test_component_linux_(display_power|display_power_command|display_power_lease|desktop_tray|machine_service_contract)|test_component_app_close_behavior)$"
      cpack -G RPM --config build-local/CPackConfig.cmake -B /workspace/runtime-fixed-output
      rpm -qp --scripts runtime-fixed-output/Vibepollo.rpm > runtime-fixed-output/scriptlets.txt
      test ! -s runtime-fixed-output/scriptlets.txt
      rpm -qp --provides runtime-fixed-output/Vibepollo.rpm |
        grep -Fx "libcudart-256e6409.so.12.9.79(libcudart.so.12)(64bit)"
      rpm -qp --requires runtime-fixed-output/Vibepollo.rpm | grep -Fx vibepollo-kmod
      rpm -qp --qf "%{RELEASE}\n" runtime-fixed-output/Vibepollo.rpm | grep -Fx 4.bazzite
      sha256sum runtime-fixed-output/Vibepollo.rpm
    '
) 2>&1 | tee "$log"
echo "FINISHED: $source_dir/runtime-fixed-output/Vibepollo.rpm"
echo 'Nothing installed or restarted. Keep the existing signed driver RPM installed.'
