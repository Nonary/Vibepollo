# Gaming Mode virtual-display investigation — 2026-09-06

## Saved direction

On 2026-09-06, the user asked to retain this idea for future work and defer
implementation: **AMD rendering → managed virtual screen → completed-frame
capture**, keeping the existing Gaming Mode Steam session and its games together.
The aim is client-specific resolution, refresh rate, and HDR through the existing
virtual-display infrastructure. Treat this as the preferred direction to
investigate when the work resumes, not a validated solution or a request to
deploy it now. Buffer ownership and encoder throughput remain separate concerns.

The desired path is the existing Gaming Mode Steam session rendered on AMD,
presented to a managed `vibeshine_drm` connector, and captured through completed
DRM frames. This is a proposed compositor integration, not an available setting.
It would give the stream its own mode and HDR display contract. It does not by
itself establish an encoding frame-rate guarantee or eliminate synchronization
bugs.

## Evidence from this Deck

The user reports Big Picture and Hades menus freezing until gameplay resumes.
The September 6 user-service journal shows:

- 15:54:51: Gaming Mode launched Steam app 1145350 through the running client.
- 15:54:51–52: requested 3024×1890 at 120 fps; Gamescope negotiated 2560×1440
  HDR10 capture. The encoder still produced 3024×1890 frames.
- 16:01:15: average capture interval 15.47 ms, approximately 65 frames/s.
- 16:02:15: maximum capture interval 8849.92 ms. This proves a capture gap,
  not which menu was visible or whether the source should have been animating.
- Around 16:01–02: HEVC encode calls averaged approximately 10 ms, exceeding
  the 8.33 ms budget for sustained 120 fps in the serial encode loop.
- Later logs from process 749758 use KWin in Desktop Mode and should not be
  confused with the Gaming Mode session (process 741786).

## Menu-capture defect

In pinned Valve commit `1290cbc1a7ca625688bde8728d8e3b1e703d6a40`,
`src/steamcompmgr.cpp::paint_pipewire` suppresses capture when the focus and
override commit IDs are unchanged. It later paints `overlayWindow`, but the
overlay does not participate in that suppression decision. An overlay-only
update can therefore remain invisible in capture until a game commit arrives.
This matches the reported symptom without requiring multiple compositors.
It does not prove every observed Hades menu freeze has that cause.

The bundled patch now includes layer identity, commit, opacity, capture size,
and pixel format in the decision, and records state only after successful GPU
rendering. App-specific capture continues excluding Steam overlays. The
regression harness executes the patched decision block with synthetic window
states. It does not validate full compositor compilation or actual menu pixels.

## Why selecting Virtual-1 is insufficient

`src/platform/linux/gamescope_display_backend.cpp` explicitly routes sessions
to the existing Gamescope scene and clears their virtual-display request.
The managed-display integration depends on KScreen/KWin for mode and topology
changes; those APIs do not control Gaming Mode Gamescope.

The pinned Gamescope `src/Backends/DRMBackend.cpp::init_drm` obtains the KMS
device from `vulkan_primary_dev_id`, then opens that device's primary node.
The AMD render device and `vibeshine_drm` connector are different DRM devices.
Connector preference cannot cross that device boundary. Gamescope already has
DMA-BUF framebuffer import and backend modifier negotiation, which make a
separate scanout device a plausible extension, but compatibility is untested.

## Concrete implementation path

1. Add explicit scanout-device selection to Gamescope while retaining AMD as
   its Vulkan renderer. Negotiate the intersection of AMD export and virtual
   plane import formats/modifiers. Use the virtual connector's single primary
   plane so Gamescope composes menus and game content into the captured image.
2. Add a Gaming Mode display controller that acquires the managed connector
   lease, requests the client mode, and asks Gamescope to switch output. It
   must report the accepted mode and HDR state without invoking KScreen.
3. Route this session to the existing managed KMS capture helper, including
   presentation sequencing and GPU synchronization. Audit producer reuse as
   well as framebuffer lifetime; retaining a DMA-BUF descriptor alone does
   not prevent its pixels from being overwritten.
4. Preserve the existing Steam instance and launch handoff. A second headless
   Gamescope can render its own clients, but does not automatically move the
   running Steam UI or games launched by that Steam instance into it.
5. Restore the prior Gaming Mode output on disconnect or failure. Supporting
   simultaneous local and virtual output needs additional multi-output work;
   an initial single-output implementation would switch the session's output.

First validate a separate test compositor presenting an animated pattern to
an unused leased virtual connector, with AMD rendering and the completed-frame
probe. Do not switch the real Steam session until that produces correct frames,
HDR values, requested modes, and clean teardown. Then validate Big Picture,
Hades menus/gameplay, overlay visibility changes, reconnect, and restoration.
Measure 1080p60 and the requested 3024×1890/120 separately; reducing duplicate
composition cannot remove the measured encoder cost.

## Separate shared-capture risk

`src/platform/linux/pipewire.cpp::fill_img_dmabuf` duplicates buffer FDs.
`on_process` returns the previous buffer to PipeWire on the next frame, while
the encoder may still consume the duplicate. PipeWire permits reuse after
queueing. This is a buffer-ownership race consistent with tearing, not proof
that it caused the observed artifact. Fixing it requires retaining the producer
buffer through GPU read completion or copying into consumer-owned storage with
proper synchronization. It is not fixed by the menu patch.

References: [pinned compositor source](https://github.com/ValveSoftware/gamescope/blob/1290cbc1a7ca625688bde8728d8e3b1e703d6a40/src/steamcompmgr.cpp),
[pinned DRM backend](https://github.com/ValveSoftware/gamescope/blob/1290cbc1a7ca625688bde8728d8e3b1e703d6a40/src/Backends/DRMBackend.cpp),
[PipeWire buffer lifecycle](https://docs.pipewire.org/page_streams.html).
