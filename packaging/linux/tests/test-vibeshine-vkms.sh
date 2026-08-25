#!/usr/bin/env bash

set -euo pipefail

SCRIPT_UNDER_TEST=${1:?usage: test-vibeshine-vkms.sh /path/to/vibeshine-vkms}
TEST_ROOT=$(mktemp -d)

cleanup_test_root() {
  rm -rf -- "$TEST_ROOT"
}
trap cleanup_test_root EXIT

# shellcheck source=/dev/null
source "$SCRIPT_UNDER_TEST"

fail() {
  printf 'FAIL: %s\n' "$*" >&2
  exit 1
}

assert_file_value() {
  local path=$1
  local expected=$2
  local actual

  [[ -f "$path" ]] || fail "missing file: ${path}"
  actual=$(<"$path")
  [[ "$actual" == "$expected" ]] || fail "${path}: expected '${expected}', found '${actual}'"
}

assert_link_target() {
  local path=$1
  local expected=$2
  local actual

  [[ -L "$path" ]] || fail "missing symbolic link: ${path}"
  actual=$(readlink -f -- "$path") || fail "could not resolve ${path}"
  [[ "$actual" == "$expected" ]] || fail "${path}: expected target ${expected}, found ${actual}"
}

CONFIGFS_TEST_ROOT="$TEST_ROOT/configfs"
mkdir -p -- "$CONFIGFS_TEST_ROOT/vkms"
configure_paths "$CONFIGFS_TEST_ROOT"
# The sourced helper functions consume this test-only switch.
# shellcheck disable=SC2034
FAKE_CONFIGFS=1

create_pool || fail "initial pool creation failed"
assert_file_value "$VKMS_DEVICE_DIR/enabled" 1

for ((pipeline = 0; pipeline < VKMS_OUTPUT_COUNT; ++pipeline)); do
  plane="$VKMS_DEVICE_DIR/planes/plane${pipeline}"
  crtc="$VKMS_DEVICE_DIR/crtcs/crtc${pipeline}"
  encoder="$VKMS_DEVICE_DIR/encoders/encoder${pipeline}"
  connector="$VKMS_DEVICE_DIR/connectors/Virtual-$((pipeline + 1))"

  assert_file_value "$plane/type" 1
  assert_file_value "$crtc/writeback" 0
  assert_file_value "$connector/status" 1
  assert_link_target "$plane/possible_crtcs/crtc${pipeline}" "$crtc"
  assert_link_target "$encoder/possible_crtcs/crtc${pipeline}" "$crtc"
  assert_link_target "$connector/possible_encoders/encoder${pipeline}" "$encoder"
done

# A second start must retain the exact four-pipeline object graph.
create_pool || fail "idempotent pool creation failed"
for group in planes crtcs encoders connectors; do
  count=0
  for entry in "$VKMS_DEVICE_DIR/$group"/*; do
    [[ -e "$entry" || -L "$entry" ]] || continue
    ((count += 1))
  done
  [[ $count -eq $VKMS_OUTPUT_COUNT ]] || fail "${group}: expected 4 items after restart, found ${count}"
done

# Cleanup must refuse to touch an instance containing paths it does not own.
mkdir -- "$VKMS_DEVICE_DIR/connectors/unmanaged"
if remove_pool; then
  fail "cleanup accepted an unmanaged connector"
fi
assert_file_value "$VKMS_DEVICE_DIR/enabled" 1
[[ -d "$VKMS_DEVICE_DIR/connectors/Virtual-1" ]] || fail "safe cleanup changed a managed connector"
rmdir -- "$VKMS_DEVICE_DIR/connectors/unmanaged"

# A managed-looking name must not be allowed to redirect cleanup elsewhere.
mv -- "$VKMS_DEVICE_DIR/connectors/Virtual-1" "$TEST_ROOT/Virtual-1.saved"
mkdir -- "$TEST_ROOT/outside"
printf 'keep' >"$TEST_ROOT/outside/sentinel"
ln -s -- "$TEST_ROOT/outside" "$VKMS_DEVICE_DIR/connectors/Virtual-1"
if remove_pool; then
  fail "cleanup followed a managed-name symbolic link"
fi
assert_file_value "$TEST_ROOT/outside/sentinel" keep
unlink -- "$VKMS_DEVICE_DIR/connectors/Virtual-1"
mv -- "$TEST_ROOT/Virtual-1.saved" "$VKMS_DEVICE_DIR/connectors/Virtual-1"

remove_pool || fail "pool removal failed"
[[ ! -e "$VKMS_DEVICE_DIR" ]] || fail "managed instance remains after removal"
remove_pool || fail "idempotent pool removal failed"

printf 'PASS: vibeshine-vkms shell tests\n'
