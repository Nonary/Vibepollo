#!/usr/bin/env bash
# Build the current committed checkout locally, never a hosted workflow.
set -euo pipefail
(( EUID != 0 )) || { echo 'Run without sudo.' >&2; exit 1; }
source=$(git -C "$(dirname -- "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)
state="${XDG_DATA_HOME:-$HOME/.local/share}/vibepollo-bazzite"
mkdir -p "$state/logs"
exec 9>"$state/build.lock"
flock -n 9 || { echo 'Another build is running.' >&2; exit 1; }
jobs=${BUILD_JOBS:-4}
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || exit 2
git -C "$source" diff --quiet HEAD || { echo 'Commit source changes before building.' >&2; exit 1; }
git -C "$source" submodule update --init --recursive
image=localhost/vibepollo-bazzite-builder:f44-gcc14-v1
if ! podman image exists "$image"; then
    context=$(mktemp -d "$state/builder.XXXXXX")
    trap 'rm -rf -- "$context"' EXIT
    mkdir -p "$context/scripts" "$context/packaging/linux"
    cp "$source/packaging/linux/bazzite/Containerfile" "$context/Containerfile"
    cp "$source/scripts/linux_build.sh" "$context/scripts/"
    cp -a "$source/packaging/linux/patches" "$context/packaging/linux/"
    podman build --layers --build-arg "BUILD_JOBS=$jobs" -t "$image" "$context"
fi
log="$state/logs/local-$(date +%Y%m%d-%H%M%S).log"
podman run --rm --userns=keep-id --user "$(id -u):$(id -g)" \
    --ulimit nofile=16384:16384 \
    --env "BUILD_JOBS=$jobs" --env HOME=/workspace/.container-home \
    --env BUILD_VERSION=2.0.2-bazzite.local1 --env BRANCH=bazzite-port \
    --volume "$source:/workspace:Z" --workdir /workspace "$image" \
    bash packaging/linux/bazzite/run-in-container.sh 2>&1 | tee "$log"
echo "Build log: $log"
