#!/bin/sh
set -eu

wrapper=$1
test_root=$(mktemp -d)
trap 'rm -rf -- "$test_root"' EXIT HUP INT TERM

write_state() {
  limit=$1
  provider=${2-mangohud}
  expires=$(($(date +%s) + 60))
  printf '%s\n' \
    'version=2' \
    "provider=$provider" \
    "limit=$limit" \
    'preset=custom' \
    'always_show_graph=0' \
    "owner_pid=$$" \
    "expires=$expires" > "$test_root/480.state"
}

probe='printf "config=%s\nlimit=%s\n" "$MANGOHUD_CONFIG" "${MANGOHUD_FPS_LIMIT-UNSET}"'

write_state 59.94
fractional=$(
  VIBEPOLLO_MANGOHUD_STATE_DIR=$test_root \
  MANGOHUD_CONFIG='position=top-right,fps_limit=30' \
  MANGOHUD_FPS_LIMIT=30 \
    "$wrapper" --appid 480 -- /bin/sh -c "$probe"
)
expected_fractional='config=read_cfg,position=top-right,fps_limit=30,fps_limit=59.94
limit=UNSET'
[ "$fractional" = "$expected_fractional" ]

write_state 120
integer=$(
  VIBEPOLLO_MANGOHUD_STATE_DIR=$test_root \
    "$wrapper" --appid 480 -- /bin/sh -c "$probe"
)
expected_integer='config=read_cfg,fps_limit=120
limit=120'
[ "$integer" = "$expected_integer" ]

write_state 116 proton
proton=$(
  VIBEPOLLO_MANGOHUD_STATE_DIR=$test_root \
  MANGOHUD=1 \
  MANGOHUD_CONFIG='fps_limit=30' \
  MANGOHUD_FPS_LIMIT=30 \
  LD_PRELOAD='/usr/$LIB/mangohud/libMangoHud_shim.so' \
    "$wrapper" --appid 480 -- /bin/sh -c \
      'printf "vkd3d=%s\nmango=%s\nconfig=%s\npreload=%s" "$VKD3D_FRAME_RATE" "${MANGOHUD-UNSET}" "${MANGOHUD_CONFIG-UNSET}" "${LD_PRELOAD-UNSET}"'
)
expected_proton='vkd3d=116
mango=UNSET
config=UNSET
preload=UNSET'
[ "$proton" = "$expected_proton" ]

rm -f -- "$test_root/480.state"
passthrough=$(
  VIBEPOLLO_MANGOHUD_STATE_DIR=$test_root \
    "$wrapper" --appid 480 -- /bin/sh -c 'printf pass'
)
[ "$passthrough" = pass ]
