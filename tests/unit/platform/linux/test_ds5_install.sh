#!/usr/bin/env bash
# Exercise installation outcomes without touching real modules or services.
set -euo pipefail
source "$1"
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
SYS_MODULE_ROOT="$work/sys"
SOURCE_DIR="$work/source"
mkdir -p "$SYS_MODULE_ROOT/$MODULE_NAME" "$SOURCE_DIR"
printf 'NEW\n' > "$SYS_MODULE_ROOT/$MODULE_NAME/srcversion"
modinfo() { printf 'NEW\n'; }
modprobe() { return "${load_result:-0}"; }
current_kernel_release() { printf 'test-kernel\n'; }
dkms() { :; }
install_with_dkms() { return "${dkms_result:-0}"; }
install_direct() { touch "$work/direct"; return "${direct_result:-0}"; }

expect_status() {
  local expected=$1 result=0
  shift
  "$@" || result=$?
  [[ $result == "$expected" ]] || { echo "expected $expected, got $result: $*" >&2; exit 1; }
}

# Missing package sources must fail before any install attempt.
expect_status 1 install_module
for file in Makefile build-module dkms.conf vibeshine_ds5_uapi.h \
            vibeshine_ds5.h vibeshine_ds5_udc.c vibeshine_ds5_gadget.c \
            vibeshine_ds5_main.c; do touch "$SOURCE_DIR/$file"; done
expect_status 1 validate_source_tree
chmod 755 "$SOURCE_DIR/build-module"
expect_status 0 install_module
[[ ! -e "$work/direct" ]]

# Loading failure must not be hidden behind a successful DKMS build.
load_result=1
expect_status 1 install_module
load_result=0
printf 'OLD\n' > "$SYS_MODULE_ROOT/$MODULE_NAME/srcversion"
expect_status 4 install_module
expect_status 4 module_status
printf 'NEW\n' > "$SYS_MODULE_ROOT/$MODULE_NAME/srcversion"
expect_status 0 module_status

# DKMS failure falls back to a direct build, whose errors also propagate.
dkms_result=1
direct_result=1
expect_status 1 install_module
[[ -f "$work/direct" ]]
direct_result=0
expect_status 0 install_module
load_result=1
expect_status 1 install_module
rm "$SYS_MODULE_ROOT/$MODULE_NAME/srcversion"
expect_status 1 module_status
printf 'DualSense install: missing sources, DKMS/direct failures, load failures, and stale modules covered.\n'
