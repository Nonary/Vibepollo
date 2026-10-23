# NVIDIA TrueHDR on Linux

This runs NVIDIA's actual native TrueHDR implementation on Linux. It is a
standalone raw-frame converter and reference harness. Vibeshine's native
CUDA/GL streaming integration is documented in
[Linux RTX HDR](../../docs/linux/rtx-hdr.md); these tools remain independent
reference and benchmark utilities. No replacement tone-mapping algorithm is used.

The pinned NVIDIA `nvidia-vfx` 0.2.0.0 wheel contains VFX SDK 1.3.0 and
`libnvidia-ngx-truehdr.so.1.2.3`. The downloader verifies its SHA-256 and extracts
it into a new directory. It does not install packages, change service
configuration, or download a replacement driver. NVIDIA's license files are
retained in the extracted runtime.

Requires little-endian Linux x86-64, Python 3.12+, an NVIDIA RTX GPU, and a
working NVIDIA driver. The included ABI3 Python binding also worked with this
host's Python 3.14.8. The scripts use Python's standard library; no PyTorch,
CuPy, NumPy, or Wine installation is needed.

## Run

From the repository root:

```bash
python tools/linux_truehdr/fetch_runtime.py /tmp/truehdr-runtime
python tools/linux_truehdr/truehdr.py \
  --runtime /tmp/truehdr-runtime \
  --width 1920 --height 1080 \
  --output /tmp/chart.rgb10a2 \
  --save-input /tmp/chart.rgba \
  --report /tmp/chart.json
```

Output paths must be new files. To reuse an already downloaded wheel, pass
`--wheel /path/to/nvidia_vfx-0.2.0.0-...whl` to `fetch_runtime.py`; its digest
must match the pinned version.

Without `--input`, the converter generates a scene containing grayscale and
red ramps, neutral and saturated patches, and a bright highlight on a dark
background. `--save-input` saves the exact input bytes. With `--input`, supply
raw RGBA8 SDR frames, four interleaved bytes per pixel, top row first. `--frames`
sets the number of frames. `--input -` and `--output -` support pipes. Diagnostic
JSON is sent to stderr unless `--report` is provided. Timing measures the native
conversion plus CPU upload/readback, excluding chart generation, pixel
statistics and file I/O. This reference harness does not represent the latency
of a future GPU-only streaming adapter.

The output is little-endian packed `R10G10B10A2`: R in bits 0–9, G in 10–19,
B in 20–29, alpha in 30–31. RGB values are **BT.2020 primaries and ST.2084/PQ**.
They are already HDR10; do not apply an SDR transfer function or another
SDR-to-HDR expansion. FFmpeg calls this raw pixel format `x2bgr10le`.

Controls use NVIDIA SDK units:

| Flag | Range | Default |
|---|---|---|
| `--contrast` | 0–200 | 100 |
| `--saturation` | 0–200 | 100 |
| `--middle-gray` | 10–100 | 50 |
| `--peak` | 400–2000 nits | 1000 |
| `--debanding-off` | 0 or 1 | 0 |

The Windows overlay's contrast/saturation controls use -100 to 100; add 100
when setting these SDK controls, as Vibeshine's Windows implementation does.
Matching dial values alone does not prove equal output.

## Check the GPU

```bash
python tools/linux_truehdr/smoke_gpu.py --runtime /tmp/truehdr-runtime
python -m unittest discover -s tools/linux_truehdr -p 'test_*.py' -v
```

The GPU check verifies real output, repeated-frame determinism, and pixel
changes for peak, contrast, saturation and middle gray. It also exercises the
debanding-off mode; this synthetic chart can produce identical pixels in both
debanding modes, so no debanding quality claim follows from that check.

## Measure resident GPU conversion

```bash
python tools/linux_truehdr/benchmark_gpu.py --runtime /tmp/truehdr-runtime \
  --output /tmp/truehdr-resident-timing.json
```

`TrueHDR.run_gpu()` enqueues conversion between the persistent GPU input and
output buffers. It performs no CPU frame allocation, upload, or readback. A
capture/encoder adapter can order its producer and consumer on the CUDA stream.
The caller must retain the buffers until GPU work completes. This is separate
from `convert()`, which intentionally uploads and downloads reference pixels.
Neither method is integrated into Vibeshine's capture loop yet.

On the RTX 4090/615.71.09, with debanding enabled, 100 samples after 10 warmup
frames gave these results for the synthetic chart:

| Resolution | GPU event median | GPU event p95 | 100-frame batch wall time per frame |
|---|---:|---:|---:|
| 1280×720 | 0.205 ms | 0.209 ms | 0.318 ms |
| 1920×1080 | 0.270 ms | 0.275 ms | 0.493 ms |
| 3840×2160 | 0.740 ms | 0.745 ms | 1.378 ms |

GPU-event spans measure an individually submitted evaluation on the effect
stream. The batch includes submission/runtime scheduling overhead and one final
CPU wait. These are different measurements; the GPU event median alone is not
an end-to-end streaming latency claim. Individual calls with a blocking CPU
wait had a 4K wall median of 2.084 ms. GPU clocks, contention and scene content
can affect results. Output after both resident and batched execution matched
the synchronous reference byte-for-byte.

The earlier 10.6–15.5 ms measurements included CPU buffers and round-trip frame
transfers in `convert()` and must not be used as the cost of the GPU effect.
The resident API preserves the same native algorithm and debanding settings.

## Verify Windows values

`windows_reference.cpp` runs the existing Vibeshine Windows TrueHDR shim on
one exact RGBA8 frame and saves the borrowed output as owned, tightly packed
RGBA16F scRGB. Build and run on Windows in an x64 MSVC developer prompt:

```bat
cl /std:c++17 /EHsc tools\linux_truehdr\windows_reference.cpp /link d3d11.lib dxgi.lib
windows_reference.exe 1920 1080 chart.rgba windows.scrgb-f16 100 100 50 1000
```

Put the pinned `vibeshine_truehdr.dll` and `nvngx_truehdr.dll` beside the EXE.
The capture tool reports failures rather than substituting SDR. It has not
been compiled or run on native Windows in this investigation.

Compare the Linux frame with the Windows capture:

```bash
python tools/linux_truehdr/compare.py chart.rgb10a2 windows.scrgb-f16 \
  --reference-format scrgb-f16
```

The comparison converts Windows linear Rec.709 scRGB (1.0 = 80 nits) to linear
Rec.2020, applies PQ, and rounds to 10-bit codes before comparing every RGB
channel. Default tolerance is zero. `--max-code-error` permits an explicit
nonzero tolerance; it must not be treated as proof of exact equality. Alpha
is excluded. An RGB10A2 reference can be compared directly without conversion.

Use identical SDR bytes, image dimensions, dial settings, debanding mode,
frame history, runtime/model version, and driver/GPU identity. A passing
frame establishes only that frame's normalized RGB values. The Windows shim
is a useful reference for Vibeshine's existing Windows path; additional
reference captures are needed to establish parity with the NVIDIA App's
game filter, including any capture/display processing it performs.

## Current evidence

See [INVESTIGATION.md](INVESTIGATION.md) for artifact identities, observed
GPU results and remaining work. Exact Windows pixel parity is still unverified.
