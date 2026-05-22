# Building
Sunshine binaries are built using [CMake](https://cmake.org) and requires `cmake` > 3.25.

## Building Locally

### Compiler
It is recommended to use one of the following compilers:

| Compiler    | Version |
|:------------|:--------|
| GCC         | 14+     |
| Clang       | 17+     |
| Apple Clang | 15+     |

### Dependencies

#### FreeBSD
> [!CAUTION]
> Sunshine support for FreeBSD is experimental and may be incomplete or not work as expected

##### Install dependencies
```sh
pkg install -y \
  audio/opus \
  audio/pulseaudio \
  devel/cmake \
  devel/evdev-proto \
  devel/git \
  devel/libayatana-appindicator \
  devel/libevdev \
  devel/libnotify \
  devel/ninja \
  devel/pkgconf \
  ftp/curl \
  graphics/libdrm \
  graphics/wayland \
  multimedia/libva \
  net/miniupnpc \
  ports-mgmt/pkg \
  security/openssl \
  shells/bash \
  www/npm \
  x11/libX11 \
  x11/libxcb \
  x11/libXfixes \
  x11/libXrandr \
  x11/libXtst
```

#### Linux
Dependencies vary depending on the distribution. You can reference our
[linux_build.sh](https://github.com/LizardByte/Sunshine/blob/master/scripts/linux_build.sh) script for a list of
dependencies we use in Debian-based, Fedora-based and Arch-based distributions. Please submit a PR if you would like to extend the
script to support other distributions.

##### KMS Capture
If you are using KMS, patching the Sunshine binary with `setcap` is required. Some post-install scripts handle this. If building
from source and using the binary directly, this will also work:

```bash
sudo cp build/sunshine /tmp
sudo setcap cap_sys_admin,cap_sys_nice+p /tmp/sunshine
sudo getcap /tmp/sunshine
sudo mv /tmp/sunshine build/sunshine
```

##### CUDA Toolkit
Sunshine requires CUDA Toolkit for NVFBC capture. There are two caveats to CUDA:

1. The version installed depends on the version of GCC.
2. The version of CUDA you use will determine compatibility with various GPU generations.
   At the time of writing, the recommended version to use is CUDA ~13.1.
   See [CUDA compatibility](https://docs.nvidia.com/deploy/cuda-compatibility/index.html) for more info.

> [!NOTE]
> To install older versions, select the appropriate run file based on your desired CUDA version and architecture
> according to [CUDA Toolkit Archive](https://developer.nvidia.com/cuda-toolkit-archive)

#### macOS
You can either use [Homebrew](https://brew.sh) or [MacPorts](https://www.macports.org) to install dependencies.

##### Homebrew
```bash
dependencies=(
  "boost"  # Optional
  "cmake"
  "doxygen"  # Optional, for docs
  "graphviz"  # Optional, for docs
  "icu4c"  # Optional, if boost is not installed
  "miniupnpc"
  "ninja"
  "node"
  "openssl@3"
  "opus"
  "pkg-config"
)
brew install "${dependencies[@]}"
```

If there are issues with an SSL header that is not found:

@tabs{
  @tab{ Intel | ```bash
    ln -s /usr/local/opt/openssl/include/openssl /usr/local/include/openssl
    ```}
  @tab{ Apple Silicon | ```bash
    ln -s /opt/homebrew/opt/openssl/include/openssl /opt/homebrew/include/openssl
    ```
  }
}

##### MacPorts
```bash
dependencies=(
  "cmake"
  "curl"
  "doxygen"  # Optional, for docs
  "graphviz"  # Optional, for docs
  "libopus"
  "miniupnpc"
  "ninja"
  "npm9"
  "pkgconfig"
)
sudo port install "${dependencies[@]}"
```

#### Windows

> [!WARNING]
> Cross-compilation is not supported on Windows. You must build on the target architecture.

First, you need to install [MSYS2](https://www.msys2.org).

For AMD64 startup "MSYS2 UCRT64" (or for ARM64 startup "MSYS2 CLANGARM64") then execute the following commands.

##### Update all packages
```bash
pacman -Syu
```

##### Set toolchain variable
For UCRT64:
```bash
export TOOLCHAIN="ucrt-x86_64"
```

For CLANGARM64:
```bash
export TOOLCHAIN="clang-aarch64"
```

##### Install dependencies
```bash
dependencies=(
  "git"
  "mingw-w64-${TOOLCHAIN}-boost"  # Optional
  "mingw-w64-${TOOLCHAIN}-cmake"
  "mingw-w64-${TOOLCHAIN}-cppwinrt"
  "mingw-w64-${TOOLCHAIN}-curl-winssl"
  "mingw-w64-${TOOLCHAIN}-doxygen"  # Optional, for docs... better to install official Doxygen
  "mingw-w64-${TOOLCHAIN}-graphviz"  # Optional, for docs
  "mingw-w64-${TOOLCHAIN}-miniupnpc"
  "mingw-w64-${TOOLCHAIN}-onevpl"
  "mingw-w64-${TOOLCHAIN}-openssl"
  "mingw-w64-${TOOLCHAIN}-opus"
  "mingw-w64-${TOOLCHAIN}-toolchain"
)
if [[ "${MSYSTEM}" == "UCRT64" ]]; then
  dependencies+=(
    "mingw-w64-${TOOLCHAIN}-MinHook"
    "mingw-w64-${TOOLCHAIN}-nodejs"
    "mingw-w64-${TOOLCHAIN}-nsis"
  )
fi
pacman -S "${dependencies[@]}"
```

##### WebRTC (optional, Windows only)
Sunshine can link against the libwebrtc C++ wrapper when `SUNSHINE_ENABLE_WEBRTC=ON`. The wrapper source is vendored as
the `third-party/libwebrtc` submodule, but you must build WebRTC separately and provide a staging directory that
contains `include/` and `lib/` (e.g., `libwebrtc.dll` and its import library). We use the `third-party/depot_tools`
submodule for `gclient`/`gn`.

###### Quickstart (recommended)
A helper script automates the full depot_tools / gclient / gn / ninja flow:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_mingw_webrtc.ps1
```

By default the script caches its working tree and output to `%LOCALAPPDATA%\Vibepollo\deps\libwebrtc\{src,out}`,
which is **shared across every Vibepollo build dir, worktree, and git checkout on the machine**. This means:

- Wiping `build/` does not destroy libwebrtc — the next CMake configure picks it back up.
- Switching git branches does not require rebuilding libwebrtc unless the WebRTC branch (`m125_release` etc.) actually changed.
- Multiple Vibepollo build dirs share one cached libwebrtc.

`cmake/dependencies/webrtc.cmake` looks at the same default location, so no `-DWEBRTC_ROOT=...` is needed after the
first build. To relocate the cache, set `VIBEPOLLO_DEPS_DIR=<path>` in your environment before invoking either the
script or CMake.

For finer control the script accepts overrides via `-BuildDir`/`-OutDir` parameters or the legacy
`WEBRTC_BUILD_DIR` / `WEBRTC_OUT_DIR` env vars (these still take precedence if set).

###### Manual build (advanced / first-time porting)

If you cannot use the helper script, the underlying steps are:

1. Create a checkout directory and add a `.gclient` that points to
   `https://github.com/webrtc-sdk/webrtc.git@m137_release` with `target_os = ['win']`.
2. Run `gclient sync`.
3. In `src`, add the libwebrtc sources (you can copy or link `third-party/libwebrtc` into `src/libwebrtc`).
4. Apply the audio patch:
   `git apply libwebrtc/patchs/custom_audio_source_m137.patch`
5. Update `src/BUILD.gn` to include `//libwebrtc` in `group("default")`.
6. Generate and build (adjust `GYP_MSVS_OVERRIDE_PATH` if Visual Studio is installed elsewhere; our local install is
   under `D:\Software\Visual Studio`):
   ```bash
   set PATH=D:\sources\sunshine\third-party\depot_tools;%PATH%
   set DEPOT_TOOLS_WIN_TOOLCHAIN=0
   set GYP_MSVS_VERSION=2022
   set GYP_GENERATORS=ninja,msvs-ninja
   set GYP_MSVS_OVERRIDE_PATH=D:\Software\Visual Studio
   set vs2022_install=D:\Software\Visual Studio
   set WINDOWSSDKDIR=D:\Software\WinSDK
   cd src
   gn gen out-debug/Windows-x64 --args="target_os=\"win\" target_cpu=\"x64\" is_component_build=false is_clang=true is_debug=true rtc_use_h264=true ffmpeg_branding=\"Chrome\" rtc_include_tests=false rtc_build_examples=false libwebrtc_desktop_capture=true" --ide=vs2022
   ninja -C out-debug/Windows-x64 libwebrtc
   ```
7. Stage the artifacts into a directory with `include/` and `lib/` subfolders. Either place them at the default
   shared cache location (`%LOCALAPPDATA%\Vibepollo\deps\libwebrtc\out`) so CMake finds them automatically, or
   point CMake at your staging directory with `-DWEBRTC_ROOT=<path>`. Copy `libwebrtc.dll` and `libwebrtc.dll.lib`
   into `lib/`.
8. Configure Sunshine with `-DSUNSHINE_ENABLE_WEBRTC=ON`. If CMake still fails to find libwebrtc, pass
   `WEBRTC_INCLUDE_DIR` and `WEBRTC_LIBRARY` explicitly.

To create a WiX installer, you also need to install [.NET](https://dotnet.microsoft.com/download).

For ARM64: To build frontend, you also need to install [Node.JS](https://nodejs.org/en/download)

### Clone
Ensure [git](https://git-scm.com) is installed on your system, then clone the repository using the following command:

```bash
git clone https://github.com/ClassicOldSong/Apollo.git --recurse-submodules
cd Apollo
mkdir build
```

### Build

```bash
cmake -B build -G Ninja -S .
ninja -C build
```

> [!TIP]
> Available build options can be found in
> [options.cmake](https://github.com/LizardByte/Sunshine/blob/master/cmake/prep/options.cmake).

### Package

@tabs{
  @tab{FreeBSD | @tabs{
    @tab{pkg | ```bash
      cpack -G FREEBSD --config ./build/CPackConfig.cmake
      ```}
  }}
  @tab{Linux | @tabs{
    @tab{deb | ```bash
      cpack -G DEB --config ./build/CPackConfig.cmake
      ```}
    @tab{rpm | ```bash
      cpack -G RPM --config ./build/CPackConfig.cmake
      ```}
  }}
  @tab{macOS | @tabs{
    @tab{DragNDrop | ```bash
      cpack -G DragNDrop --config ./build/CPackConfig.cmake
      ```}
  }}
  @tab{Windows | @tabs{
    @tab{Installer | ```bash
      cpack -G WIX --config ./build/CPackConfig.cmake
      # note: MSI packaging requires WiX Toolset v3 to be installed (e.g. `choco install wixtoolset`)
      ```}
    @tab{WiX Installer | ```bash
      cpack -G WIX --config ./build/CPackConfig.cmake
      ```}
    @tab{Portable | ```bash
      cpack -G ZIP --config ./build/CPackConfig.cmake
      ```}
  }}
}

### Remote Build
It may be beneficial to build remotely in some cases. This will enable easier building on different operating systems.

1. Fork the project
2. Activate workflows
3. Trigger the *CI* workflow manually
4. Download the artifacts/binaries from the workflow run summary

<div class="section_buttons">

| Previous                              |                            Next |
|:--------------------------------------|--------------------------------:|
| [Troubleshooting](troubleshooting.md) | [Contributing](contributing.md) |

</div>

<details style="display: none;">
  <summary></summary>
  [TOC]
</details>
