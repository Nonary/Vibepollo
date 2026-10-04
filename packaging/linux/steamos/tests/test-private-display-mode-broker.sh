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
printf 'Virtual-1\n' >"$VKMS_DEVICE_DIR/connectors/Virtual-1/drm_name"
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
rm "$VKMS_DEVICE_DIR/connectors/Virtual-1/requested_mode"
mv "$fixture/unchanged" "$VKMS_DEVICE_DIR/connectors/Virtual-1/requested_mode"
expect_ok 'status Virtual-1' 'STATUS connected Virtual-1'
for connector in Virtual-0 Virtual-01 Virtual-2147483648 Virtual-5x; do
  for verb in connect disconnect status; do
    expect_error 1000 "$verb $connector"
  done
  expect_error 1000 "mode $connector 3024 1890 120000"
done

# Public DRM connector IDs can be offset and noncontiguous. The SteamOS entry
# point must inherit the canonical resolver and keep mutation/leases bounded to
# the corresponding logical configfs slot while preserving the public reply.
for slot in 2 3 4; do
  connector="Virtual-$slot"
  mkdir -p "$VKMS_DEVICE_DIR/connectors/$connector"
  printf '2\n' >"$VKMS_DEVICE_DIR/connectors/$connector/status"
  printf '0 0 0\n' >"$VKMS_DEVICE_DIR/connectors/$connector/requested_mode"
done
for ((slot = 1; slot <= VKMS_OUTPUT_COUNT; ++slot)); do
  public_id=$((slot + 2))
  if ((slot == 1)); then
    public_id=2
  fi
  printf 'Virtual-%s\n' "$public_id" >"$VKMS_DEVICE_DIR/connectors/Virtual-${slot}/drm_name"
done

printf 'Virtual-10\n' >"$VKMS_DEVICE_DIR/connectors/Virtual-4/drm_name"

expect_ok 'connect Virtual-2' 'OK connected Virtual-2'
[[ $(<"$VKMS_DEVICE_DIR/connectors/Virtual-1/status") == 1 ]]
[[ $(<"$LEASE_ROOT/Virtual-1.owner") == 1000 ]]
[[ ! -e "$LEASE_ROOT/Virtual-2.owner" ]]
expect_ok 'mode Virtual-2 3024 1890 120000' 'OK mode Virtual-2 3024 1890 120000'
[[ $(<"$VKMS_DEVICE_DIR/connectors/Virtual-1/requested_mode") == '3024 1890 120000' ]]
expect_ok 'status Virtual-2' 'STATUS connected Virtual-2'
expect_ok 'disconnect Virtual-2' 'OK disconnected Virtual-2'
[[ ! -e "$LEASE_ROOT/Virtual-1.owner" ]]

expect_ok 'connect Virtual-4' 'OK connected Virtual-4'
[[ $(<"$VKMS_DEVICE_DIR/connectors/Virtual-2/status") == 1 ]]
[[ $(<"$LEASE_ROOT/Virtual-2.owner") == 1000 ]]
expect_ok 'mode Virtual-4 2560 1440 120000' 'OK mode Virtual-4 2560 1440 120000'
expect_error 1001 'mode Virtual-4 1920 1080 60000'
expect_error 1001 'disconnect Virtual-4'
[[ $(<"$VKMS_DEVICE_DIR/connectors/Virtual-2/requested_mode") == '2560 1440 120000' ]]
expect_ok 'disconnect Virtual-4' 'OK disconnected Virtual-4'

if response=$(control_connection 1000 <<< 'connect Virtual-3'); then
  printf 'noncontiguous mapping accepted an absent public name\n' >&2
  exit 1
fi
[[ "$response" == 'ERROR invalid connector' ]]
[[ $(<"$VKMS_DEVICE_DIR/connectors/Virtual-3/status") == 2 ]]
[[ ! -e "$LEASE_ROOT/Virtual-3.owner" ]]
if response=$(control_connection 1000 <<< 'connect Virtual-1'); then
  printf 'unmapped public Virtual-1 fell back to logical slot 1\n' >&2
  exit 1
fi
[[ "$response" == 'ERROR invalid connector' ]]

expect_ok 'connect Virtual-10' 'OK connected Virtual-10'
[[ $(<"$VKMS_DEVICE_DIR/connectors/Virtual-4/status") == 1 ]]
[[ $(<"$LEASE_ROOT/Virtual-4.owner") == 1000 ]]
[[ ! -e "$LEASE_ROOT/Virtual-10.owner" ]]
expect_ok 'mode Virtual-10 3033 1891 119880' 'OK mode Virtual-10 3033 1891 119880'
[[ $(<"$VKMS_DEVICE_DIR/connectors/Virtual-4/requested_mode") == '3033 1891 119880' ]]
expect_ok 'status Virtual-10' 'STATUS connected Virtual-10'
expect_ok 'disconnect Virtual-10' 'OK disconnected Virtual-10'

printf 'Private-display mode broker tests passed.\n'
