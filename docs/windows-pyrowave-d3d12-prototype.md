# Experimental PyroWave D3D12 conversion

`SUNSHINE_EXPERIMENTAL_PYROWAVE_D3D12` is an OFF-by-default build option for
native Windows qualification of a conversion-only backend. Enabling it asks
Windows PyroWave sessions to convert on a D3D12 **compute** command queue on
the capture adapter. It does not add native AMF D3D12 input, move DDA/WGC
capture copies onto a compute queue, or establish a performance improvement.

The supported source is a single-mip, single-layer, non-MSAA Texture2D with
an ordinary matching SRV, in BGRA8 UNORM, RGBA8 UNORM, or RGBA16 FLOAT. All
existing transfer variants and the existing conversion shader are retained.
The feature remains OFF by default. With an explicit runtime request, the
core first registers a source-retention sink for the full adapter LUID. D3D12
initialization failure keeps a D3D11-only sink; failure to allocate or
register that sink fails core creation before capture begins. Unsupported
sources use the existing D3D11 conversion path. The core reports whether the
D3D12 backend was selected so qualification cannot silently measure fallback.

The first prototype intentionally serializes retirement:

1. Hold the capture keyed mutex, copy the source into a private NT-shared
   D3D11 snapshot, signal a dedicated producer fence, and flush D3D11.
2. The D3D12 compute queue waits for that producer value. Its command list
   reads the snapshot and writes the existing three shared PyroWave planes,
   returning every accessed resource to COMMON state.
3. The queue signals the existing Vulkan-imported conversion fence. The CPU
   waits up to two seconds for retirement before releasing capture ownership
   and passing the planes to the existing synchronous Vulkan encoder.

The successful return retires the sole allocator, descriptors, and snapshot
even if the caller subsequently skips encode because its byte budget is too
small. After a queue ordering error or timeout the session fails; it does
not overwrite potentially live planes through D3D11 fallback. Unfinished
backend objects retain separate producer and consumer obligations. A valid
covering D3D11 producer ticket must be observed complete; a failed producer
Signal with no covering ticket permanently retains the exact source image,
original and opened COM objects, and acquired encode-device keyed mutex. A
D3D12 tail, timeout, device removal or sentinel never discharges that unknown
producer obligation. A failed capture release also permanently retains the
source without retry. A successfully queued consumer-retirement signal can
cover the consumer tail. The existing removed-device policy remains for a
consumer whose completion is no longer marked unknown; native teardown effects
are not established here. An explicitly unknown consumer tail stays retained
even after device removal. Only one D3D12 conversion backend is admitted per full
adapter LUID; other opted-in sessions may hold fallback-only sinks. The
per-LUID frame gate serializes their capture and fallback ownership. A source
whose release status is unknown permanently blocks that full LUID. Retained
objects are checked when another sink is registered.

Nonblank D3D12 copies require an explicit caller-scoped keyed-writer lease.
The host supplies the existing image control block from the exclusively async
PyroWave pool: pool selection requires use_count()==1, and actual DDA/WGC pixel
writers acquire the capture resource mutex. Standalone callers must provide a
real source owner and require every future writer of that exact resource to
acquire its mutex; a generic shared_ptr or arbitrary raw writer is insufficient.
The node pins exact original and opened resources independently of image-field
replacement, cache/session destruction and metadata changes. The production
PyroWave caller binds original and opened resources through `open_image()`'s
capture-texture cache key and `encoder_texture_handle`; the generic source
record does not prove that relationship for arbitrary callers. In an opted-in
session, missing or unproven leases fail before acquisition. The sink takes
the unarmed exact mutex pin before `AcquireSync` and arms it only on `S_OK`.
Unsupported D3D12 preparation keeps the acquired lease in the sink through
D3D11 fallback. All D3D12 command setup and Close precede the possible-copy
mark. After that mark, failures are terminal and only scalar operation/status
metadata is recorded; there is no fallback or diagnostic allocation/callback
in that submission path.

Collection keeps a completed node admitted while it attempts capture release and
drops source owners outside the registry lock. Failed release poisons admission;
successful release permits detachment and final destruction outside the lock.
Permanent unknown retention is bounded to the existing single admitted generation
and its one source per full adapter LUID, with an explicit availability cost.
Native API-internal effects, cross-thread COM behavior, failure triggers and fence
query liveness remain unqualified; this source protocol does not establish a
healthy-device failed Signal outcome or full backend acceptance.

Before acquisition in an explicitly opted-in session, the sink pins the
caller-provided image owner, original capture texture/mutex, opened SRV,
opened texture, and the exact keyed-mutex interface that will be acquired.
The production PyroWave caller binds original and opened resources through
`open_image()`'s capture-texture cache key and `encoder_texture_handle`; the
generic source record does not prove that relationship for arbitrary callers.
The per-LUID gate serializes same-adapter capture, conversion, and fallback
release. Contention skips a frame. Unsupported D3D12 preparation keeps the
acquired lease in the sink through D3D11 dispatch and unbind, then releases it
before signaling or flushing the output. A failed release is attempted once,
returns a failed frame before output publication, retains the exact disarmed
mutex plus all source/fallback owners, and permanently poisons that full LUID.
No later same-LUID sink may acquire after poison.

The configured OFF path keeps the existing local D3D11 guard behavior. These
retention guarantees describe an explicitly requested, build-enabled
experimental session. They do not establish native API failure frequency,
all-caller lease validity, performance, feature acceptance, or deployment.

This conservative CPU wait and longer capture-mutex lifetime are prototype
limits. Changing them requires an explicit per-frame retirement contract,
not just removal of the wait. The separate AMF work needs a DX12 surface
contract, per-output fences and surface-observer lifetime accounting.

With a native Windows toolchain and working PyroWave interop, configure with
`-DSUNSHINE_EXPERIMENTAL_PYROWAVE_D3D12=ON`, build
`test_pyrowave_windows_gpu`, and run CTest's `pyrowave_windows_gpu` and
`pyrowave_windows_d3d12_gpu`. The second test requires actual D3D12 selection.
It checks real keyed-mutex sharing from a separate D3D11 producer, changing
frame values and snapshot extents, R8/R16 planes, 4:2:0/4:4:4, quarter-turn
letterboxing, blank frames, conversion-before-budget-skip retirement,
single-generation admission, and unsupported-source fallback through real
encode/decode calls. Decoding is to eight-bit CPU planes, so this does not
establish bit-identical R16 output or all HDR transfer functions.

Before shipping an enabled build, run D3D12 debug-layer and GPU validation,
raw-plane parity for SDR/scRGB/PQ/SDR-to-PQ and all supported formats,
alternating-frame/repeat detection under load, device-loss/timeout and
shutdown/reinitialization tests, and latency plus game-frame-time comparisons
on Radeon and fallback adapters. These are native qualification requirements;
a Linux source check or a successful fixture build cannot establish them.
