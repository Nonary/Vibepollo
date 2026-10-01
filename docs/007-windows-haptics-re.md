# 007 First Light: user-mode haptics interception

Static investigation on 2026-09-30 of the installed Windows binaries. The game
files were inspected on a read-only Btrfs mount. No game files were modified,
and no game process was launched. These findings establish interception points;
they do not establish that an implemented shim works in gameplay.

## Binary identity

Original installation: `SteamLibrary/steamapps/common/007 First Light/Retail`.

| Binary | SHA-256 |
| --- | --- |
| `007FirstLight.exe` | `d3a7dad42b1d71a2ce581392d77457a474cf01baa5dd112a339d1ce96ccd6aef` |
| `libscepad.dll` | `c27452712df85b04bb4043ee61badcc34036fd1a389f9fecb2885ba1ebc134a3` |

Executable preferred image base: `0x140000000`. DLL preferred image base:
`0x180000000`. Addresses below are RVAs, relative to the applicable loaded
module, so ASLR must be accounted for. Hashes and instruction patterns must
be checked before using these locations with another game update.

## Findings

The executable imports 16 Sony pad functions, including `scePadOpen`,
`scePadReadState`, `scePadGetControllerType`, `scePadGetContainerIdInformation`,
`scePadSetTriggerEffect`, `scePadSetVibrationMode`, and `scePadSetVibration`.
The DLL exports 30 functions. Its imports cover HID, SetupAPI, kernel functions,
and WINMM timer functions. Its import table does not show a waveform render API.
This is supporting evidence, not proof that it never dynamically loads one.

The decisive waveform path is in the executable. Embedded source strings name
Wwise `2023.1.8_bv` and `AkQuadAudioHapticsSink.cpp`. Disassembly connects that
source string to the sink allocation and initialization code and connects its
vtable to audio processing and WASAPI submission.

At executable RVA `0x6761aa`, the game loads Sony function pointers and calls
the exported `AkMotionInitializeScePadFunctions` at RVA `0x27bc2a0`. It passes
`scePadGetHandle`, `scePadGetContainerIdInformation`, `scePadGetControllerType`,
`scePadSetVibrationMode`, and `scePadSetVibration`. Wwise stores these callbacks
and invokes them through wrappers. There is no PCM callback in this set.

## Audio discovery and initialization

Endpoint lookup at RVA `0x28e2050`:

1. Calls Sony's container-ID callback with a controller handle.
2. Creates `MMDeviceEnumerator` and requests render endpoints with state mask
   `0x0f`.
3. Opens each endpoint property store and retrieves property
   `{8c7ed206-3f8a-4827-b3ab-ae9e1faefc6c}, 2` (device container ID).
4. Converts the endpoint GUID to text and compares it to the Sony callback's
   string, starting four bytes into that callback's output structure.
5. Returns the matching `IMMDevice`.

Sink initialization at RVA `0x28e2250`:

- Checks controller type through the registered Sony callback.
- Looks up the associated audio endpoint.
- Activates `IAudioClient` (IID
  `1cb9ad4c-dbfa-4c32-b178-c2f568a703b2`).
- Calls `GetMixFormat` at RVA `0x28e24fc` and explicitly checks
  `WAVEFORMATEX.nChannels == 4` at RVA `0x28e2511`.
- Adjusts the requested sample rate to Wwise's rate when necessary, initializes
  a shared-mode stream, obtains the buffer size, and requests
  `IAudioRenderClient` (IID `f294acfc-3146-4483-a7bf-addca7c260e2`).
- Allocates two planes of 32-bit samples for the haptics buffer.

The static code establishes a four-channel requirement. It does not hardcode
48 kHz at this point: the requested rate comes from the audio engine. A shim
must inspect that rate rather than assume it.

## Waveform buffers

Sink vtable RVA: `0x2ad3750`. The following roles are inferred from code and
their order in the sink interface, rather than debug symbols for these methods.

| Slot | Function RVA | Observed behavior |
| --- | --- | --- |
| 0 | `0x28e1bb0` | Destructor |
| 1 | `0x28e2250` | Initialization and endpoint activation |
| 2 | `0x28e2800` | Releases audio objects and sample storage |
| 3 | `0x28e27e0` | Starts `IAudioClient` |
| 4 | `0x28e1c40` | Consumes two float sample planes and applies a gain ramp |
| 5 | `0x28e26c0` | Submits the accumulated frames to WASAPI |

The consume function reads the input buffer's data pointer at offset `0`,
capacity at offset `0x10`, and valid frames at offset `0x12`. The second sample
plane starts at `data + capacity * sizeof(float)`. It applies the supplied gain
ramp and stores two output planes in the sink's buffer at object offset `0x30`.

The submission function reads capacity at sink offset `0x40` and valid frames
at offset `0x42`. It obtains a WASAPI buffer and writes, per frame:

```text
channel 1 = 0
channel 2 = 0
channel 3 = first haptics plane's 32-bit sample
channel 4 = second haptics plane's 32-bit sample
```

The interleaving loop is at RVA `0x28e2741`. `IAudioRenderClient::ReleaseBuffer`
is called at RVA `0x28e2793`. An empty block takes a separate silent-buffer path.
The combination of float multiplication in consume and 32-bit copies here
identifies the sample representation at this interception point as float PCM.

## Proposed user-mode shim

For this binary, a pad DLL replacement alone does not capture native waveform
haptics. It can, however, be the loader and controller-facing portion of a shim
which also redirects the embedded Wwise sink.

Two candidate designs follow from the inspected code:

1. Supply controller input/identity through a Sony pad API proxy and replace
   the sink initialization, start, submission, and teardown methods. Retain
   the original consume method and its gain processing, provide its required
   sample storage, and send its two output planes to Vibepollo at frame end.
   This avoids needing an actual Windows audio endpoint. The replacement must
   preserve sink lifecycle, success codes, buffer layout, and pacing behavior.
2. Supply the same pad identity plus process-local implementations of
   MMDevice/AudioClient/AudioRenderClient interfaces. Let the original sink run
   and capture channels 3/4 when it releases its fake render buffer. This
   requires more COM interface emulation but avoids replacing the sink's
   sample processing.

Both are user-mode proposals: the game would communicate with intercepted
interfaces inside its own process, so no virtual controller or audio device
needs to be registered with Windows. Neither proposal has been implemented or
validated. Controller API emulation must still make the game create the haptics
output; merely tapping consume will not work if endpoint initialization fails.

The first design is a focused prototype candidate for this exact 007 build.
It should convert float samples to the existing stream's stereo S16LE format
using the negotiated engine rate, preserve output gain, bound the feedback
queue, and handle silence and disconnects. Adaptive triggers remain separate
Sony feedback commands which the proxy must forward independently.

## Evidence and remaining work

Raw import/export dumps, selected disassembly, hashes, and the full executable
disassembly were retained in `/tmp/007-haptics-analysis`. Objdump's nearest
export labels in the selected disassembly are often unrelated to the actual
internal function; use the addresses and instruction flow, not those labels.

Next implementation work is a diagnostic pad proxy with controller API logging
and a replacement haptics sink. Live validation must establish that the game
recognizes the emulated pad, creates the sink without a real Sony audio device,
produces nonzero samples during gameplay, and delivers them to the client with
appropriate timing. No Windows host runtime or physical haptics validation was
performed in this investigation.
