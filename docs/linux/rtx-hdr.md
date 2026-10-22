# RTX HDR on Linux

Native Linux builds with CUDA on x86-64 include NVIDIA's pinned VFX TrueHDR
runtime. This converts SDR capture into HDR10 for FFmpeg NVENC and native
NVENC and PyroWave streaming; Python is used only to fetch the build dependency, never in
the live capture/encode loop. The Web UI exposes app opt-in and global tuning
in both interfaces on Linux NVIDIA hosts.

Enable HDR in the client, select an NVIDIA encoder, and enable RTX HDR for the
application. The capture output stays SDR while the stream advertises
BT.2020/PQ and the selected peak luminance. SDR-only clients, forced SDR and
10-bit SDR do not enable conversion. Linux PyroWave supports RTX HDR on NVIDIA
GPUs. Windows PyroWave uses the existing D3D11/NGX TrueHDR runtime.
Other Linux encoder/capture paths reject a requested RTX HDR session rather
than relabeling unconverted SDR as HDR.

For NVENC, capture must reach the CUDA/GL path (normally KMS on this host). Crop and
cursor composition occur before conversion. The scanout LUT is applied in SDR
before TrueHDR, then removed from the subsequent HDR-to-YUV pass so it cannot
be applied twice. Frame transfers between GL, the SDK and the encoder remain
on the GPU. CUDA contexts, streams and registered resources remain owned until
the effect has drained; live parameter updates reuse the existing effect.

PyroWave composes SDR capture, crop, cursor and the scanout LUT in Vulkan, then
passes GPU buffers to native CUDA TrueHDR and converts its packed BT.2020/PQ
output to the negotiated YUV planes. Explicit GPU ownership transfers and
timeline semaphores order Vulkan and CUDA work without intermediate CPU
waits (CPU fences remain the fallback when external semaphores are unavailable).
Unchanged cursor and LUT textures are reused. Desktop/non-matching app fallback stays in
the HDR stream, using SDR Brightness. The capture/encode GPU must be NVIDIA;
failed conversion ends the stream.

App tuning overrides global values. Contrast and saturation use the UI's
-100..100 scale, mapped to the SDK's 0..200 scale; middle gray and peak use
their existing units. Live edits use the same authenticated, UUID-scoped API
as Windows. Peak changes update the HDR metadata and request an IDR after the
existing slider-settling interval.

The pinned Linux SDK accepts peak values through 2000 but its packed output
saturates around 1000 nits. Above 1000, the adapter runs NVIDIA's native effect
at 1000 with a compensated middle gray, then uses a cached 1024-entry PQ lookup
on the same CUDA stream to restore the selected linear-light peak. At the SDK's
minimum gray of 10, the correction anchors the requested gray and expands the
remaining highlight range instead of brightening the entire image. Settings at
or below 1000 and the SDR fallback bypass this pass. This covers both NVENC
paths and PyroWave; metadata alone cannot correct the pixel values. See the
[peak investigation](rtx-hdr-peak-investigation.md) for measurements and limits.

On KDE, a bounded read-only helper observes the selected desktop's foreground
window. Steam identity, Lutris catalog ownership, or a custom command's
executable identifies the application. Desktop, unrelated windows, unknown or
stale observations, and live-disabled conversion use a neutral SDR-to-PQ
fallback with the SDR Brightness setting. Conversion remains opted into the
HDR pipeline so it can resume without restarting the stream. Custom launchers
whose foreground executable differs from the configured command cannot be
identified by this fallback matcher; prefer a Steam/Lutris catalog entry or
the game's executable. This observer does not activate or unminimize windows
and never runs on an encode thread. Linux has no NVIDIA App profile source;
Vibeshine's saved app/client/global settings are authoritative.

The CMake dependency is checksum-pinned and installed with its license files
under `/usr/lib/vibeshine/truehdr`. Missing libraries, failed SDK startup or
conversion errors terminate the affected initialization/stream with an error.
Installing the updated host and helper is necessary before testing the new
foreground operation. Restarting the host interrupts active streams.

`linux_truehdr_gpu` and `linux_truehdr_private_context_gpu` exercise actual
GL/CUDA conversion against the direct native SDK with identical input and
settings (with an independent CPU peak correction above 1000), measured highlight
and gray luminance, live tuning, fallback, invalid updates and teardown under both
primary and private CUDA contexts. `pyrowave_linux_truehdr_gpu` encodes and
decodes both chroma modes and ranges,
compares the converted color against an independent native SDK invocation,
and exercises LUT/cursor composition, live peak changes and SDR fallback.
Browser coverage checks Linux visibility,
non-NVIDIA exclusion, live updates and persistence in both interfaces. These
checks do not establish Windows pixel parity or client/display behavior; the
reference capture procedure remains in `tools/linux_truehdr/README.md`.

A manual `benchmark_pyrowave_linux` target measures the complete synchronous
encode call at 3840x2160 HDR 4:4:4 with a linear GBM DMA-BUF, including any
linear CPU-upload fallback, GPU waits, readback and packetization:

```sh
build/tests/benchmark_pyrowave_linux /dev/dri/renderD128 build/truehdr-runtime 240
```

Use `-` instead of the runtime path for neutral SDR-to-PQ. Record concurrent
GPU load and interleave baseline/candidate runs. These are wall timings for a
synthetic frame, not native inference latency or proof of gameplay/client
120 Hz cadence. The final packet payload checksum helps detect output changes.
