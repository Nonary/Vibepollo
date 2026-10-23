# Linux RTX HDR peak investigation, 2026-10-22

The reported plateau is reproducible in NVIDIA's standalone runtime, before
the source Vibeshine investigation's capture, encoding, HDR metadata or client display. A 512x288 chart
with neutral ramps and white highlights produced these decoded BT.2020/PQ
luminances at default contrast/saturation and middle gray 50:

| Requested peak | Raw SDK maximum (nits) |
|---|---:|
| 400 | 398.08 |
| 650 | 647.40 |
| 800 | 796.27 |
| 1000 | 996.59 |
| 1200 | 998.93 |
| 1400 | 998.93 |
| 1600 | 998.93 |
| 2000 | 998.93 |

These are measured pixel luminances, not measurements of a physical display.
The gray-128 patch stayed at 51.21 nits throughout. Values above 1000 changed
some output codes without increasing the maximum, explaining why the slider
could appear to have little effect. No pixel exceeded 1020 nits.

## Binary evidence

The target is the pinned `nvidia-vfx` 0.2.0.0 runtime (VFX SDK 1.3.0), running
on RTX 4090 with NVIDIA 615.71.09. REA used the Ghidra provider. Local complete
bundles and raw frame/sweep artifacts are retained in the ignored build folder
`build/rtx-hdr-peak-investigation/`, not shipped with the application.

| Target | SHA-256 |
|---|---|
| `libnvVFXTrueHDR.so` | `7851e321ba2c31f89314b872cd40b1075122832c1d2d57cc901a36222dbd20a6` |
| `libnvidia-ngx-truehdr.so.1.2.3` | `2ec41bc1a5664c44f2a8a97ab59e60890ab3becda143b753337d12588c993688` |

Finding ledger:

- **Established statically:** adapter `SetU32` at `0x105420` accepts and stores
  luminance 400..2000 unchanged. REA Evidence
  `ev_76c6a230c03f07323fe44b07222bb54efc0c900e426320085b41735baf813e00`.
- **Established statically:** adapter `EvaluateNGXFeature` at `0x1058a0`
  forwards that stored value as `TrueHDR.MaxLuminance`. Evidence
  `ev_613032fc55dfecaa61d5027dc9a44891a189d6dded53aab00eec5a1a3db2ab9f`.
- **Established statically, with decompiler type-recovery limitations:** NGX
  evaluation at `0x1200d0` clamps the supplied parameter to 400..2000,
  converts it to float and places it in the dispatch parameter block. Its
  default is 1000, but supplied higher values overwrite that default. Evidence
  `ev_217b8a078a1abe7e53933782b3d5fdbb6ba418aeb5fb37478a08582edad9bed0`.
- **Established in source:** Linux forwarded the requested SDK peak and copied
  its packed output directly into GL/Vulkan processing. Windows already used
  SDK peak 1000, compensated middle gray and linear-light output gain.
- **Established by execution:** the raw Linux output plateaus near 999 nits
  on this chart. This localizes the ceiling to NVIDIA's downstream evaluation
  or output conversion, rather than a rejected setting or our HDR metadata.
- **Unresolved:** the exact GPU instruction/model stage causing the ceiling.
  Host-side dispatch has indirect calls and opaque GPU processing. Extracted
  embedded CUDA postprocessing kernels are not sufficient to identify the
  active clipping stage. Static analysis does not prove which instructions
  executed. Windows/NVIDIA App pixel parity and other runtime versions remain
  unverified. The Ghidra bridge used 12.1.2 in its supported 12.1.x range;
  its separately verified version is 12.1.4.

## Correction and validation boundary

Above 1000, keep NVIDIA's effect at 1000, lower its middle gray by the requested
peak ratio (rounded to SDK integer units), and apply linear-light gain to its
PQ output. The lookup decodes PQ, scales nits, re-encodes PQ and rounds to
10 bits. This is output compensation, not a recovered NVIDIA algorithm or a
claim of Windows pixel equality. The existing SDK effect and debanding remain
in use. Contrast and saturation are unchanged.

When gray compensation would fall below the SDK minimum 10, uniform gain
would brighten midtones (gray 10 would become approximately 20 at peak 2000).
For that case only, map black and native gray to black and requested gray,
then linearly expand the interval from native gray to 1000 onto requested gray
to requested peak. This continuous monotonic per-channel correction avoids
raising black or doubling low middle gray; its changed highlight contrast and
possible color-ratio changes are a deliberate bounded tradeoff at the gray
floor. It does not reproduce an undocumented NVIDIA filter.

The persistent table is updated only with tuning changes. The packed frame
stays on the GPU and the lookup kernel runs on the SDK stream, ahead of both
GL and PyroWave consumers. Below/equal 1000, no lookup kernel runs. SDR fallback
is unchanged. Native GL tests compare every pixel against independently
computed CPU PQ math, verify actual highlight/gray/black nits, restore settings
across the 1000 boundary, and test repeated frames and both CUDA context types.
PyroWave checks independent raw-SDK-plus-CPU references through actual encoding
and decoding, both chroma modes/ranges and both semaphore/fence policies.

Installed-host and physical client/display validation require a later install
and streaming test; the investigation and development tests do not restart or
replace the installed host.

## Candidate results

The native GL chart test (128x64 with neutral patches, 12 settling frames per
setting) measured the following through the production adapter:

| Peak setting | Highlight nits | Gray-128 nits | Baseline gray nits at peak 1000 |
|---|---:|---:|---:|
| 1200, gray 50 | 1184.40 | 51.35 | 50.82 |
| 1600, gray 50 | 1577.64 | 50.82 | 50.82 |
| 2000, gray 50 | 1974.05 | 50.82 | 50.82 |
| 2000, gray 10 | 1974.05 | 10.30 | 10.18 |

Black stayed below 0.01 nits. Tests compare every channel code, including the
colored gradient, against an independent CPU transform of native SDK output.
The raw sweep and this chart differ, so their maxima are not a paired
same-chart error metric. The SDK has temporal settling: single frames after
slider changes are not valid evidence of steady-state gray preservation.

The added lookup kernel alone, measured with CUDA events on persistent 4K
buffers (20 warmups, 100 samples, input reset before each timed span), took
0.020672 ms median and 0.021568 ms p95 on this host. The measurement excludes
input resets, SDK evaluation, encode and CPU transfers. It is not an
end-to-end streaming performance or 120 Hz cadence claim. The harness linked
the actual production library object and is retained alongside its JSON in
the ignored investigation directory.

The full RelWithDebInfo build with the production web bundles passed. Full
CTest completed in 91.57 seconds: 280 passed, one existing Playnite lifecycle
test skipped, no failures. Both native GL context tests passed again after
a test-only adjustment to keep actual/reference frame histories aligned.
PyroWave passed real encode/decode tests with both external semaphore and CPU
fence synchronization. No installation, service restart or physical client
validation was performed for this candidate.
