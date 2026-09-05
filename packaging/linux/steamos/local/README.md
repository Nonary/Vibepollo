# Local SteamOS HDR deployment

These tools prepare an explicitly requested local development installation.
They do not build or activate the signed release image described in `../sysext/`.
All build and staging steps leave the current desktop, stream, Steam client,
services and operating-system graphics packages running unchanged.

The private encoder is Mesa 26.1.7 with the upstream Deck HEVC firmware fix,
built for the host libva 2.22 API. It exposes only the Radeon VAAPI encoder;
SteamOS continues supplying OpenGL, Vulkan, GBM, EGL, PipeWire and the C library.
Mesa's ACO compiler avoids an additional LLVM runtime. Linking against host
libm and using traditional GNU TLS avoid newer development-container ABI
requirements. The resulting driver uses compiler and system-library symbols
already supplied by SteamOS; no `LD_LIBRARY_PATH` is installed in the service
environment.

## Prepare and verify

The existing `moonlight-dev` container must contain the normal Mesa development
dependencies. The build script installs only pinned Python build tools into a
private virtual environment. It verifies the upstream Mesa and libva archive
checksums and builds with two low-priority jobs by default:

```bash
python3 packaging/linux/steamos/local/build-hdr-runtime.py \
  --output "$PWD/build/local-hdr-runtime"
```

The result is `runtime/`, with a native driver, source
metadata and `env.conf`. Native `ldd -r` and the expected libva entry point are
checked before packaging succeeds. A successful package is not an encode test;
run the HDR integration smoke test using this driver and the exact new payload.
The test must decode the produced HEVC and verify its pixels, not just open the
encoder. `../tests/run-hdr-smoke.py` owns a separate headless compositor.

`env.conf` contains only `LIBVA_DRIVERS_PATH`, `LIBVA_DRIVER_NAME` and
`VIBEPOLLO_PRIVATE_VAAPI=1`. The marker tells the SteamOS host to remove those
inherited driver choices from launched games and preparation commands. If the
runtime is copied to a permanent release directory, update the absolute driver
path in `env.conf` and its manifest checksum before passing it to the cutover
helper. Do not move a runtime while a service uses it.

Build the patched Gamescope candidate using `../gamescope/build-gamescope.py`,
then stage the audited candidate (which includes its file manifest):

```bash
python3 packaging/linux/steamos/local/activate-gamescope.py stage \
  --candidate /path/to/gamescope-candidate \
  --output "$PWD/build/gamescope-local" \
  --patchelf "$PWD/build/local-hdr-runtime/build-venv/bin/patchelf"
python3 packaging/linux/steamos/tests/test-local-hdr.py
```

Staging verifies the candidate's checksums and SteamOS build, embeds an absolute
library RUNPATH for its planned `/opt` destination, and copies the stock
Gaming Mode session script with one absolute compositor invocation replacement.
The launcher verifies OS, architecture, package, stock binary, session script,
payload inventory and checksums each time. The installed tree must remain
root-owned, unwritable by other users, and retain exactly `CAP_SYS_NICE` on the
compositor. A failed check runs the stock Valve session or compositor instead.
No replacement PATH or graphics-library environment is published to Steam.

## Activate after the current stream has ended

The native user host can replace Sunshine without root. This command disconnects
an active stream, migrates settings and pairings, and has a failure rollback:

```bash
bash packaging/linux/steamos/local/replace-sunshine.sh \
  --payload /path/to/validated-payload \
  --service-environment /path/to/permanent-hdr-runtime/env.conf
```

The copied profile consolidates legacy pairing aliases that share the exact
client certificate and identical permissions/settings. It retains the first
record's name and UUID, every distinct client certificate, the host identity,
and web credentials. Conflicting duplicate permissions stop migration and
restore Sunshine. The original profile and private backup remain unchanged.

Gamescope's additive `/opt` installation needs root to retain Valve's scheduler
capability. It does not replace `/usr/bin/gamescope` or disable SteamOS read-only
protection. Use the concrete destination printed by the staging command:

```bash
sudo python3 packaging/linux/steamos/local/activate-gamescope.py install-root \
  --stage "$PWD/build/gamescope-local"
python3 packaging/linux/steamos/local/activate-gamescope.py activate-user \
  --installed /opt/vibepollo-gamescope/local/BUILD-PATCH
```

The user drop-in takes effect on the next Gaming Mode entry. Neither command
restarts the compositor or switches modes. Enter Gaming Mode only after the
user is ready to end the current desktop/session. HDR client playback and
native/Proton game behavior still require checking in that actual session.

## Roll back

```bash
python3 packaging/linux/steamos/local/activate-gamescope.py rollback-user
```

This removes only the local session drop-in and selects Valve Gamescope for the
next Gaming Mode entry. It keeps the `/opt` candidate for inspection or reuse.
OS, Gamescope or session-script updates automatically select Valve's path via
the launch checks. Sunshine's original profile is preserved by the cutover
helper; its backup records the previous service and installation state.

## Managed virtual displays in Desktop Mode

The module, pool units, and restricted capture helper retain their shared
Vibeshine DRM names and paths; the streaming host and its user profile use
Vibepollo names.

The native host must be built with `SUNSHINE_ENABLE_DRM=ON` for this path.
It remains an ordinary capability-free user bundle. A separate C helper uses
only system libc, libdrm and libcap, and is installed as immutable root-owned
code with exactly `cap_sys_admin=p`. It accepts fixed requests for the managed
`vibeshine_drm` device only; physical GPUs and arbitrary ioctl requests are
rejected. Frame and fence descriptors cross the process boundary with
`SCM_RIGHTS`. Games retain the normal SteamOS execution environment.

Build the module against the exact running kernel headers. Apply
`0001-amd-private-display-modifiers.patch` to a private copy of the module
source before building; the patch adds the uncompressed AMD layouts validated
on this Deck. Build `linux/vkms_peercred.cpp` from libvirtualdisplay against
the host ABI, then build and stage the capture helper:

```bash
python3 packaging/linux/steamos/local/build-capture-helper.py \
  --output /path/to/vibeshine-kms-capture
python3 packaging/linux/steamos/local/activate-private-display.py stage \
  --module /path/to/vibeshine_drm.ko \
  --peercred /path/to/vibeshine-vkms-peercred \
  --driver-source third-party/libvirtualdisplay \
  --capture-helper /path/to/vibeshine-kms-capture \
  --user deck --output /path/to/private-display-stage
```

The staged manifest pins the kernel, module and all helper hashes. Installation
uses `/opt` and additive `/etc/systemd/system` units; it does not modify `/usr`
or disable SteamOS read-only protection. `install-root --stage DIR` installs
only. `start-root --installed DIR` explicitly loads the module and creates the
dormant pool; `enable-root --installed DIR` enables it for the next boot. Use
the installation directory printed by staging. Run these root operations only
when the user is ready for live display changes.

After provisioning, update an existing host with:

```bash
bash packaging/linux/steamos/local/update-user.sh \
  --payload /path/to/validated-payload --check
bash packaging/linux/steamos/local/update-user.sh \
  --payload /path/to/validated-payload --enable-private-display
```

The update preserves the profile and pairings, restarts the host, and restores
the previous release and profile if its startup check fails. Private displays
use client-specific ownership and the existing request, mode, HDR, input and
topology logic. An explicit layout choice is preserved; the default extends
the desktop and makes the stream output primary. Both SDR and HDR use completed
DRM frames, avoiding KWin 6.4's SDR conversion and integer refresh rounding in
its screencast path. Actual mode acceptance, GPU import and HDR capture must
still be validated after activation on the target machine.

This pool is managed by KScreen in Desktop Mode. Gaming Mode uses the separate
patched Gamescope compositor described above; independent private displays in
Gaming Mode are not implemented by this pool. After a kernel upgrade, the
loader stops with a clear version mismatch until the module is rebuilt.
`disable-root` disables future loading. To remove the files, disable the pool,
reboot, then use `uninstall-root`; the installer refuses to unload a module
that a running compositor may still hold.
