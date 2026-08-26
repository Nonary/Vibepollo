#!/usr/bin/env bash

set -euo pipefail

BUILD_HELPER_UNDER_TEST=${1:?usage: test-vibeshine-drm-build-module.sh /path/to/build-module}
TEST_ROOT=$(mktemp -d)

cleanup_test_root() {
  rm -rf -- "$TEST_ROOT"
}
trap cleanup_test_root EXIT

# shellcheck source=/dev/null
source "$BUILD_HELPER_UNDER_TEST"

fail() {
  printf 'FAIL: %s\n' "$*" >&2
  exit 1
}

TEST_MODULES="$TEST_ROOT/modules"
TEST_SOURCE="$TEST_ROOT/source"
CLANG_KERNEL="6.99.1-clang"
GCC_KERNEL="6.99.1-gcc"
MAKE_CALLS="$TEST_ROOT/make-calls"
mkdir -p -- "$TEST_SOURCE" "$TEST_MODULES/$CLANG_KERNEL/build" "$TEST_MODULES/$GCC_KERNEL/build"
printf 'obj-m += vibeshine_drm.o\n' >"$TEST_SOURCE/Makefile"
printf 'all:\n' >"$TEST_MODULES/$CLANG_KERNEL/build/Makefile"
printf 'CONFIG_CC_IS_CLANG=y\n' >"$TEST_MODULES/$CLANG_KERNEL/build/.config"
printf 'all:\n' >"$TEST_MODULES/$GCC_KERNEL/build/Makefile"
printf 'CONFIG_CC_IS_GCC=y\n' >"$TEST_MODULES/$GCC_KERNEL/build/.config"
: >"$MAKE_CALLS"

make() {
  printf '%s\n' "$*" >>"$MAKE_CALLS"
}

configure_build_paths "$TEST_MODULES"
build_module "$CLANG_KERNEL" "$TEST_SOURCE" || fail "Clang kernel build dispatch failed"
build_module "$GCC_KERNEL" "$TEST_SOURCE" || fail "GCC kernel build dispatch failed"

clang_call=$(sed -n '1p' "$MAKE_CALLS")
gcc_call=$(sed -n '2p' "$MAKE_CALLS")
[[ "$clang_call" == *"M=$TEST_SOURCE"* ]] || fail "Clang build omitted the module source path"
[[ "$clang_call" == *"LLVM=1"* ]] || fail "Clang build omitted LLVM=1"
[[ "$clang_call" == *"modules" ]] || fail "Clang build omitted the modules target"
[[ "$gcc_call" == *"M=$TEST_SOURCE"* ]] || fail "GCC build omitted the module source path"
[[ "$gcc_call" != *"LLVM=1"* ]] || fail "GCC build incorrectly enabled LLVM mode"
[[ "$gcc_call" == *"modules" ]] || fail "GCC build omitted the modules target"

if build_module '../unsafe' "$TEST_SOURCE"; then
  fail "unsafe kernel release was accepted"
fi

printf 'PASS: vibeshine-drm build-module shell tests\n'
