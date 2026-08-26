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
mkdir -p -- "$CONFIGFS_TEST_ROOT/vibeshine-drm"
configure_paths "$CONFIGFS_TEST_ROOT"
# The sourced helper functions consume this test-only switch.
# shellcheck disable=SC2034
FAKE_CONFIGFS=1

ensure_display_backend || fail "preferred backend selection failed"
[[ "$ACTIVE_BACKEND" == "$PREFERRED_BACKEND" ]] || fail "preferred HDR backend was not selected"
create_pool || fail "initial pool creation failed"
assert_file_value "$VKMS_DEVICE_DIR/enabled" 1

for ((pipeline = 0; pipeline < VKMS_OUTPUT_COUNT; ++pipeline)); do
  plane="$VKMS_DEVICE_DIR/planes/plane${pipeline}"
  crtc="$VKMS_DEVICE_DIR/crtcs/crtc${pipeline}"
  encoder="$VKMS_DEVICE_DIR/encoders/encoder${pipeline}"
  connector="$VKMS_DEVICE_DIR/connectors/Virtual-$((pipeline + 1))"

  assert_file_value "$plane/type" 1
  assert_file_value "$crtc/writeback" 0
  assert_file_value "$connector/status" "$CONNECTOR_STATUS_DISCONNECTED"
  assert_link_target "$plane/possible_crtcs/crtc${pipeline}" "$crtc"
  assert_link_target "$encoder/possible_crtcs/crtc${pipeline}" "$crtc"
  assert_link_target "$connector/possible_encoders/encoder${pipeline}" "$encoder"
done

# The socket broker may connect and disconnect one provisioned output without
# rebuilding the DRM device or disturbing the other dormant connectors.
configfs_is_mounted() {
  return 0
}
response=$(printf 'connect Virtual-2\n' | control_connection) || fail "connect request failed"
[[ "$response" == "OK connected Virtual-2" ]] || fail "unexpected connect response: ${response}"
assert_file_value "$VKMS_DEVICE_DIR/connectors/Virtual-2/status" "$CONNECTOR_STATUS_CONNECTED"
response=$(printf 'status Virtual-2\n' | control_connection) || fail "status request failed"
[[ "$response" == "STATUS connected Virtual-2" ]] || fail "unexpected status response: ${response}"
validate_topology || fail "live connected topology was rejected"

# A second start must retain the exact four-pipeline object graph and must not
# reset a live connector back to dormant.
create_pool || fail "idempotent pool creation failed"
assert_file_value "$VKMS_DEVICE_DIR/connectors/Virtual-2/status" "$CONNECTOR_STATUS_CONNECTED"
for group in planes crtcs encoders connectors; do
  count=0
  for entry in "$VKMS_DEVICE_DIR/$group"/*; do
    [[ -e "$entry" || -L "$entry" ]] || continue
    ((count += 1))
  done
  [[ $count -eq $VKMS_OUTPUT_COUNT ]] || fail "${group}: expected 4 items after restart, found ${count}"
done

response=$(printf 'disconnect Virtual-2\n' | control_connection) || fail "disconnect request failed"
[[ "$response" == "OK disconnected Virtual-2" ]] || fail "unexpected disconnect response: ${response}"
assert_file_value "$VKMS_DEVICE_DIR/connectors/Virtual-2/status" "$CONNECTOR_STATUS_DISCONNECTED"
response=$(printf 'status Virtual-2\n' | control_connection) || fail "disconnected status request failed"
[[ "$response" == "STATUS disconnected Virtual-2" ]] || fail "unexpected disconnected status: ${response}"

for invalid_request in 'connect Virtual-0' 'connect Virtual-5' 'connect Virtual-1 extra' 'CONNECT Virtual-1'; do
  if response=$(printf '%s\n' "$invalid_request" | control_connection); then
    fail "invalid control request succeeded: ${invalid_request}"
  fi
  [[ "$response" == "ERROR malformed request" ]] || fail "unexpected protocol error: ${response}"
done

# Control must never follow a connector attribute symlink in the test model.
mv -- "$VKMS_DEVICE_DIR/connectors/Virtual-3/status" "$TEST_ROOT/Virtual-3.status"
printf 'outside' >"$TEST_ROOT/outside-status"
ln -s -- "$TEST_ROOT/outside-status" "$VKMS_DEVICE_DIR/connectors/Virtual-3/status"
if response=$(printf 'connect Virtual-3\n' | control_connection); then
  fail "control followed a symbolic-link status attribute"
fi
[[ "$response" == "ERROR unsafe connector path" ]] || fail "unexpected unsafe-path response: ${response}"
assert_file_value "$TEST_ROOT/outside-status" outside
unlink -- "$VKMS_DEVICE_DIR/connectors/Virtual-3/status"
mv -- "$TEST_ROOT/Virtual-3.status" "$VKMS_DEVICE_DIR/connectors/Virtual-3/status"

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

# If the HDR module is unavailable, upstream configfs VKMS is selected and
# remains fully functional as an explicitly logged SDR fallback.
rmdir -- "$PREFERRED_CONFIGFS_ROOT"
mkdir -- "$FALLBACK_CONFIGFS_ROOT"
# ShellCheck cannot see these command mocks being invoked by the sourced helper.
# shellcheck disable=SC2329
modprobe() {
  [[ ${1:-} == "$FALLBACK_MODULE" ]]
}
ensure_display_backend || fail "upstream VKMS fallback selection failed"
[[ "$ACTIVE_BACKEND" == "$FALLBACK_BACKEND" ]] || fail "SDR fallback backend was not selected"
create_pool || fail "fallback pool creation failed"
assert_file_value "$VKMS_DEVICE_DIR/enabled" 1
response=$(printf 'connect Virtual-1\n' | control_connection) || fail "fallback connect request failed"
[[ "$response" == "OK connected Virtual-1" ]] || fail "unexpected fallback response: ${response}"
response=$(printf 'disconnect Virtual-1\n' | control_connection) || fail "fallback disconnect request failed"
[[ "$response" == "OK disconnected Virtual-1" ]] || fail "unexpected fallback disconnect response: ${response}"

# When the HDR backend later appears, its pool takes precedence and the stale
# managed fallback pool is removed before provisioning continues.
mkdir -- "$PREFERRED_CONFIGFS_ROOT"
select_backend "$PREFERRED_BACKEND"
create_pool || fail "preferred pool creation after fallback failed"
remove_inactive_backend_pool || fail "stale fallback cleanup failed"
[[ ! -e "$FALLBACK_CONFIGFS_ROOT/$VKMS_DEVICE_NAME" ]] || fail "stale fallback pool remains"
[[ -d "$PREFERRED_CONFIGFS_ROOT/$VKMS_DEVICE_NAME" ]] || fail "preferred pool was removed"
remove_all_backend_pools || fail "all-backend cleanup failed"

# Unsupported systems fail without creating a pool in either subsystem.
rmdir -- "$PREFERRED_CONFIGFS_ROOT" "$FALLBACK_CONFIGFS_ROOT"
# shellcheck disable=SC2329
modprobe() {
  return 1
}
if ensure_display_backend; then
  fail "backend detection succeeded without either configfs subsystem"
fi
[[ ! -e "$PREFERRED_CONFIGFS_ROOT/$VKMS_DEVICE_NAME" ]] || fail "preferred pool appeared on failure"
[[ ! -e "$FALLBACK_CONFIGFS_ROOT/$VKMS_DEVICE_NAME" ]] || fail "fallback pool appeared on failure"

printf 'PASS: vibeshine-vkms shell tests\n'
