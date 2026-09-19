# PlayStation virtual controllers

Linux DS4 and DS5 emulation exposes USB HID devices. DS5 uses the composite
USB driver when `/dev/vibeshine-ds5` is available and otherwise uses UHID
(`BUS_USB`); DS4 uses UHID. The
portable descriptor and feature contracts in Inputtino's `ds4_usb.hpp` and
`ds5_usb.hpp` were ported from the libvirtualgamepad fixes identified in their
source comments. Updating the Windows driver submodule alone does not update
these Linux implementations. DualSense creation uses the USB report descriptor
and firmware from `ds5_usb.hpp` for UHID. The composite driver's descriptor
and feature replies must satisfy the same contract.

Native games inspect feature usages, firmware revisions, calibration, and sensor
initialization before accepting input. Browser or Steam controller detection is
insufficient validation. Unknown feature operations must return an error.

Games that use Sony's `libScePad.dll` (007 First Light is one) also require the
firmware feature's UpdateVersion at offset 44 to be at least `0x0390`. Below
that the library reports a required DualSense firmware update and does not
program adaptive triggers or vibration. HD haptics still need the DualSense USB
audio function, which UHID does not provide; rumble and trigger effects go
through the HID output report. Keep UpdateVersion consistent in Inputtino,
the portable Windows contract, and the Linux composite driver. Replacing
the firmware reply with an older hardware capture can regress this field
even when FirmwareVersion at offset 28 remains valid.

The machine service needs both `DeviceAllow=/dev/vibeshine-ds5 rw` and the
`vibepollo-uinput` group on that device. An `InaccessiblePaths` recovery
override still blocks it even when those permissions are correct. Compare
the loaded `/sys/module/vibeshine_ds5/srcversion` with
`modinfo -F srcversion vibeshine_ds5` after updating the module; installing
the module does not replace an already loaded copy.

Native packages include `/usr/libexec/vibeshine/vibeshine-ds5-install`, the
versioned `/usr/src/vibeshine-ds5-*` sources and DKMS configuration, and
`/usr/lib/modules-load.d/70-vibeshine-ds5.conf` for boot loading. Package hooks
build and load the module; `scripts/linux_install.sh` checks the result and
fails if DualSense installation remains incomplete. The helper's `status`
command returns 0 for matching installed/loaded source revisions, 4 when a
reboot is required, and a failure for a missing or unloaded module. To repair
an existing installation, run `sudo /usr/libexec/vibeshine/vibeshine-ds5-install install`.
After first loading the module, restart the session controller and reconnect
the stream so systemd grants the newly created device and Vibepollo creates
a composite controller. Restarting disconnects the stream and its game.

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

For the composite driver, with gadget slot 3 free:

```sh
timeout 30 build/tests/probe_ds5_gadget_lifecycle
```

This checks twenty create/destroy cycles, slot reuse, firmware UpdateVersion,
continuous input reports, and Cross press/release. Both probes create devices
visible to Steam and must remain manual tests.

This verifies actual Linux UHID descriptors, feature reads/writes, unknown-request
rejection, and input reports. It requires access to `/dev/uhid` and the resulting
hidraw nodes. It does not establish game compatibility through Wine/Proton or
client feedback behavior. After deployment, reconnect the stream to recreate
controllers and validate the affected game with its actual Proton and Steam
Input settings. DS4 and DS5 selections retain their respective device identities;
game-specific translation by Steam or Proton is a separate layer.

## Native waveform feedback to Moonlight

The coordinated Moonlight fork can advertise `LI_CCAP_HAPTICS_PCM` (`0x8000`) for
a controller with a waveform renderer and SDP `ML_FF_HAPTICS_PCM` (`0x04`) for
the connection. With the controller capability present, Inputtino passes the
virtual USB audio samples to Vibepollo instead of reducing PCM to rumble RMS.
Normal HID rumble and adaptive-trigger callbacks remain separate. Clients that
do not advertise the capability keep the existing PCM-to-rumble fallback.

Vibepollo extracts only actuator channels 3/4, preserving their signed S16LE
samples, and batches 240 stereo frames (5 ms at 48 kHz). Control type `0x5601`
contains a versioned payload defined in `ControllerHaptics.h` in both copies of
moonlight-common-c. It uses the existing encrypted control connection and
unreliable ENet channel `0x08`, so missing samples cannot hold up input or reliable
feedback. PCM uses non-purging queue admission; it cannot clear pending rumble,
LED or trigger messages. Connections advertising waveform support poll control
at most every 5 ms so haptics continue when input is idle.

The initial client renderer supports Linux Bluetooth DualSense and DualSense
Edge through hidraw and the MPL-2.0 SAxense packet format. The client repository
retains the pinned upstream source/license, adaptations and credits, and embeds
them in the binary (`moonlight --haptics-license`). No SAxense code is linked into
Vibepollo or Inputtino. USB/other clients retain rumble fallback; speaker and
microphone transport are outside this extension.

Test channel extraction/packet validation with `test_ds5_haptics`, and test the
client resampler/worker using Moonlight's `tests/haptics/haptics.pro`. Validation
must also include native game haptics, distinct left/right effects, simultaneous
input and adaptive triggers, Bluetooth interruption, reconnect, stream teardown,
and an unmodified client. Neither a build nor the socket-backed playback test
establishes that physical end-to-end behavior.

The virtual HCD must schedule individual isochronous packets using the URB's
interval. Completing a whole URB at each 125-us scheduler tick speeds up ALSA's
sample clock and truncates packets after one gadget request fills. The corrected
scheduler retains per-URB packet progress and a per-endpoint deadline. With slot
3 free, the manual probe below checks signed sample continuity and approximately
48 kHz delivery through the actual kernel/ALSA/Inputtino path:

```sh
timeout 12 build/tests/probe_ds5_haptics
```

It requires ALSA development files at build time and access to the newly created
virtual sound node at runtime. It creates and destroys a temporary controller,
so it is deliberately excluded from automatic CTest runs.

## Global Proton DualSense compatibility

On Linux, **Input → DualSense compatibility for Proton games** defaults to on
(`proton_dualsense_compatibility = enabled`). During a stream, Vibepollo supplies
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

Vibepollo-aware Moonlight clients also include a PlayStation-controller bitmap
in the launch request. A direct Proton launch with compatibility enabled waits
up to two seconds for the matching virtual DualSense and its audio playback
endpoint to enumerate before Steam starts the game. This prevents one-time
Sony/Wwise initialization from racing the controller and its audio endpoint.
Older clients omit the hint and retain the previous launch ordering.

The game must implement native haptics/adaptive triggers. Test the actual Proton
build and game, especially controller reconnects. The initial 007-only diagnostic
settings below are historical and are superseded by this global policy once the
new host is installed. Remove that explicitly marked local diagnostic block when
migrating so it does not override the global opt-out.

## 007 First Light with GE-Proton11-5

A working host-to-client waveform probe does not validate the game's audio
endpoint discovery. In the September 19 runtime investigation, 007's native
`libScePad.dll` opened the virtual USB DualSense (054c:0ce6), returned controller
type 2, and accepted rumble. Its Wwise haptics sink nevertheless failed:

- `AkQuadAudioHapticsSink` matches the HID container ID against enumerated
  MMDevice render endpoints. GE-Proton11-5 hid the activated Sony mono endpoint,
  so none of the six returned endpoints matched the controller.
- Making that endpoint visible allowed the container comparison and activation
  to succeed. Wwise then required `GetMixFormat().nChannels == 4`; the default
  mono format still prevented stream initialization.
- Selecting the existing Windows Sony audio mode made `GetMixFormat` return
  four-channel, 48 kHz audio. Wwise's stream initialization returned success,
  the virtual controller's PCM started, and the user reported gameplay feedback
  working. No game executable or Sony library was patched.

For this Proton build, set both variables in the **007 process environment**:

```sh
PROTON_KEEP_SONY_AUDIO_ENDPOINT_VISIBLE=1
PROTON_SONY_WINDOWS_DEVICE_NAMES=1
```

On the investigated host these are scoped to Steam AppID `3768760` in the
selected GE-Proton build's `user_settings.py`, preserving its existing stream
limiter hook. They take effect on the next game launch; that fresh-launch path
still needs user validation. Do not infer compatibility with other Proton
versions from this test. The live controlled test restored its temporary
mode/visibility flags after the game had successfully opened its haptics stream.

The mono PipeWire profile alone is not proof of failure: this Proton build
intentionally switches from Direct to its speaker profile and implements the
four-channel Windows interface through a separate haptics route. Forcing Direct
with `pactl` is therefore insufficient. Inspect the format and container ID
actually returned to the game. Adaptive-trigger effects use a separate HID path;
waveform stream initialization alone does not establish that the game sends them.
