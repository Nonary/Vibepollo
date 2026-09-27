# PyroWave on Linux

Linux builds enable `SUNSHINE_ENABLE_PYROWAVE` by default. Building requires a
Vulkan shader compiler (`glslc` or `glslangValidator`) in addition to the normal
Linux dependencies. No change to the PyroWave bitstream or client protocol is
required; see [the wire contract](../pyrowave-protocol.md).

The encoder imports RGB DMA-BUF frames with explicit DRM modifiers into Vulkan,
waits for producer completion, and runs scaling, color conversion, cursor
composition, and wavelet encoding on the GPU. KMS supplies the exact capture DRM
node and crop offsets, including the physical renderer behind managed virtual
outputs. The negotiated matrix, range, chroma subsampling, and depth apply to
both SDR and HDR sessions. scRGB input is converted to SDR or PQ as needed;
native 10-bit HDR KMS input is already PQ. Ten-bit SDR remains SDR.

BGRA system-memory capture is also accepted, with an upload cost. CUDA-only
NvFBC capture is unsupported and suppresses PyroWave advertisement. Unsupported
DMA-BUF formats/modifiers fail explicitly rather than interpreting tiled memory
as linear pixels.

The existing `pyrowave` configuration switch controls advertisement. Startup
probing performs a real GPU conversion, encode, and readback. Session frames use
the same record/length-prefixed framing, bitrate budget, and critical FEC policy
as Windows. A passing probe does not verify a capture modifier or client playback.

## Validation

The normal build produces `build/tests/test_pyrowave_linux_gpu`. It exercises
the production encoder and the vendored decoder, checks decoded colors, and
covers 4:2:0/4:4:4, full/limited range, 8/10-bit settings, SDR-to-PQ conversion,
scaling, flipped frames, gamma LUTs, cursor blending, and both packet boundaries.
It also checks coexistence with direct libvulkan calls in the same executable.
The CTest case skips when no Vulkan device is available.

```sh
build/tests/test_pyrowave_linux_gpu
# Use the actual render node to test GBM DMA-BUF export and Vulkan import:
build/tests/test_pyrowave_linux_gpu /dev/dri/renderD128
# Optional native 10-bit source; requires GBM allocation/mapping support:
build/tests/test_pyrowave_linux_gpu /dev/dri/renderD128 10
ctest --test-dir build --output-on-failure -j10
```

These are local GPU tests. After installing with the repository's normal Linux
procedure, validate an actual PyroWave client session, including desktop/game
motion, cursor placement, HDR color, and disconnect/reconnect. Installation
interrupts active streams and requires the normal switchover warning.
