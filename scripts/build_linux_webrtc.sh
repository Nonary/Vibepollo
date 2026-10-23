#!/bin/bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
deps_root="${VIBEPOLLO_DEPS_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/Vibepollo/deps}"
build_dir="${WEBRTC_BUILD_DIR:-$deps_root/libwebrtc/src}"
out_dir="${WEBRTC_OUT_DIR:-$deps_root/libwebrtc/out}"
webrtc_branch="${WEBRTC_BRANCH:-m125_release}"
webrtc_repo_url="${WEBRTC_REPO_URL:-https://github.com/webrtc-sdk/webrtc.git}"
depot_tools_dir="${WEBRTC_DEPOT_TOOLS_DIR:-$build_dir/depot_tools}"
git_cache_dir="${WEBRTC_GIT_CACHE_DIR:-$build_dir/git-cache}"
gclient_jobs="${WEBRTC_GCLIENT_JOBS:-16}"
wrapper_dir="$root_dir/third-party/libwebrtc"

configuration="Release"
retain_sources=0
stage="all"

function write_step() {
  echo "[webrtc] $*"
}

function fail() {
  echo "[webrtc] $*" 1>&2
  exit 1
}

function _usage() {
  local exit_code=$1

  cat <<EOF
This script builds libwebrtc for Linux and stages it where cmake/dependencies/webrtc.cmake looks for it.
It does not install host packages. Install git, python3, curl, pkg-config and a C/C++ toolchain first.

Usage:
  $0 [options]

Options:
  -h, --help               Display this help message.
  --debug                  Build the Debug configuration. Default is Release.
  --retain-sources         Keep the WebRTC source workspace after a successful build.
  --stage                  Which stage to run: sync, build, or all (default: all)

Environment:
  VIBEPOLLO_DEPS_DIR       Cache root. Default is \${XDG_CACHE_HOME:-~/.cache}/Vibepollo/deps.
  WEBRTC_BUILD_DIR         Source workspace. Default is <cache root>/libwebrtc/src.
  WEBRTC_OUT_DIR           Staged SDK. Default is <cache root>/libwebrtc/out.
  WEBRTC_BRANCH            WebRTC ref. Default is m125_release.
  WEBRTC_REPO_URL          WebRTC repository. Default is https://github.com/webrtc-sdk/webrtc.git.
  WEBRTC_DEPOT_TOOLS_DIR   depot_tools checkout. Default is <source workspace>/depot_tools.
  WEBRTC_GIT_CACHE_DIR     gclient git cache. Default is <source workspace>/git-cache.
  WEBRTC_GCLIENT_JOBS      gclient sync jobs. Default is 16.
EOF

  exit "$exit_code"
}

while getopts ":h-:" opt; do
  case ${opt} in
    h ) _usage 0 ;;
    - )
      case "${OPTARG}" in
        help) _usage 0 ;;
        debug) configuration="Debug" ;;
        retain-sources) retain_sources=1 ;;
        stage=*)
          stage="${OPTARG#*=}"
          ;;
        *)
          echo "Invalid option: --${OPTARG}" 1>&2
          _usage 1
          ;;
      esac
      ;;
    \? )
      echo "Invalid option: -${OPTARG}" 1>&2
      _usage 1
      ;;
  esac
done
shift $((OPTIND -1))

case "$stage" in
  all|sync|build) ;;
  *) fail "Invalid stage: $stage. Use sync, build, or all." ;;
esac

write_step "Using libwebrtc build dir: $build_dir"
write_step "Using libwebrtc out dir:   $out_dir"

if [[ ! -f "$wrapper_dir/BUILD.gn" ]]; then
  fail "Local libwebrtc not found: $wrapper_dir. Run: git submodule update --init third-party/libwebrtc"
fi

mkdir -p "$build_dir" "$git_cache_dir"

if [[ ! -d "$depot_tools_dir/.git" ]]; then
  write_step "Cloning depot_tools"
  git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git "$depot_tools_dir"
fi

export PATH="$depot_tools_dir:$PATH"
export DEPOT_TOOLS_UPDATE=0
export GIT_CACHE_PATH="$git_cache_dir"

cd "$build_dir"

if [[ "$stage" == "all" || "$stage" == "sync" ]]; then
  if [[ -d "src/build/.git" ]]; then
    write_step "Cleaning src/build before sync"
    git -C src/build reset --hard > /dev/null
    git -C src/build clean -fdx > /dev/null
  fi

  write_step "Writing .gclient"
  cat > .gclient <<EOF
solutions = [
  {
    "name"        : 'src',
    "url"         : '${webrtc_repo_url}@${webrtc_branch}',
    "deps_file"   : 'DEPS',
    "managed"     : False,
    "custom_deps" : {
    },
    "custom_vars": {},
  },
]
target_os  = ['linux']
EOF

  write_step "Syncing WebRTC sources"
  gclient sync --jobs "$gclient_jobs"

  if [[ "$stage" == "sync" ]]; then
    write_step "WebRTC source sync complete."
    exit 0
  fi
elif [[ ! -f "src/BUILD.gn" ]]; then
  fail "WebRTC sources not found under $build_dir. Run with --stage=sync first."
fi

dest_wrapper_dir="$build_dir/src/libwebrtc"
write_step "Copying libwebrtc wrapper into checkout"
rm -rf "$dest_wrapper_dir"
cp -r "$wrapper_dir" "$dest_wrapper_dir"
rm -rf "$dest_wrapper_dir/.git"

if ! grep -q "//libwebrtc" src/BUILD.gn; then
  write_step "Patching BUILD.gn to include libwebrtc"
  sed -i 's|deps = \[ ":webrtc" \]|deps = [ ":webrtc", "//libwebrtc" ]|' src/BUILD.gn
  grep -q "//libwebrtc" src/BUILD.gn || fail "Unable to patch src/BUILD.gn"
fi

is_debug="false"
if [[ "$configuration" == "Debug" ]]; then
  is_debug="true"
fi

# Sunshine feeds its own frames, so skip the wrapper's capturer and its gio/PipeWire dependencies.
gn_args=(
  'target_os="linux"'
  'target_cpu="x64"'
  "is_debug=$is_debug"
  'is_component_build=false'
  'rtc_use_h264=true'
  'rtc_use_h265=true'
  'ffmpeg_branding="Chrome"'
  'rtc_include_tests=false'
  'rtc_build_examples=false'
  'rtc_enable_protobuf=false'
  'use_rtti=true'
  'libwebrtc_desktop_capture=false'
  'is_clang=true'
  'clang_use_chrome_plugins=false'
  'treat_warnings_as_errors=false'
)

# depot_tools' gn and ninja wrappers need a bootstrap that DEPOT_TOOLS_UPDATE=0 skips.
gn_bin="$build_dir/src/buildtools/linux64/gn"
ninja_bin="$build_dir/src/third_party/ninja/ninja"
if [[ ! -x "$gn_bin" ]]; then
  fail "gn not found at $gn_bin. Run with --stage=sync first."
fi
if [[ ! -x "$ninja_bin" ]]; then
  ninja_bin="$(command -v ninja)" || fail "ninja not found in PATH."
fi

gn_out_dir="$build_dir/src/out/linux"
write_step "Generating GN build files"
(cd src && "$gn_bin" gen "$gn_out_dir" --args="${gn_args[*]}")

write_step "Building libwebrtc"
"$ninja_bin" -C "$gn_out_dir" libwebrtc

so_file="$gn_out_dir/libwebrtc.so"
if [[ ! -f "$so_file" ]]; then
  fail "libwebrtc.so not found at $so_file"
fi

mkdir -p "$out_dir/include" "$out_dir/lib"

write_step "Copying headers"
cp -rf "$dest_wrapper_dir/include/." "$out_dir/include/"

write_step "Copying shared library"
install -m 755 "$so_file" "$out_dir/lib/libwebrtc.so"

write_step "Staged reusable WebRTC SDK. Set WEBRTC_ROOT to $out_dir"

if [[ "$retain_sources" -eq 1 ]]; then
  write_step "Retaining WebRTC source workspace: $build_dir"
else
  case "$(realpath "$build_dir")" in
    "/"|"$(realpath "$HOME")"|"$(realpath "$root_dir")")
      fail "Refusing to remove $build_dir"
      ;;
  esac
  write_step "Removing WebRTC source workspace: $build_dir"
  cd "$root_dir"
  rm -rf "$build_dir"
fi

write_step "Done."
