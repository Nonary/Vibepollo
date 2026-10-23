# Driver frame push transport prerequisite

This standalone target compiles the production portable host session, bounded
transaction state machine, and actual libvirtualdisplay registration server.
It does not open a control device or enable a capture backend. The Windows
application has a separate `SUNSHINE_ENABLE_FRAME_PUSH_CAPTURE` build option,
which defaults off. With the option enabled, the explicit `capture = idd-push`
selector probes only a Sunshine virtual-display target, validates its active
mode and adapter, allocates a bounded ring, and requires the driver's
keyed-mutex-ring capability with four slots. The selector returns the native
capture adapter only after live registration succeeds; any failed prerequisite
takes the WGC fallback. The driver reports frame-ring capability only when its
matching experimental producer option is enabled.

Supply an exact libvirtualdisplay checkout containing the prior #626 prerequisite
and this candidate, plus local googletest sources. No network fetch is performed.

```sh
cmake -S tests/frame_push_transport -B /tmp/frame-push-tests -G Ninja \
  -DFRAME_TRANSPORT_DRIVER_ROOT=/absolute/path/to/libvirtualdisplay \
  -DFRAME_TRANSPORT_GTEST_ROOT=/absolute/path/to/googletest
cmake --build /tmp/frame-push-tests --parallel 2
ctest --test-dir /tmp/frame-push-tests --output-on-failure --parallel 2
```

On Windows, this target also compiles the native D3D11 ring allocator, importer
and thread-pool I/O adapter against real SDK headers. Linux only compiles their
empty platform guards. Portable success does not establish native compilation,
WDK/IddCx callback correctness, WDF request identity, process access permissions,
GPU ordering or pixel validity. Physical Windows/GPU validation is still
required before enabling the options in a distributed build.

An asynchronous encoder must retain the returned private frame token through its
last GPU access. At most two private output textures are preallocated per ring;
exhaustion drops a frame. Native generation fencing, current-mode/HDR/cursor
correctness and the native old/new compatibility matrix remain qualification
requirements.
