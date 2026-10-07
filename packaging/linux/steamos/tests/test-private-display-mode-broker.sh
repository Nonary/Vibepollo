#!/usr/bin/env bash
set -euo pipefail

repository=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../../.." && pwd -P)
fixture=$(mktemp -d)
trap 'rm -rf -- "$fixture"' EXIT
mkdir -p "$fixture/libexec" "$fixture/config/vibeshine-drm/vibeshine/connectors/Virtual-1" "$fixture/leases"
cp "$repository/third-party/libvirtualdisplay/linux/packaging/vibeshine-vkms" "$fixture/libexec/vibeshine-vkms"
cp "$repository/packaging/linux/steamos/local/private-display-mode-broker" "$fixture/libexec/private-display-mode-broker"
source "$fixture/libexec/private-display-mode-broker"
configure_paths "$fixture/config"
configure_lease_paths "$fixture/leases"
FAKE_CONFIGFS=1
configfs_is_mounted() { return 0; }
printf '1\n' >"$VKMS_DEVICE_DIR/enabled"
printf '1\n' >"$VKMS_DEVICE_DIR/connectors/Virtual-1/status"
printf '0 0 0\n' >"$VKMS_DEVICE_DIR/connectors/Virtual-1/requested_mode"
printf '1000\n' >"$LEASE_ROOT/Virtual-1.owner"

expect_ok() {
  local request=$1 expected=$2 response
  response=$(control_connection 1000 <<<"$request")
  [[ "$response" == "$expected" ]] || { printf 'unexpected response: %s\n' "$response" >&2; exit 1; }
}
expect_error() {
  local uid=$1 request=$2 before response
  before=$(<"$VKMS_DEVICE_DIR/connectors/Virtual-1/requested_mode")
  if response=$(control_connection "$uid" <<<"$request"); then
    printf 'unexpected acceptance: %s\n' "$request" >&2
    exit 1
  fi
  [[ "$response" == ERROR* ]]
  [[ $(<"$VKMS_DEVICE_DIR/connectors/Virtual-1/requested_mode") == "$before" ]]
}

expect_ok 'mode Virtual-1 3024 1890 120000' 'OK mode Virtual-1 3024 1890 120000'
[[ $(<"$VKMS_DEVICE_DIR/connectors/Virtual-1/requested_mode") == '3024 1890 120000' ]]
expect_ok 'mode Virtual-1 3033 1891 119880' 'OK mode Virtual-1 3033 1891 119880'
expect_ok 'mode Virtual-1 1920 1080 59940' 'OK mode Virtual-1 1920 1080 59940'
[[ $(<"$VKMS_DEVICE_DIR/connectors/Virtual-1/status") == 1 ]]
expect_error 1001 'mode Virtual-1 3024 1890 120000'
expect_error 0 'mode Virtual-1 3024 1890 120000'
for request in \
  'mode Virtual-1 63 1890 120000' 'mode Virtual-1 3024 8193 120000' \
  'mode Virtual-1 3024 1890 999' 'mode Virtual-1 3024 1890 1000001' \
  'mode Virtual-1 -3024 1890 120000' 'mode Virtual-1 03024 1890 120000' \
  'mode Virtual-1 3024 1890 120000;id' 'mode ../../card0 3024 1890 120000'; do
  expect_error 1000 "$request"
done
printf '2\n' >"$VKMS_DEVICE_DIR/connectors/Virtual-1/status"
expect_error 1000 'mode Virtual-1 3024 1890 120000'
printf '1\n' >"$VKMS_DEVICE_DIR/connectors/Virtual-1/status"
rm "$LEASE_ROOT/Virtual-1.owner"
expect_error 1000 'mode Virtual-1 3024 1890 120000'
printf '1000\n' >"$LEASE_ROOT/Virtual-1.owner"
cp "$VKMS_DEVICE_DIR/connectors/Virtual-1/requested_mode" "$fixture/unchanged"
rm "$VKMS_DEVICE_DIR/connectors/Virtual-1/requested_mode"
ln -s "$fixture/unchanged" "$VKMS_DEVICE_DIR/connectors/Virtual-1/requested_mode"
expect_error 1000 'mode Virtual-1 3024 1890 120000'
expect_ok 'status Virtual-1' 'STATUS connected Virtual-1'
printf 'Private-display mode broker tests passed.\n'
