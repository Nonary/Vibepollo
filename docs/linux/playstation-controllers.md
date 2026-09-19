# PlayStation virtual controllers

Linux DS4 and DS5 emulation exposes USB HID devices (`BUS_USB` via UHID). The
portable descriptor and feature contracts in Inputtino's `ds4_usb.hpp` and
`ds5_usb.hpp` were ported from the libvirtualgamepad fixes identified in their
source comments. Updating the Windows driver submodule alone does not update
these Linux implementations. DualSense creation uses the USB report descriptor
and firmware from `ds5_usb.hpp` so libScePad sees the same transport as Windows.

Native games inspect feature usages, firmware revisions, calibration, and sensor
initialization before accepting input. Browser or Steam controller detection is
insufficient validation. Unknown feature operations must return an error.

Games that use Sony's `libScePad.dll` (007 First Light is one) also require the
firmware feature's UpdateVersion at offset 44 to be at least `0x0390`. Below
that the library reports a required DualSense firmware update and does not
program adaptive triggers or vibration. HD haptics still need the DualSense USB
audio function, which UHID does not provide; rumble and trigger effects go
through the HID output report.

The wire calibration is zero bias, 16 gyro counts per degree/second, and 8192
accelerometer counts per g. Inputtino's public DS5 motion API takes radians/second
and m/s²; Vibepollo's DS4 API takes degrees/second and m/s². Pairing replies must
agree with each device's unique address, in reversed wire order.

Build and run the normal suite, including `test_component_ds4_usb` and
`test_component_ds5_usb`. A separate manual test creates temporary controllers
visible to the desktop and Steam, sends a Cross press and synthetic motion,
and destroys only those controllers:

```sh
timeout 20 build/tests/probe_playstation_uhid
```

This verifies actual Linux UHID descriptors, feature reads/writes, unknown-request
rejection, and input reports. It requires access to `/dev/uhid` and the resulting
hidraw nodes. It does not establish game compatibility through Wine/Proton or
client feedback behavior. After deployment, reconnect the stream to recreate
controllers and validate the affected game with its actual Proton and Steam
Input settings. DS4 and DS5 selections retain their respective device identities;
game-specific translation by Steam or Proton is a separate layer.

## Global Proton DualSense compatibility

On Linux, **Input → DualSense compatibility for Proton games** defaults to on
(`proton_dualsense_compatibility = enabled`). During a stream, Vibeshine supplies
`PROTON_KEEP_SONY_AUDIO_ENDPOINT_VISIBLE=1` and
`PROTON_SONY_WINDOWS_DEVICE_NAMES=1` to Proton game launches.

The session-owned Proton hook covers games launched inside an already-running
Steam client, including Desktop and Big Picture streams. Brokered direct Steam
launches carry the same option explicitly. This policy is independent of HDR,
frame limiting, and the game AppID. Proton builds which do not implement these
variables do not gain support merely from having the variables set.

Explicit game environment values, including `0`, take priority. Turn the option
off to stop supplying these defaults; this does not erase user-authored game
settings. Reconnect the stream and relaunch the game after changing the option.
Without an active stream the installed Proton hook is inert. Existing game
processes keep their launch environment. The controller speaker is not made the
system's default audio output.
