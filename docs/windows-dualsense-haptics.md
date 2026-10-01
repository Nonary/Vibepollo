# Windows DualSense waveform haptics

Enable **DualSense waveform haptics (experimental)** in an application's editor.
The option is saved as `dualsense-haptics` in `apps.json` and defaults to false.
It selects a VHF DualSense for controllers allocated while that application is
active, without changing the saved global controller preference. The Windows
Vibeshine virtual gamepad driver and a Moonlight client implementing the existing
waveform PCM extension are required.

Use a direct game `.exe` command. Steam URI, Playnite, desktop, detached-command,
and shell launches are rejected. For a Steam game, create a manual application
with its executable and working directory. Games which hand control to an
already-running launcher are outside this implementation. Start a new stream
after changing this option; controllers allocated before the option was enabled
keep their previous profile.

## Launch and interception

Vibepollo creates a random, application-owned shared-memory mapping, restricted
to SYSTEM and the launch user, before running application commands. The helper
`tools/vibepollo_dualsense_haptics.exe` waits up to 15 seconds for a streamed
VHF DualSense slot and its enumerated HID container. This wait allows RTSP and
controller arrival to complete after the launch request, while the game has
not yet been created.

The helper creates the game suspended and stages a small native x64 entry-point
bootstrap. Windows initializes its loader normally. Before the executable entry
point runs, the bootstrap loads `tools/vibepollo_dualsense_audio.dll`, calls its
initializer, restores the original entry bytes, and signals readiness. Only a
successful initializer allows the game to proceed. No debugger is attached and
no remote thread is created. DLL initialization and TLS callbacks can run before
the executable entry point, so audio created from those callbacks is not covered
by this guarantee. The bootstrap uses two process-owned memory regions, released
when the game exits.

The DLL hooks `CoCreateInstance` for `IMMDeviceEnumerator`. It appends
process-local render endpoints for active streamed VHF DualSense slots and
preserves real endpoints and the real default audio device. Each added endpoint
reports the container ID used by libvirtualgamepad for that host slot and a
four-channel float mix format. It implements `IMMDevice`, `IMMEndpoint`,
`IPropertyStore`, `IAudioClient`, `IAudioRenderClient`, and `IAudioClock`.
This follows the matching and render path found in
[the 007 investigation](007-windows-haptics-re.md).

Shared-mode float32 and signed PCM16 four-channel streams at 8–192 kHz are
accepted. The actual negotiated rate is converted to stereo S16LE at 48 kHz;
only channels 3/4 reach the actuators. Packets contain 240 frames (5 ms). The
bounded IPC queue never blocks an audio submission. Host polling drops packets
older than 30 ms and forwards the remaining packets through the existing
gamepad feedback queue and encrypted `0x5601` control extension. Controller slot
generations invalidate open streams on disconnect or slot reuse.

The game can create children through `CreateProcessW` or `CreateProcessA`;
these receive the same hook before their entry points, including when they use
a custom environment block. A caller-requested suspended child stays suspended.
Failed initialization terminates the child rather than letting an unhooked
game continue. The helper remains the tracked application root, and the game
and its descendants remain in the existing application process group.

## Limits and validation

This is a process-local WASAPI implementation, not an installed Windows audio
driver. It does not replace the Sony pad API: the game must recognize the VHF
DualSense HID device itself. Adaptive triggers and other HID feedback keep the
existing VHF path. Other activation routes such as `CoCreateInstanceEx`,
`ActivateAudioInterfaceAsync`, and `IAudioClient2/3` are not intercepted.
Virtual endpoint notification events and exclusive-mode streams are not
implemented. The launch bootstrap currently supports native x64 Windows executables.
Cross-architecture child launches and explicit debugger launches are unsupported.
Games that refuse injected DLLs may reject this launch route.

The portable PCM tests cover signed actuator extraction, finite float handling,
clipping, block boundaries, sample-rate conversion, silence, and reset behavior.
`windows_dualsense_audio` is a hardware-free Windows integration probe covering
COM enumeration, Sony container matching, shared-mode render buffers, waveform
forwarding, silence, controller-generation invalidation, and hooked child
startup with a custom environment, caller-requested suspension, and no debugger.
It can also run under Wine with the helper DLL beside the probe executable.

These checks do not establish native Windows gameplay or physical haptics.
Acceptance still requires a Windows-host game session that creates the Sony
haptics sink, produces nonzero actuator samples, negotiates waveform support,
and delivers correctly timed physical feedback to the client controller.
