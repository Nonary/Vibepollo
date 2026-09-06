# SteamOS support audit

Migration note: the validation results below describe the source Vibeshine
implementation. They are historical evidence, not a claim that this Vibepollo
migration has been tested on a live SteamOS host.

Audit date: 2026-09-04. Starting revision: `017cc604` plus the existing
untracked SteamOS packaging and Gamescope source files.

## Scope and findings

The starting files were a prototype, not an integrated SteamOS implementation.
`SUNSHINE_BUILD_STEAMOS` was documented but absent from CMake; Gamescope was not
compiled or selected; its capture implementation called missing PipeWire
methods and an undefined configuration policy. The session tests were not
registered. The system-extension directory contained templates only.

This change integrates stock Gamescope SDR capture, a separate user bundle, and
a pinned Gamescope patch with a versioned HDR10 capture contract. The modified
compositor has been built and tested in isolated headless instances. The active
SteamOS compositor and host drivers have not been replaced. Full SteamOS feature
parity is not established.

| Area | Implementation and evidence |
| --- | --- |
| Build and packaging | Dedicated CMake profile; generated Wayland protocol; relocatable assets and application dependencies; no machine-service hooks, udev rules or kernel installation. |
| Gaming Mode capture | Gamescope discovery precedes other automatic backends. Uses the private PipeWire node, SDR format negotiation and compositor downscaling. Wayland discovery has bounded waits. |
| Capture failure paths | Initializes PipeWire resource handles; rejects negotiation timeouts and corrupt memory buffers; retains CPU frame storage for the encoder; recreates capture after a size change. |
| Encoder lifecycle | VAAPI probe cleanup stays on the thread owning its EGL context. Hardware testing exposed a Mesa crash when the generic watchdog destroyed it on another thread before the next codec probe. |
| SDR capability probing | Unsupported HDR capture no longer invalidates an encoder's successful SDR probe. Stock Gamescope rejects HDR; patched Gamescope requires the separate profile and verified 10-bit PQ negotiation. |
| Session environment | Bounded, owned, non-symlink file reads; no shell evaluation; local X11 display imported only for a matching, verified Gamescope connection. |
| Startup privileges | The SteamOS profile requires empty permitted/effective capabilities and clears inherited bits, including stock `CAP_WAKE_ALARM`. Native machine-package entry checks remain unchanged. |
| Installation and upgrades | Serialized install/uninstall; payload execution and web asset preflight; restart on upgrade; restoration of previous release and launcher on activation failure. |
| State and relocation | XDG locations pinned in generated launcher; binary resolves assets from its own release; uninstall preserves configuration and pairings. |
| Desktop Mode | Existing Linux capture backends retained. Physical capture remains the user-bundle baseline. |
| Input | SteamOS host permits `/dev/uinput` and GPU render access. Xbox One is the profile default. `/dev/uhid` is inaccessible to this user, so DualSense is not a stock-install guarantee. |
| Audio and Steam | Existing PulseAudio/PipeWire path and `steam -applaunch` handoff retained. Local playback follows the client's request. Actual playback and game launch still require stream testing. |
| HDR and virtual outputs | Stock path reports SDR only. The optional patch provides full-range 10-bit BT.2020/PQ capture with a defined reference volume. Normal game launch/resume requests use the existing Gamescope scene even when virtual display is selected; no independent output is created. Managed KScreen initialization is skipped for Gamescope. |

## Validation

- Installer tests exercise install, upgrade restart, failed-activation rollback,
  custom XDG directories, malicious/duplicate environment assignments and state
  preservation, with mocked systemctl and temporary directories.
- C++ tests cover environment parsing, real file discovery, symlink/oversize/FIFO
  rejection, matching X11 import, and unresponsive/disconnected Wayland peers.
- The Linux capture-options UI regression suite passes (17 tests).
- All six relevant CTest suites pass: Steam integration, Gamescope session,
  Wayland timeout, SteamOS installation, SteamOS profile and startup capability
  sanitization. The capability test also passes natively with SteamOS's actual
  inherited `CAP_WAKE_ALARM` bit. The enabled graphical-session service graph
  passes `systemd-analyze --user --man=no verify` using temporary unit copies.
- Read-only checks on SteamOS 3.8.24 / build 20260716.2 confirm the PipeWire
  socket, GPU render access and `/dev/uinput` access. They do not prove streaming.
- The host builds in the existing Arch development container with GCC 16.1,
  pinned Boost 1.89 and the prepared FFmpeg dependencies. Both browser UIs build.
- A native headless Gamescope 3.16.23.5 process, displaying `glxgears` at
  1280×800/30 Hz, delivered 12 CPU frames at 640×400 and 12 GPU DMA-BUF frames
  at 800×500 for an 800×600 client request. Retaining the first CPU image across
  subsequent callbacks preserved its pixels. Both captures report SDR.
  This uses the actual capture objects on the Deck, but does not exercise a
  Moonlight client or the active Gaming Mode session.
- The hardware test exposed two negotiation races: accepting the native size
  before Gamescope applied downscaling, and treating an initialization-time
  PipeWire pause during renegotiation as a dead stream. Both were corrected.
- Native VAAPI probing completes successfully and discovers `h264_vaapi` and
  `hevc_vaapi`; AV1 is unavailable on this Deck. This validates codec startup,
  test packet production and teardown, not sustained client playback. HEVC
  emits a VUI warning, so client decoding remains an explicit release gate.
- The final staged executable loads natively; `ldd -r` reports no missing
  libraries or unresolved symbols. The actual bundle passes installation,
  launch from a different directory, uninstall and configuration preservation
  using a temporary home and mocked systemctl. No live service was installed.
- The local artifact and evidence are in
  `build/steamos-validation-20260904/` (ignored build output). The test compositor
  is isolated from the active graphical session and is stopped after validation.

### Development toolchain baseline

The available development container has glibc 2.43; this SteamOS host has
glibc 2.41 and GLib 2.84.3. An initially staged binary could not load on the
host. Bundling the newer C library is not the solution: the bundle excludes
C/math, GLib and graphics/session libraries. For this local validation build,
the executable was explicitly linked against the host's math ABI through the
container's read-only `/run/host` mount:

```text
CMAKE_EXE_LINKER_FLAGS=-Wl,-rpath-link,/run/host/usr/lib -Wl,-rpath-link,/run/host/usr/lib/pulseaudio -Wl,--push-state,--no-as-needed /run/host/usr/lib/libm.so.6 /run/host/usr/lib/libmvec.so.1 -Wl,--pop-state
```

The other options follow the bundle README, with
`CMAKE_DISABLE_FIND_PACKAGE_Boost=ON` to use the pinned fallback rather than the
container's incompatible partial Boost package. This is a local validation
setup, not a portable release SDK. A release pipeline still needs an explicitly
versioned SteamOS-compatible sysroot and artifact tests on every supported OS.

## Release gates still requiring actual streaming

Run the exact staged artifact, not a development-container binary, on each
supported SteamOS build. Record client version, Deck model, codec, resolution,
frame rate and logs for each check:

1. Pair Moonlight and sustain an SDR H.264 hardware-encoded stream in Gaming
   Mode. Verify the Steam UI, a native game and a Proton game; test HEVC separately.
2. Verify stereo audio, local playback on/off, keyboard, mouse, controller
   buttons, sticks, triggers and rumble. Check absolute input at native and
   downscaled resolutions and with differing client aspect ratios.
3. Switch to Desktop Mode and back. Each transition should stop the old host
   and start a process with fresh session credentials. Reconnect from Moonlight.
4. Test dock/undock, resolution changes, suspend/resume, client loss, compositor
   exit, and service restart during a stream. Check logs for recovery and leaked
   processes or file descriptors.
5. Upgrade with a live stream; verify the new process uses the new release.
   Exercise manual rollback and uninstall; pairing and settings must survive.
6. Repeat after a SteamOS update. Reject an artifact with missing runtime symbols
   or dependencies; do not disable the read-only root filesystem to accommodate it.

Independent higher-resolution virtual output, explicit synchronization and
compositor presentation timestamps still require additional compositor work.
HDR capture is implemented; privileged Gaming Mode deployment, Moonlight
playback and OS update/rollback checks remain release gates. The `sysext/`
templates alone activate nothing.

## Patched Gamescope HDR validation

The pinned patch, source lock and rebuild instructions are in
[`gamescope/`](gamescope/README.md). It applies cleanly to Valve commit
`1290cbc1a7ca625688bde8728d8e3b1e703d6a40` and reproduces the compiled source
changes. The client and compositor protocol XML files match byte for byte.

Hardware checks on the Deck pass:

- Stock Gamescope rejects HDR while retaining SDR capture.
- Patched Gamescope delivers 12 native DMA-BUF HDR frames with full-range
  10-bit BT.2020/PQ signaling. Black, 100, 203, 1,000 and 4,000-nit patches and
  RGB primaries match expected code values within eight 10-bit steps.
- SDR content maps into the HDR scene at 203-nit white with BT.709-to-BT.2020
  gamut conversion. The test exposed an upstream screenshot LUT endpoint
  interpolation loss (about 180 nits instead of 203); the capture-only LUT now
  normalizes its shaper domain and does not inherit panel looks or LUT overrides.
- The actual Vibeshine GPU converter, VAAPI encoder and packet-header replacement
  path produce 16 HEVC frames with Mesa 26.1.7/libva 2.24.1 in the development
  container. Native FFmpeg decodes all frames without errors, reports Main10,
  BT.2020 and ST 2084, and decoded RGB patches stay within eight 10-bit steps of
  the source values. Both HDR and SDR-source cases pass. These are real GPU
  tests using the host's PipeWire session, not mocked capture.

**The installed host driver remains a release blocker.** Its Mesa
`26.1.2.221562.radeonsi_26.1.2-1.1` generates malformed HEVC slice headers with
old Deck firmware. System FFmpeg reproduces the failure independently of
Vibeshine; the installed Vulkan encoder also fails decoding. Mesa's upstream
[firmware fix](https://gitlab.freedesktop.org/mesa/mesa/-/commit/5af4976e23a6dd361f5a1e0d26595b47d228c191)
is included in [26.1.7](https://docs.mesa3d.org/relnotes/26.1.7.html) and
[26.2.1](https://docs.mesa3d.org/relnotes/26.2.1.html). The successful encoder test
uses that fix only in the container; host packages have not been changed.
No packet-bit workaround is shipped. Require a compatible SteamOS driver
carrying the fix and rerun the decode tests before enabling HDR in Gaming Mode.

`tests/run-hdr-smoke.py` fails on decoder stderr or missing decoded frames even
when FFmpeg returns exit status zero. Capture-only results are labeled separately.
Evidence, bitstreams, decoded pixel measurements and candidate artifacts are in
`build/steamos-hdr-validation-20260904/`. Test compositor groups are stopped on
both success and failure. No live session or Vibeshine service was replaced.

The local Gamescope build also links against the host math, libinput, libavif
and LuaJIT ABIs, because the rolling development container has newer dependencies:

```text
-Dcpp_link_args=-Wl,-rpath-link,/run/host/usr/lib -Wl,--push-state,--no-as-needed /run/host/usr/lib/libm.so.6 /run/host/usr/lib/libmvec.so.1 /run/host/usr/lib/libinput.so.10 /run/host/usr/lib/libavif.so.16 /run/host/usr/lib/libluajit-5.1.so.2 -Wl,--pop-state
```

This remains a local SDK setup. The companion Gamescope archive is a development
candidate with source/patch hashes and host metadata, not a signed sysext image.
Privileged activation, Moonlight HDR playback, actual native/Proton HDR games,
mode switching and OS update/rollback behavior remain unvalidated.

## Local native deployment validation, 2026-09-05

The SteamOS work was merged with `vibe-test` revision `be846187`. The resulting
host was rebuilt from `fe6f9437`; subsequent changes only adjust a shell test
dependency and this audit. The staged bundle loads on SteamOS without missing
libraries or unresolved symbols. Fourteen selected CTest suites and 24 browser
UI checks pass, including dormant installation, Sunshine migration/rollback,
private-driver child-environment isolation and compositor deployment guards.

The [local deployment tools](local/README.md) build a private Mesa 26.1.7 VAAPI
driver against the host's libva 1.22 API, math ABI and traditional GNU TLS.
Its dependencies resolve from SteamOS. The host's Mesa package remains
unchanged; the private driver fixes the native encoder failure recorded above
for the Vibeshine process, without exporting its driver choice to launched games.

Native hardware checks now pass with that private driver and the installed
Vibeshine bundle's libraries: HDR and SDR-source patterns each produce 12
verified 10-bit capture frames and 16 HEVC Main10 frames. Native software decode
reports no errors, correct BT.2020/PQ signaling and expected decoded pixels.
Stock Gamescope still rejects HDR while providing SDR capture. All test
compositors were headless and stopped after validation.

Vibeshine and its private encoder runtime are installed but inactive. The
exact-host Gamescope candidate is staged for an additive, capability-preserving
`/opt` installation. Sunshine and the active desktop remain running while the
user streams. Root installation, live host replacement and Gaming Mode entry
await the user's check-in. Actual Moonlight HDR playback remains unvalidated.
Local build provenance and evidence are in `build/steamos-local-20260905/`.
The upstream `libvirtualdisplay` gitlink `398590ce` is unavailable publicly;
this build records use of its published predecessor `c1a087f`. The SteamOS
profile excludes the managed virtual-display driver affected by that change.

## Upstream protocol references

- [Valve Gamescope PipeWire protocol](https://github.com/ValveSoftware/gamescope/blob/master/protocol/gamescope-pipewire.xml)
- [Valve Gamescope PipeWire producer](https://github.com/ValveSoftware/gamescope/blob/master/src/pipewire.cpp)
- [Private requested-size property](https://github.com/ValveSoftware/gamescope/blob/master/src/pipewire_gamescope.hpp)

The installed SteamOS `gamescope-session.service`, `gamescope-session.target`,
`steam-launcher.service` and `/usr/lib/steamos/gamescope-session` were also inspected
to verify graphical-session ownership and how the environment file is published.
