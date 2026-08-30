#!/bin/sh
set -eu

wrapper=$1
test_root=$(mktemp -d)
trap 'rm -rf -- "$test_root"' EXIT HUP INT TERM

write_state() {
  limit=$1
  provider=${2-mangohud}
  preset=${3-custom}
  always_show_graph=${4-0}
  limiter_method=${5-late}
  expires=$(($(date +%s) + 60))
  printf '%s\n' \
    'version=2' \
    "provider=$provider" \
    "limit=$limit" \
    "preset=$preset" \
    "always_show_graph=$always_show_graph" \
    "limiter_method=$limiter_method" \
    "owner_pid=$$" \
    "expires=$expires" > "$test_root/480.state"
}

probe='printf "config=%s\nlimit=%s\n" "$MANGOHUD_CONFIG" "${MANGOHUD_FPS_LIMIT-UNSET}"'

write_state 59.94 mangohud custom 0 early
fractional=$(
  VIBEPOLLO_MANGOHUD_STATE_DIR=$test_root \
  MANGOHUD_CONFIG='position=top-right,fps_limit=30' \
  MANGOHUD_FPS_LIMIT=30 \
    "$wrapper" --appid 480 -- /bin/sh -c "$probe"
)
expected_fractional='config=read_cfg,position=top-right,fps_limit=30,fps_limit_method=early,fps_limit=59.94
limit=UNSET'
[ "$fractional" = "$expected_fractional" ]

write_state 120
integer=$(
  VIBEPOLLO_MANGOHUD_STATE_DIR=$test_root \
    "$wrapper" --appid 480 -- /bin/sh -c "$probe"
)
expected_integer='config=read_cfg,fps_limit_method=late,fps_limit=120
limit=120'
[ "$integer" = "$expected_integer" ]

write_state 116 proton
proton=$(
  VIBEPOLLO_MANGOHUD_STATE_DIR=$test_root \
  MANGOHUD=1 \
  MANGOHUD_CONFIG='fps_limit=30' \
  MANGOHUD_FPS_LIMIT=30 \
  DXVK_CONFIG='dxgi.maxFrameRate = 30' \
  LD_PRELOAD='/usr/$LIB/mangohud/libMangoHud_shim.so' \
    "$wrapper" --appid 480 -- /bin/sh -c \
      'printf "vkd3d=%s\ndxvk=%s\nmango=%s\nconfig=%s\npreload=%s" "$VKD3D_FRAME_RATE" "$DXVK_CONFIG" "${MANGOHUD-UNSET}" "${MANGOHUD_CONFIG-UNSET}" "${LD_PRELOAD-UNSET}"'
)
expected_proton='vkd3d=116
dxvk=dxgi.maxFrameRate = 30; dxvk.maxFrameRate = 116; dxgi.maxFrameRate = 116; d3d9.maxFrameRate = 116
mango=UNSET
config=UNSET
preload=UNSET'
[ "$proton" = "$expected_proton" ]

write_state 59.94 mangohud-proton 3 1
combined=$(
  VIBEPOLLO_MANGOHUD_STATE_DIR=$test_root \
  MANGOHUD_CONFIG='position=top-right,fps_limit=30' \
  MANGOHUD_FPS_LIMIT=30 \
  LD_PRELOAD='/usr/lib/libz.so.1' \
    "$wrapper" --appid 480 -- /bin/sh -c \
      'printf "vkd3d=%s\ndxvk=%s\nmango=%s\nconfig=%s\nlimit=%s\npreload=%s" "$VKD3D_FRAME_RATE" "$DXVK_CONFIG" "$MANGOHUD" "$MANGOHUD_CONFIG" "${MANGOHUD_FPS_LIMIT-UNSET}" "$LD_PRELOAD"'
)
expected_combined='vkd3d=59.94
dxvk=dxvk.maxFrameRate = 60; dxgi.maxFrameRate = 60; d3d9.maxFrameRate = 60
mango=1
config=read_cfg,position=top-right,fps_limit=30,preset=3,no_display=0,frame_timing=1,fps_limit=0
limit=UNSET
preload=/usr/lib/libz.so.1:/usr/$LIB/mangohud/libMangoHud_shim.so'
[ "$combined" = "$expected_combined" ]

rm -f -- "$test_root/480.state"
passthrough=$(
  VIBEPOLLO_MANGOHUD_STATE_DIR=$test_root \
    "$wrapper" --appid 480 -- /bin/sh -c 'printf pass'
)
[ "$passthrough" = pass ]
