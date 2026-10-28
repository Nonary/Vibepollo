# Windows DualSense waveform haptics

Vibepollo exposes the streamed controller as a wired USB DualSense with HID and
audio interfaces on the Windows host. The bundled installer transport currently
targets Windows x64. Compatible games see a normal Windows controller audio endpoint
and can submit native waveform haptics. Vibepollo sends the actuator samples to a
Moonlight client implementing the waveform extension, which renders them on the
physical DualSense. The client manages the physical controller connection;
that connection does not change the wired USB identity exposed to Windows games.

## Setup

1. In the Vibepollo installer, select the optional **DualSense USB audio and
   haptics** component. It is off by default and installs the bundled
   Microsoft-signed usbip-win2 transport. Restart Windows if the installer requests it.
2. Windows automatically uses the composite USB/audio backend whenever controller
   selection chooses DualSense and the optional transport is available. This includes
   Automatic with a PlayStation controller and the saved `vhf_ds5` profile; desktop
   and Steam Big Picture streams do not need an application opt-in. Explicit Xbox,
   DualShock 4, and Switch profiles keep their selected identity. The application
   **DualSense waveform haptics** option (`dualsense-haptics` in `apps.json`) can
   still force DualSense and delay game launch until its audio endpoint is ready.
   **DualSense with waveform haptics** (`gamepad = usbip_ds5`) also forces this backend.
3. Start a new stream so controllers are allocated on the composite backend.
   Use a Moonlight client with waveform support and a compatible DualSense renderer.
   Stock clients without this extension cannot render native waveform feedback.
4. Enable native DualSense support in the game. Game-specific Steam Input settings
   can determine whether the game sees the DualSense or a translated controller.

Direct executables, Steam launches, Playnite launches, and desktop streaming use
the same controller audio endpoint. With the per-application option enabled,
Vibepollo waits up to 30 seconds for a waveform-capable streamed controller and
its HID and active audio endpoint before running preparation commands or starting
the game. Global `usbip_ds5` selection creates the device when a controller arrives;
a desktop stream can still start with only a mouse and keyboard.
Games launched outside Vibepollo can also use the endpoint and send waveform
haptics while the composite controller is connected, including after an opted-in
application ends. The per-application option does
not require a special game-launch helper or an injected audio DLL.

If the optional transport is unavailable, the per-application option reports an
error before launching the application. An explicitly selected composite controller
does not silently substitute a VHF or ViGEm controller. Run the installer again with
the optional component selected, follow any restart request, and reconnect the stream.
The shared usbip-win2 driver remains installed when Vibepollo is uninstalled.

## Controller and audio path

The existing shared Vibeshine VHF driver exposes HID controllers. The composite route uses
usbip-win2's virtual USB host controller and a local user-mode device emulator to
expose HID and audio functions under the same USB device. Windows' built-in HID and
USB audio drivers enumerate these functions and associate their container IDs.
This is the same controller/audio contract used by the Linux composite DualSense.

The game writes four-channel, 48 kHz S16LE audio to the controller endpoint.
Only actuator channels 3/4 enter the waveform transport; controller-speaker channels
and microphone audio are outside this extension. Vibepollo batches the signed
samples into 240 stereo frames (5 ms) and forwards them through the existing
gamepad feedback queue and encrypted `0x5601` control extension. PCM admission is
bounded so audio cannot clear pending rumble, LED, or adaptive-trigger messages.
Slot teardown discards outstanding samples before that controller index is reused.
Moonlight's **keep controllers attached after disconnect** setting (`gcpersist`)
also preserves the composite controller's USB audio endpoint while the application
is paused. Resume reuses that endpoint and routes feedback to the new client
connection, so the game can keep its existing haptics audio connection. Feedback
is discarded during the pause, and ending the application removes retained devices.
Without that setting, disconnect removes the controller and its audio endpoint;
games that do not reopen controller audio after device removal may require a restart.

Buttons, axes, touchpad, motion, and battery state arrive from the client through
the composite HID interface. HID output reports carry ordinary rumble, LEDs, and
adaptive-trigger effects through the existing feedback messages. Waveform feedback
requires both the connection's `ML_FF_HAPTICS_PCM` support and the controller's
`LI_CCAP_HAPTICS_PCM` capability.

The installer redistributes the unmodified signed usbip-win2 package. Building or
modifying those kernel drivers requires separate signing; installing this release
package does not require enabling Windows test-signing mode. The transport is open
source under BSD-2-Clause; its notices are included with the bundled package.
Windows distributions also include the HIDMaestro and libvirtualgamepad MIT
notices in `licenses/dualsense_usbip_NOTICES.txt`.

## Diagnosing missing waveform feedback

Enable Debug logging, start a fresh stream, then reproduce native haptics in the
game. For the affected controller, check these boundaries in order:

- `received nonzero USB actuator PCM`: the host received nonzero actuator bytes.
  This alone does not mean a frame was forwarded.
- `PCM not forwarded`: gives the admission reason, such as an inactive binding,
  an endpoint not ready, missing controller capability, or a missing queue.
- `PCM frame accepted by feedback queue`: a complete 240-frame block reached the
  host's feedback queue. A rejection instead means that queue was full or stopped.
- `Dropping waveform haptics ... mlFeatureFlags=`: the connection did not negotiate
  waveform support, even if its individual controller advertised the capability.
- `Waveform haptics ENet queue accepted frame`: ENet accepted an outgoing frame.
  This is not proof of delivery or playback. The same diagnostic reports a
  rejection, negotiated flags, and the peer's channel count.

These diagnostics are bounded per controller binding or stream. Resume starts
fresh backend diagnostics so a previous connection cannot hide new failures.
They contain controller indices and protocol state, not audio samples. If ENet
accepts frames but the controller remains silent, client reception and rendering
still require investigation; do not treat host queue acceptance as a hardware fix.

## Earlier process-local audio implementation

The source tree retains the earlier `tools/vibepollo_dualsense_haptics.exe` and
`tools/vibepollo_dualsense_audio.dll` implementation for development and its
hardware-free integration probes. That implementation creates process-local WASAPI
endpoints through injected COM hooks and pairs them with a VHF DualSense. Its launch
and interception restrictions do not apply to the installed composite controller.
The `dualsense-haptics` application option now selects the USB/audio route and does
not fall back to these hooks when the optional driver is missing.

## Validation

Portable tests cover descriptor and feature replies, controller identity, USB/IP
request handling, signed actuator extraction, block boundaries, and controller
lifetime behavior. These checks establish protocol and sample handling; they do
not establish native Windows gameplay or physical haptics.

Acceptance requires a Windows-host game session that discovers the composite HID
controller and matching audio endpoint, produces nonzero actuator samples,
negotiates waveform support, and delivers correctly timed physical feedback to the
client controller. Check distinct left/right effects, simultaneous input and
adaptive triggers, client controller disconnect, reconnect, and stream teardown. Also
check a client without waveform support and confirm that audio samples cannot reach
a different controller after slot reuse.
