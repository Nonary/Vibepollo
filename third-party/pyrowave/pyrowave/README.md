# PyroWave

PyroWave is an intra-only video codec (practially speaking a still-image codec)
that is optimized for extremely fast GPU encode (< ~0.1 ms encode and decode at 1080p, < ~0.2 ms at 4K).
It is fully implemented in Vulkan compute shaders.

The targeted bit-rates are quite high (~200+ mbit/s) and the intended use case is
local network game streaming over ethernet with absolute minimum latency where bandwidth is less of a concern.

It is integrated as part of my [pyrofling](https://github.com/Themaister/pyrofling) project.

It is similar in scope to my [master thesis from 2014](https://ntnuopen.ntnu.no/ntnu-xmlui/handle/11250/2400689),
except the use case now is game streaming instead of adaptation of raw video to ethernet links.

## Overview

### Colorspace

Currently implemented YCbCr 4:2:0 and 4:4:4.

### Wavelets

The images are transformed with the Discrete Wavelet Transform using CDF 9/7 filter.
This is basically the exact same as JPEG2000.

### Exact rate control

The encoder can target exact maximum bitrate for an encoded image.

### Trivial "entropy" coding

The encoding of coefficients is trivial compared to normal image codecs,
and is responsible for a large increase in bit-rate, especially at higher compression ratios.
This simplicity massively improves encode/decode performance, since it's extremely parallelizable.
It also makes it possible to do a single-pass exact rate control for an entire image.

It should be possible in theory to add a more proper entropy coder to the encoded bit-planes to achieve somewhat
competent compression, but that has not been explored and is out of scope.
High Throughput JPEG2000 is likely a good place to look for that anyway.

### Robustness against packet loss

Being intra-only and encoding 64x64 blocks of coefficients in isolation ensures great recovery against packet loss.
PyroWave has been battled tested over long distance streaming over fiber links.

### Bitstream definition

See [docs/bitstream.md]()

## Building

PyroWave is intended to be built alongside PyroFling with Granite in the normal case.

### Standalone C API

NOTE: This API is still under development and the API/ABI is not yet quite stable.

A small portion of Granite needs to be checked out.

```
bash checkout_granite.sh
```

Build normally with CMake and a C API is installed.

```
$ mkdir build
$ cd build
$ cmake .. -DCMAKE_INSTALL_PREFIX=output -DCMAKE_BUILD_TYPE=Release -G Ninja
$ ninja install

[0/1] Install the project...
-- Install configuration: "Release"
-- Installing: ...../build/output/include/pyrowave/pyrowave.h
-- Installing: ...../build/output/lib/libpyrowave-shared.so.0.0.0
-- Installing: ...../build/output/lib/libpyrowave-shared.so.0
-- Installing: ...../build/output/lib/libpyrowave-shared.so
-- Installing: ...../build/output/share/pyrowave-shared/cmake/pyrowave-sharedConfig.cmake
-- Installing: ...../build/output/share/pyrowave-shared/cmake/pyrowave-sharedConfig-release.cmake
-- Installing: ...../build/output/share/pkgconfig/pyrowave-shared.pc
```

The build is tested on Linux, MinGW, msys2 and MSVC.
See `pyrowave-c-test` which unit tests the shared C API.
That test also serves as a basic user guide for the API.

`build-steamrt.sh` builds against the Sniper SDK and is also supported.

#### Android standalone build

This assumes that NDK is installed somewhere.

```
# Replace as needed. Any NDK should work in theory, but this is the one I checked with.
$ NDK_VERSION=29.0.14206865

$ cmake .. -DCMAKE_TOOLCHAIN_FILE=$ANDROID_HOME/ndk/$NDK_VERSION/build/cmake/android.toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DANDROID_ABI=arm64-v8a \
    -G Ninja -DCMAKE_INSTALL_PREFIX=output
$ ninja install
```

### Local development and CLI

For the sample and test applications in this repo however, check out
the full https://github.com/Themaister/Granite before invoking CMake.
Build with `-DPYROWAVE_DEVEL=ON` to get the "full" build.

```shell
git clone --depth 1 --recursive --shallow-submodules https://github.com/Themaister/Granite Granite
```

#### Basic encoder/decoder CLI

The basic CLI takes a y4m and dumps out a raw bitstream. This is just intended for local testing.

```shell
pyrowave-encode test.y4m out.wave 400000
```

This encodes a raw bitstream where each frame consumes maximum 400 KB.
To decode back to y4m:

```shell
pyrowave-decode out.wave out.y4m
```

