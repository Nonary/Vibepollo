#!/usr/bin/env bash

# ShellCheck cannot see variables and command mocks consumed by the sourced helper.
# shellcheck disable=SC2034,SC2329

set -euo pipefail

INSTALLER_UNDER_TEST=${1:?usage: test-vibeshine-drm-install.sh /path/to/vibeshine-drm-install}
TEST_ROOT=$(mktemp -d)

cleanup_test_root() {
  rm -rf -- "$TEST_ROOT"
}
trap cleanup_test_root EXIT

# shellcheck source=/dev/null
source "$INSTALLER_UNDER_TEST"

# The source template is tested before CMake substitutes its deterministic hash.
MODULE_VERSION=1.2.3
MODULE_SOURCE_ID=$(printf 'a%.0s' {1..64})

fail() {
  printf 'FAIL: %s\n' "$*" >&2
  exit 1
}

assert_contains() {
  local path=$1
  local expected=$2

  [[ -f "$path" ]] || fail "missing file: ${path}"
  if ! grep -F -- "$expected" "$path" >/dev/null; then
    fail "${path} does not contain '${expected}'"
  fi
}

TEST_SOURCE="$TEST_ROOT/vibeshine-drm-$MODULE_VERSION"
TEST_STATE="$TEST_ROOT/state"
TEST_MODULES="$TEST_ROOT/modules"
TEST_SYS_MODULE="$TEST_ROOT/sys-module"
TEST_BUILD_TMP="$TEST_ROOT/build-tmp"
TEST_KERNEL="6.99.1-vibeshine"
mkdir -p -- "$TEST_SOURCE" "$TEST_MODULES/$TEST_KERNEL/build" "$TEST_SYS_MODULE" "$TEST_BUILD_TMP"
printf 'obj-m += vibeshine_drm.o\n' >"$TEST_SOURCE/Makefile"
printf 'PACKAGE_NAME="vibeshine-drm"\n' >"$TEST_SOURCE/dkms.conf"
printf '/* test source */\n' >"$TEST_SOURCE/vkms_drv.c"
printf 'all:\n' >"$TEST_MODULES/$TEST_KERNEL/build/Makefile"
cp -- "$(dirname -- "$INSTALLER_UNDER_TEST")/vibeshine-drm/build-module" "$TEST_SOURCE/build-module"
chmod 0755 "$TEST_SOURCE/build-module"

configure_install_paths "$TEST_SOURCE" "$TEST_STATE" "$TEST_MODULES" "$TEST_SYS_MODULE" "$TEST_BUILD_TMP"
KERNEL_RELEASE_OVERRIDE=$TEST_KERNEL

test_dkms_install() (
  DKMS_REGISTERED=0
  DKMS_INSTALLED=0
  DKMS_OLD_REGISTERED=1
  DKMS_CALLS="$TEST_ROOT/dkms-calls"
  : >"$DKMS_CALLS"

  dkms() {
    local action=${1:-}
    printf '%s\n' "$*" >>"$DKMS_CALLS"
    case "$action" in
      status)
        if ((DKMS_OLD_REGISTERED)); then
          printf '%s/%s, %s, x86_64: installed\n' "$DKMS_NAME" 0.9.0 "$TEST_KERNEL"
        fi
        if ((DKMS_REGISTERED)); then
          if ((DKMS_INSTALLED)); then
            printf '%s/%s, %s, x86_64: installed\n' "$DKMS_NAME" "$MODULE_VERSION" "$TEST_KERNEL"
          else
            printf '%s/%s: added\n' "$DKMS_NAME" "$MODULE_VERSION"
          fi
        fi
        ;;
      add)
        DKMS_REGISTERED=1
        ;;
      build)
        ((DKMS_REGISTERED == 1))
        ;;
      install)
        DKMS_INSTALLED=1
        ;;
      remove)
        if [[ $* == *'-v 0.9.0'* ]]; then
          DKMS_OLD_REGISTERED=0
        else
          DKMS_REGISTERED=0
          DKMS_INSTALLED=0
        fi
        ;;
      *)
        return 1
        ;;
    esac
  }

  mkdir -- "$TEST_ROOT/vibeshine-drm-0.9.0"
  install_module || fail "DKMS installation failed"
  [[ ! -e "$TEST_ROOT/vibeshine-drm-0.9.0" ]] || fail "obsolete source tree remains"
  assert_contains "$DKMS_CALLS" "remove -m $DKMS_NAME -v 0.9.0 --all"
  install_module || fail "idempotent DKMS installation failed"
  [[ $(grep -c '^install ' "$DKMS_CALLS") -eq 1 ]] || fail "DKMS installed more than once"

  MODULE_SOURCE_ID=$(printf 'b%.0s' {1..64})
  install_module || fail "same-version DKMS refresh failed"
  [[ $(grep -c '^install ' "$DKMS_CALLS") -eq 2 ]] || fail "changed DKMS source was not reinstalled"
  assert_contains "$DKMS_CALLS" "remove -m $DKMS_NAME -v $MODULE_VERSION -k $TEST_KERNEL"
  assert_contains "$DKMS_CALLS" "add -m $DKMS_NAME -v $MODULE_VERSION"
  assert_contains "$DKMS_CALLS" "build -m $DKMS_NAME -v $MODULE_VERSION -k $TEST_KERNEL"
  remove_module || fail "DKMS removal failed"
  assert_contains "$DKMS_CALLS" "remove -m $DKMS_NAME -v $MODULE_VERSION --all"
)

test_direct_install() (
  DIRECT_CALLS="$TEST_ROOT/direct-calls"
  : >"$DIRECT_CALLS"

  dkms_available() {
    return 1
  }
  run_build_helper() {
    local build_workdir=$1
    local kernel_release=$2
    printf 'build-helper %s %s\n' "$kernel_release" "$build_workdir" >>"$DIRECT_CALLS"
    printf 'fake module\n' >"$build_workdir/$MODULE_NAME.ko"
  }
  depmod() {
    printf 'depmod %s\n' "$*" >>"$DIRECT_CALLS"
  }
  modprobe() {
    printf 'modprobe %s\n' "$*" >>"$DIRECT_CALLS"
  }

  install_module || fail "direct installation failed"
  destination=$(direct_destination "$TEST_KERNEL")
  marker=$(direct_marker "$TEST_KERNEL")
  [[ -f "$destination" ]] || fail "direct module was not installed"
  [[ -f "$marker" ]] || fail "direct-install marker was not created"
  [[ ! -e "$SOURCE_DIR/$MODULE_NAME.ko" ]] || fail "direct build polluted the packaged source tree"
  install_module || fail "idempotent direct installation failed"
  [[ $(grep -c '^build-helper ' "$DIRECT_CALLS") -eq 1 ]] || fail "direct module built more than once"

  MODULE_SOURCE_ID=$(printf 'c%.0s' {1..64})
  install_module || fail "same-version direct refresh failed"
  [[ $(grep -c '^build-helper ' "$DIRECT_CALLS") -eq 2 ]] || fail "changed direct source was not rebuilt"
  if compgen -G "$BUILD_TMP_ROOT/vibeshine-drm-build.*" >/dev/null; then
    fail "temporary direct-build directory was not cleaned"
  fi

  mkdir -- "$SYS_MODULE_ROOT/$MODULE_NAME"
  remove_module || fail "direct module removal failed"
  [[ ! -e "$destination" ]] || fail "direct module remains after removal"
  [[ ! -e "$marker" ]] || fail "direct-install marker remains after removal"
  assert_contains "$DIRECT_CALLS" "modprobe -r $MODULE_NAME"
  assert_contains "$DIRECT_CALLS" "depmod -a $TEST_KERNEL"
)

test_obsolete_direct_cleanup() (
  OLD_KERNEL="6.98.0-old"
  OLD_SOURCE="$TEST_ROOT/vibeshine-drm-0.8.0"
  UNMANAGED_SOURCE="$TEST_ROOT/vibeshine-drm-backup"
  OLD_DESTINATION="$TEST_MODULES/$OLD_KERNEL/updates/vibeshine/$MODULE_NAME.ko"
  OLD_MARKER="$TEST_STATE/direct-0.8.0-$OLD_KERNEL"
  mkdir -p -- "$OLD_SOURCE" "$UNMANAGED_SOURCE" "${OLD_DESTINATION%/*}" "$TEST_STATE"
  printf 'old module\n' >"$OLD_DESTINATION"
  # Legacy markers contained only the installed destination.
  printf '%s\n' "$OLD_DESTINATION" >"$OLD_MARKER"

  dkms_available() {
    return 1
  }
  depmod() {
    :
  }

  cleanup_obsolete_installations || fail "obsolete direct cleanup failed"
  [[ ! -e "$OLD_DESTINATION" ]] || fail "obsolete direct module remains"
  [[ ! -e "$OLD_MARKER" ]] || fail "obsolete direct marker remains"
  [[ ! -e "$OLD_SOURCE" ]] || fail "obsolete direct source tree remains"
  [[ -d "$UNMANAGED_SOURCE" ]] || fail "non-versioned source directory was removed"
)

test_dkms_failure_falls_back() (
  FALLBACK_CALLS="$TEST_ROOT/fallback-calls"
  : >"$FALLBACK_CALLS"

  dkms_available() {
    return 0
  }
  dkms() {
    case ${1:-} in
      status)
        return 0
        ;;
      add)
        return 1
        ;;
    esac
    return 1
  }
  run_build_helper() {
    local build_workdir=$1
    local kernel_release=$2
    printf 'build-helper %s %s\n' "$kernel_release" "$build_workdir" >>"$FALLBACK_CALLS"
    printf 'fake module\n' >"$build_workdir/$MODULE_NAME.ko"
  }
  depmod() {
    :
  }

  install_module || fail "direct fallback after DKMS failure failed"
  [[ -f "$(direct_destination "$TEST_KERNEL")" ]] || fail "DKMS failure did not install direct module"
)

test_dkms_install
test_direct_install
test_obsolete_direct_cleanup

# The direct fallback test uses its own state tree so an earlier direct marker
# cannot turn the fallback path into an idempotent no-op.
configure_install_paths \
  "$TEST_SOURCE" "$TEST_ROOT/fallback-state" "$TEST_MODULES" "$TEST_SYS_MODULE" "$TEST_BUILD_TMP"
test_dkms_failure_falls_back

printf 'PASS: vibeshine-drm installer shell tests\n'
