# Shared native Linux build/install helper (experimental)

On **Arch Linux and CachyOS**, this tool builds a native package from the local
checkout and installs it through `linux_install.sh --package`. It supports first
installation, replaces conflicting Sunshine/Vibeshine packages through pacman,
and reuses the package account, machine-profile, signing, driver and firewall setup. The
native installer installs required dependencies and matching kernel headers, but
does not run a full system upgrade. Driver sources and the build helper are
required in every local package; installation verifies or builds the module for
the running kernel and fails if it remains missing. Reboot/key-enrollment states
are reported separately from build failures.
Run from the checkout as your normal user, not with `sudo`:

```bash
python3 scripts/linux_local_deploy.py install --version 1.19.0-beta.5
```

The script locates its checkout relative to its own file, so an absolute path
to the script also works from any directory. It never assumes a username or
home-directory layout and does not install shell aliases.

The command configures an incremental Ninja/RelWithDebInfo `/usr` build, builds
both web interfaces and the host, stages a SHA-256-pinned payload, and
prompts before elevating for installation. Tests are skipped by default;
`--enforce` runs the full suite and blocks installation on **any** failure.
Archive, ownership, driver and startup checks always apply. Arch installations
retain a local `build/vibepollo-*.pkg.tar.gz` and a SHA-256-verified root-private
copy. Pacman performs the installation and conflict removal in one transaction;
no prebuilt host release is downloaded. This tool does not commit, push, or
delete itself. The normal CMake/npm build may download its declared
build dependencies.

## Supported scope and caveats

- Python 3.11+, native x86_64 Linux, and the repository's C++23 build dependencies.
  CMake, Ninja, npm, and Git must already be available. CUDA is optional.
- Live deployment requires systemd, cgroup v2, Linux 6.16+, and a merged module
  layout where `/lib/modules` resolves to `/usr/lib/modules`. It requires the
  existing Vibepollo service account, machine configuration, shared state,
  controller/socket architecture, and managed virtual-display setup **only for
  the file updater on non-Arch systems**. On Arch/CachyOS the native package
  lifecycle creates these prerequisites.
- Runtime operation targets the native controller's KDE/Wayland seat0 desktop
  or greeter sessions. On Arch, confirmed installation disables the invoking
  user's obsolete Sunshine/Vibeshine/Vibepollo service and imports the selected
  desktop profile into service-owned state. Original profiles remain intact. Multiple
  source profiles require explicit selection through the native installer
  (`--source-profile`) or `vibepollo configure USER SOURCE` before retrying setup.
  It does not support arbitrary compositors, Windows, macOS, live deployment
  inside containers, or cross-compiling. `--stage-only` can be used in Linux build
  containers: it skips the live-host requirements and does not elevate.
- The development baseline is an Arch-family KDE/NVIDIA host. Other compatible
  machines and GPU/toolchain combinations are **not end-to-end certified**.
  The driver upgrade/reboot/rollback workflow has focused tests and staged
  validation, but has not yet been exercised end-to-end on a live system.
- Backups are retained locally; keep sufficient free space and independent
  backups. File rollback cannot reverse firmware changes, a loaded kernel
  module, external package hooks, or arbitrary concurrent administrator edits.
  The package-process check is best-effort, not a package-manager-wide lock.

Unsupported platforms and missing native tools fail before deployment. Build
options do not permit changing privileged installation or signing paths.

## Build settings

`--version` overrides the `BUILD_VERSION` environment variable, then the existing
`build/CMakeCache.txt` version, then an exact Git tag on a clean HEAD checkout
(an optional `v` prefix is removed from tags). An untagged or modified fresh checkout needs an explicit
version. Positive-major release versions and numbered alpha/beta/rc/stable
versions are accepted; the script never invents `0.0.0` or a version from an
older ancestor tag. The example version above is illustrative, not a pinned
script default.

`--cc` and `--cxx` select compiler executables. Otherwise `CC`/`CXX`, existing
cache settings, and finally CMake defaults apply, in that order. Use a compiler
and standard library satisfying the repository's C++23 requirements. Options
accept executable names/paths, not shell command strings.

`--cuda auto` preserves a cached CUDA choice; on a fresh build it enables CUDA
when it finds `nvcc`. `--cuda on` requires a toolkit; `--cuda off` disables it.
Use `--cuda-root` for a directory containing `bin/nvcc`, or let the script use
`CUDAToolkit_ROOT`/`CUDA_PATH`, the build cache, PATH, then common toolkit
locations. `--cuda-host-compiler` selects a compatible CUDA host compiler.
An existing CUDA-enabled build never silently downgrades when its toolkit is
missing. A toolkit's presence does not establish GPU/driver compatibility.

`--jobs` controls build and enforced-test parallelism; the default is the CPU
count capped at 10. One checkout uses one `build` directory and deployment lock.
Do not run another CMake/Ninja/npm build in that checkout concurrently. CMake
may require a clean build when changing compilers; this script never deletes
your cache/build tree automatically.

For example, stage a non-CUDA build using explicit compilers without touching
system services:

```bash
python3 scripts/linux_local_deploy.py install --version 1.19.0-beta.5 \
  --cc gcc --cxx g++ --cuda off --jobs 4 --stage-only
```

Existing configurations remain usable with simply:

```bash
python3 scripts/linux_local_deploy.py install
```

## Installation and readiness

On Arch/CachyOS, package hooks create service accounts before setting private
host permissions, import configuration and pairings without modifying the
original desktop profile, install the driver, and enable the session controller.
A successful pacman exit alone is insufficient: the helper verifies the exact
installed payload, permissions and capabilities, then checks startup readiness.
If the loaded driver needs replacement, reboot and run `--part2`. The same
command retries readiness after resolving a package setup error.

**Package recovery uses pacman**, for example
`sudo pacman -U /path/to/previous.pkg.tar.zst`, with the previous package from
`/var/cache/pacman/pkg` or your retained local builds. `--recover` reports this
requirement; it never applies a file rollback over the package database. Package
hooks and dependency changes are not covered by the file updater's rollback journal.
The following file-journal behavior applies only to non-Arch configured hosts.

Installation disconnects streams. The root phase closes and temporarily masks
broker admission, quiesces the controller/host/brokers, verifies empty cgroups,
backs up the exact installed files (including ownership, modes, capability
xattrs, symlinks, and absent-file records), installs the complete native
payload, upgrades the DRM driver, reloads units/udev, and starts **only the controller**.
Each shutdown/install phase prints progress. Old hashed web
assets and old versioned executables are backed up and removed from the new
installation.

Readiness checks use the current host invocation, its Web UI listener, H.264
discovery, a generation-bound broker
display-power probe, and active Vibepollo virtual kernel scanout (a working
physical monitor cannot hide a broken virtual output). They do not prove
client video delivery or suspend/resume; test those
with Moonlight afterwards. Capture log messages are not a startup gate: synthetic
encoder probes can succeed before a client starts actual KMS capture.
With no active Wayland seat0 session, an active controller
can report “waiting for session” instead of pretending a stream was tested.

## Resume and explicit recovery

Installation failures retain the candidate and diagnostics. The helper never
reverts application or driver files automatically, including on readiness
failure after reboot. A partially replaced payload stays stopped until you
explicitly recover it. A complete candidate that fails readiness can be retried
without rebuilding or rebooting again when the loaded driver already matches.

Resume the latest transaction or retry its readiness check:

```bash
python3 scripts/linux_local_deploy.py install --part2
```

Restore the previous installation only when requested:

```bash
python3 scripts/linux_local_deploy.py install --recover
```

On hosts with the `vibepollo-install` alias, use `vibepollo-install --part2`
and `vibepollo-install --recover`. Both select the latest transaction under the
root-owned deployment lock; no transaction ID is needed. The flags cannot be
combined with build options. The legacy `finalize [TRANSACTION_ID]` and
`rollback [TRANSACTION_ID]` commands remain available for explicit operations.
Old transactions' automatic rollback policies are no longer used.

Backups and transaction records remain under `/var/lib/vibepollo-local-deploy`.
An unfinished transaction blocks another installation until it is validated or
recovered. Changed installed files or corrupt backups stop recovery rather than
overwriting a later/manual upgrade. Failed recovery keeps admission closed.
Existing enabled/masked unit policy is not rewritten; pre-existing administrative
masks are rejected. Repeating `--recover` after a recovery reboot finishes that
recovery; `--part2` can finish it as well.

## Useful options and limits

`--skip-build` reuses the configured artifacts without checking for changed source;
normally let the incremental build run. It cannot be combined with compiler/CUDA
overrides. Combine with `--enforce` to test an existing `BUILD_TESTS=ON` build;
disabled or undiscovered tests cannot pass the enforced gate.
`--stage-only` produces `build/local-deploy-candidate.tar.gz` without sudo or
service changes. `--yes` skips the switchover confirmation, not sudo
authentication. Interactive confirmation clears queued build-time keystrokes,
uses readline editing to protect the prompt, and repeats on unrecognized input.
Noninteractive stdin requires `--yes`. `--timeout 120` extends the readiness deadline (10–300 seconds).

To opt into the full test gate (there are no test-failure exception flags):

```bash
python3 scripts/linux_local_deploy.py install --enforce
```

## Driver upgrades and reboot

The transaction backs up Vibepollo DRM sources, its DKMS registrations/builds,
its installation markers, and installed module files across kernels. It invokes
the official signed driver installer for the running kernel, previously installed
module kernels, and other installed kernels with headers. Same-version source
updates are supported. Rollback restores both application and driver state,
removes newly introduced driver artifacts, and runs `depmod` for affected kernels.
Keep package managers and other DKMS builds idle for the entire operation.

If KWin holds an older module, installation succeeds as `REBOOT_REQUIRED`, leaves
the host stopped, and prints this command to run after you reboot:

```bash
python3 scripts/linux_local_deploy.py install --part2
```

Finalization checks the boot changed, the selected kernel was built, and both
module version and source version match the installed candidate. A rollback
after the new module has loaded may itself require another reboot, followed by
`--part2` (or `--recover`); restoring disk files cannot replace a module held by KWin.

## Signing follows the native installer

The signing paths are **already native installer policy**, not a developer's
personal paths. `vibeshine-drm-install` uses `/var/lib/dkms/mok.key` and
`/var/lib/dkms/mok.pub`, validates the key pair and module signatures, and checks
kernel trust requirements. Its package-install path can create keys and arrange
MOK enrollment when required; a differently configured DKMS signing key may
cause it to choose its verified signed direct-build fallback or refuse a refresh.

This helper reuses that installer rather than implementing a second signing
scheme. Its additional transaction requirement is that the key pair already
exists with the official ownership/modes and `signing-status` succeeds before
the host is stopped. It invokes `install`, not the enrollment-capable
`install-package` action: key creation/enrollment belongs to the official initial
setup because firmware/MOK changes are outside file rollback. On kernels that
do not enforce trusted signatures, the official signing check does not require
MOK enrollment merely because Secure Boot is enabled.

The shared transaction currently allows empty/comment-only DKMS configuration,
plus literal framework assignments repeating the official defaults:

```bash
mok_signing_key="/var/lib/dkms/mok.key"
mok_certificate="/var/lib/dkms/mok.pub"
try_sign_modules="not_in_chroot"
```

Other active DKMS configuration, shell evaluation, per-module overrides, and
weak-module hooks are conservatively rejected pending review of their rollback
effects. This is a narrower transaction guarantee, **not** a claim that the
native installer cannot handle other DKMS configurations. Matching headers for
every targeted kernel and the existing signing setup remain prerequisites;
missing prerequisites are reported before stopping the host.

This updates an **existing** native install.
It does not run `configure-auto`, migrate profiles, alter pairing/configuration
state, or change the desktop session. The script never unloads a live module, switches
VTs, restarts KWin, or reboots the machine.
