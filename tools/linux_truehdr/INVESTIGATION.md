# Linux RTX HDR investigation

2026-10-21. Changes are confined to `tools/linux_truehdr`; no existing capture,
encoder, display, configuration or frontend files have been changed by this work.

## Proven

- Host GPU: NVIDIA GeForce RTX 4090; driver 615.71.09. This was checked outside
  the filesystem/device sandbox. A sandbox-only NVIDIA-SMI failure did not
  indicate a broken host driver.
- NVIDIA's native Linux TrueHDR effect initializes and converts actual SDR
  RGBA8 frames to varying BT.2020/PQ RGB10A2 pixels. Both the NVIDIA Python
  interface and the direct VFX C API were exercised.
- The reusable converter retains the effect, GPU input/output images and CUDA
  stream across frames. It checks each VFX status, synchronizes before releasing
  CPU buffers, and returns owned output bytes.
- Seven GPU cases passed: baseline, peak 400, peak 2000, contrast 0,
  saturation 0, middle gray 10, and debanding off. Repeated identical frames
  produced identical bytes within each case. All five tone/color dial changes
  changed the output. Debanding on/off were identical for this chart.
- Three 1080p frames had identical output hashes and RGB codes from 0 to 769.
  Measured conversion/upload/readback times were 5.462, 4.921 and 3.681 ms.
- Three 4K frames also produced identical output hashes, with codes 0 to 769.
  Conversion/upload/readback took 15.478, 14.827 and 10.648 ms. These are
  three-frame synthetic measurements, not a sustained gameplay benchmark.
- Eight CPU comparison tests passed, including known PQ values, normalization
  from Windows scRGB, malformed input rejection, and detection of a single
  erroneous channel among otherwise identical pixels.

## Resident-path performance correction

The original 4K measurement (10.648–15.478 ms) was the CPU reference harness,
including host buffer allocation, upload and readback. It was not comparable
to the Windows GPU capture/encode path.

Added `TrueHDR.run_gpu()` to submit the effect asynchronously using persistent
GPU buffers, plus `benchmark_gpu.py` with CUDA event timing, separate host
submission/completion timing, and a batch with one final wait. At 4K with
**debanding still enabled**, 100 warmed samples measured 0.740 ms median GPU
event span and 0.745 ms p95. The 100-frame batch measured 1.365 ms GPU time and
1.378 ms wall time per frame. Individually waiting for each call gave 2.084 ms
median wall completion. The batch overhead/difference has not been attributed
to a specific runtime or scheduling mechanism; report it rather than claiming
0.740 ms is the complete sustained frame cost.

1080p GPU event median was 0.270 ms and batch wall cost 0.493 ms/frame. The
same tests with debanding disabled measured 0.409 ms median GPU event time and
0.758 ms batch wall time/frame at 4K. Debanding remains enabled for the primary
performance comparison. Every resident and batched output compared byte-for-byte
equal to its synchronous reference. This is a synthetic chart measurement,
not Windows parity or a live gameplay/streaming benchmark.

Full measurements: `/tmp/vibeshine-rtx-hdr-investigation/resident-benchmark-with-batch.json`.

## Artifact identity

| Artifact | SHA-256 |
|---|---|
| NVIDIA Linux VFX wheel 0.2.0.0, cp312 ABI3 x86-64 | `0252866a7b798a6f92c8543443d88f716e8c5bbc6a7ddd77a2754728dada9fa8` |
| Linux `libnvidia-ngx-truehdr.so.1.2.3` | `2ec41bc1a5664c44f2a8a97ab59e60890ab3becda143b753337d12588c993688` |
| Vibeshine Windows TrueHDR runtime v1.0.0 ZIP | `1e1e5c4d231900fec78375f0ced643bb3e7dafa5d6e0325474e4412a30ea4abf` |
| Windows `nvngx_truehdr.dll` | `9a80575f247190c05fe80eac0c4baa1d0d4d932348f26808310b5ec4bf9eeb4b` |

Sources:

- [NVIDIA's Python package](https://pypi.org/project/nvidia-vfx/)
- [NVIDIA TrueHDR filter interface](https://docs.nvidia.com/maxine/vfx/latest/Filters/TrueHDR.html)
- [Published image C ABI](https://docs.nvidia.com/maxine/vfx/latest/API/Reference/Structures.html)
- [NvCVImage enumerations](https://docs.nvidia.com/maxine/nvcvimage/latest/enumerations.html)
- [Existing Vibeshine Windows runtime](https://github.com/Nonary/vibeshine_truehdr_runtime/releases/tag/v1.0.0)

## Static analysis and its limits

REA/Ghidra inspected the Windows DLL. Its CUDA CreateFeature entry has a real
implementation and checks a driver version threshold; CUDA EvaluateFeature
sets a CUDA context and calls the shared evaluation path. This is static
control-flow evidence, not a Windows runtime test.

- Windows overview: `ev_90848f74fc251335e4916f9847d1b87b073ebf616cf1fc94d6a195173f315b58`
- CUDA CreateFeature: `ev_defccb6270235efa49308cfec8e457b19fef07f66e07cc142dd188f3b3e1e6de`
- CUDA EvaluateFeature: `ev_a3462ca35448c5248910f0844a3f8832204ace6c62a7e556b90d3192fde84630`
- Shared evaluation, including dial mapping and conversion stages:
  `ev_3b5739d3237afe08abc4fc4636db86cfb6f0af72f241a1f51e8b53201358ec3e`

The native ELF was opened separately. Its legacy `NVSDK_NGX_CUDA_Init` export
returns failure (`ev_7aff19a3cd894a6cafaeb3945f7ee0fa8c44bd10e9bff2f7cca51166e023d1f5`).
That does not make native TrueHDR unavailable: the VFX route was subsequently
shown to work through actual frame conversion.

A separate bounded ELF-section comparison found 57 of 65 selected embedded
Linux CUDA text/constant sections also present byte-for-byte in the Windows
DLL, comparing section name, architecture tag and SHA-256. The unmatched
sections were SM89 TrueHDR stages. This supports shared implementation
components; it does not establish full model identity or output equivalence.
Neither static analysis nor equal slider names proves parity.

Retained session artifacts are under `/tmp/vibeshine-rtx-hdr-investigation/`:
`windows-evidence.json`, `linux-evidence.json`, `cuda-section-comparison.json`,
`gpu-smoke.json`, raw input/output frames and their JSON manifests. These are
local investigation artifacts, not committed or installed NVIDIA binaries.

## Remaining work

- Run `windows_reference.cpp` on native Windows with the same SDR inputs and
  dials, then compare all normalized RGB values. Native Windows build/run
  validation remains unavailable in this session.
- Obtain NVIDIA App game-filter references if parity with that exact path is
  required in addition to Vibeshine's existing Windows shim.
- Extend the reference corpus to representative game scenes, spatial context,
  UI/highlights, dark gradients, saturated colors, motion and changing settings.
- Add the verified effect to Vibeshine's GPU capture/encode loop, preserving
  source SDR state, source/stream metadata separation, cursor handling, color
  LUT order, CUDA context/stream ownership and runtime failure behavior. The
  current tool performs CPU upload/readback and is not this integration.
- Validate actual encoded HDR10 metadata and client display behavior before
  calling a streaming implementation complete. No host deployment occurred.
