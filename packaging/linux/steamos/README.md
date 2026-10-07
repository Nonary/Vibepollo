# SteamOS user bundle

This experimental profile supports stock Gamescope SDR capture and an optional
[patched Gamescope HDR10 path](gamescope/README.md). See [AUDIT.md](AUDIT.md) for
validation evidence, the SteamOS Mesa prerequisite and remaining release gates.
Gaming Mode routes normal game sessions to the existing Gamescope scene,
including sessions configured for per-client/shared virtual displays or a
Desktop Mode monitor. It preserves those settings for Desktop Mode and leaves
Gaming Mode's physical layout alone. Independent Gaming Mode virtual monitors
are not created. HDR is negotiated with the running compositor; selecting a
virtual display does not force SDR.

This packaging profile runs Vibepollo as the logged-in SteamOS user and leaves
the read-only root filesystem untouched. It is separate from Vibepollo's
machine-service Linux package, which remains the right deployment for managed
KDE hosts and the optional `vibeshine_drm` virtual-display driver.

## Build, payload layout, and installation

Configure the dedicated rootless profile from the repository. The install
prefix is only the staging directory; runtime assets are deliberately compiled
as bundle-relative paths.

```bash
cmake -S . -B build-steamos -G Ninja \
  -D CMAKE_BUILD_TYPE=Release \
  -D CMAKE_INSTALL_PREFIX="$PWD/build-steamos/payload" \
  -D SUNSHINE_BUILD_STEAMOS=ON \
  -D BUILD_DOCS=OFF \
  -D SUNSHINE_ENABLE_TRAY=OFF \
  -D SUNSHINE_SYSTEM_VULKAN_HEADERS=ON \
  -D SUNSHINE_ENABLE_CUDA=OFF \
  -D SUNSHINE_ENABLE_DRM=OFF
cmake --build build-steamos
cmake --install build-steamos
```

Use the normal Linux build dependencies and initialize the pinned Linux
submodules, including `libvirtualdisplay` (its userspace library and UAPI are
still build dependencies; this profile installs no kernel module). Vulkan
headers and a shader compiler must be present for the command above. Build on
the oldest supported SteamOS-compatible userspace: a newer distribution's
compiler and libraries can introduce symbols missing on SteamOS. The installer
runs the staged executable with `--help` before activating it and rejects an
incompatible or incomplete payload.

Installation copies application shared-library dependencies into `lib/`. The
OS loader, C/math libraries, GLib, Mesa/Vulkan/VAAPI drivers, Wayland, PipeWire and PulseAudio
remain supplied by SteamOS. Bundle relocation does not promise compatibility
with every SteamOS release; test the actual artifact on each supported version.

Disabling CUDA and direct KMS is the reference Steam Deck build: Gamescope and
KWin use PipeWire DMA-BUF capture and AMD encoding uses VAAPI or Vulkan. Other
Gamescope distributions may enable those backends when their hardware and
permission model require them.

The resulting relocatable payload contains at least:

```text
payload/
  bin/vibepollo
  lib/...
  share/vibepollo/...
```

Install it from the checkout as the `deck` user (never with `sudo`):

```bash
packaging/linux/steamos/install-user.sh --payload /path/to/payload
```

The installer copies each build to
`$XDG_DATA_HOME/vibepollo-steamos/releases/`, atomically changes `current`, and
enables `vibepollo-steamos.service` in the user's graphical-session and
gamescope-session targets.
Installation restarts the service, interrupting an active stream. A failed
activation restores the previous release and launcher. Old release directories
are retained for manual rollback. Pass `--no-start` to activate the files without
restarting the host; a running user service manager is still required.
Use `--no-enable` for a dormant first installation: it installs the bundle,
launcher and unit without enabling or starting the service. It preserves any
existing service enablement, so it does not pause an installation already enabled.

The generated launcher preserves the installer's XDG data and configuration
directories even when the user service manager has different environment values.

The launch wrapper reads only a bounded `GAMESCOPE_WAYLAND_DISPLAY` value from
Gamescope's runtime environment file. It never evaluates that file. After
verifying the compositor, the host also imports its matching local X11 display
for application launches. The unit stops with `graphical-session.target` and
is also enabled directly in `gamescope-session.target`: SteamOS can retain the
shared graphical target while stopping its services during a mode transition.
Gaming Mode explicitly starts the host after `gamescope-session.service` has
published its compositor environment and reported ready. PipeWire and the
physical display remain owned by the session.

## Current capability boundary

- Gaming Mode automatically uses Gamescope's compositor-owned PipeWire stream,
  negotiates GPU DMA-BUFs when the encoder can import them, and asks stock
  Gamescope to downscale to the client bounds without changing the panel mode.
  If capture initialization fails, it tries regular KMS capture when built in,
  then X11 for SDR. The fallback captures an existing output and keeps virtual
  desktop preferences unchanged. HDR requests require an HDR-capable fallback.
- Desktop Mode prefers native KWin capture before enumerating the portal, so
  Automatic capture does not open a screen-selection dialog on Plasma. KMS,
  portal, and X11 remain fallbacks when native compositor capture is unavailable.
  The optional `vibeshine_drm` pool remains a Desktop Mode
  machine-package feature rather than a Gaming Mode dependency.
- Local audio playback follows the Moonlight client's setting. Steam titles are handed to the Steam client
  already running in Gaming Mode instead of starting a second Steam/Proton
  environment. This is resolved when launching, including catalog entries saved
  in Desktop Mode with direct Proton commands. Vibepollo's environment-based
  frame limiter and Smooth Motion injection are unavailable for these Steam
  handoffs; Steam's own launch options still apply.
- The SteamOS profile defaults to Xbox One controller emulation because stock
  SteamOS does not give this user access to `/dev/uhid`. DualSense requires that
  access; it is not enabled by this installer. Keyboard, mouse and Xbox input
  use the session's existing `/dev/uinput` permissions.
- Stock Gamescope advertises 8-bit SDR only. The optional capture patch adds
  verified 10-bit BT.2020/PQ DMA-BUF capture before panel tone mapping. It requires
  a working Main10 encoder; the tested host Mesa needs an upstream firmware fix.
  Independent or larger virtual modes, explicit synchronization and authoritative
  presentation timestamps remain unavailable.

Keep capture and output selection on Automatic when switching modes. Select
the physical display. Disable client HDR with stock Gamescope; the optional
patched path can follow the client HDR request after its hardware tests pass. An explicit
virtual-display request is rejected with an explanation; it must not invoke
KScreen or the managed-display driver in the Gaming Mode capture path.

For local development, inspect the service with:

```bash
systemctl --user status vibepollo-steamos.service
journalctl --user -u vibepollo-steamos.service -b
bash packaging/linux/steamos/check-host.sh
```

Validate the rootless install flow without touching the live user service:

```bash
packaging/linux/steamos/tests/test-user-install.sh
```

The same scripts are included in a staged payload at `share/vibepollo/steamos/`.
The readiness script checks devices and the PipeWire socket without changing
permissions or restarting services. A passing check is not proof of streaming.

Uninstall with:

```bash
packaging/linux/steamos/uninstall-user.sh
```

This removes the installed payload, launcher, and unit. It deliberately does
not delete Vibepollo configuration, credentials, pairings, or application data.

## Privileged components

Stock Gamescope capture is the default and requires no root component. The
`gamescope/` directory contains the pinned HDR patch, source lock and build
script. Its development candidate has not replaced the active compositor.
`sysext/` contains release metadata templates for a signed, exact-version image;
it is not an activation tool. Shipping privileged Gaming Mode integration still
requires those release checks and an OS graphics driver carrying the encoder fix.

The `vibeshine_drm` module remains optional and is not loaded in Game Mode. A
future Desktop Mode fallback may install it through the existing machine
package, with a matching kernel header package and the normal reboot/update
guardrails.
