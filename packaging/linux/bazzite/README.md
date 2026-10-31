# Experimental Bazzite support

This is a single-machine qualification, not a general Bazzite release. The
opt-in app RPM omits mutable-distribution installer scriptlets. A separate
signed driver RPM has only module-index maintenance scriptlets; it does not
rebuild drivers at boot.

## Tested configuration

User-reported tests on 2026-10-29: Bazzite NVIDIA Open 44 (44.20261026), KDE
Wayland, kernel `7.2.11-ogc4.1.fc44.x86_64`, NVIDIA 615.78.08, RTX 5080,
Secure Boot enabled, SELinux enforcing, Artemis on an M1 MacBook Air.

Pairing, desktop video, audio, keyboard, mouse, disconnect/reconnect, monitor
restoration, startup after reboot and login, logout/login, and suspend/resume
passed. The physical monitor blanks during streaming and returns afterward.
Both signed drivers loaded. The tray error and mixed-case account setup were
also verified fixed.

The tested app was `efd8d7341dba879174529f7319a89d8d0332cb74` plus the local
fixes consolidated here. This consolidated checkout still needs a clean
end-to-end build/install rerun. Startup logs confirm H.264 NVENC; HEVC was the
requested client setting, but the negotiated codec was not independently
recorded. HDR, AV1, controller input, remote-network access, pre-login capture,
and performance measurements are not qualified.

## Build locally

Use a committed checkout. Prerequisites include git, Podman, Python 3, host
make/compiler, matching kernel-devel, OpenSSL, kmod and sudo. Allow at least
35 GiB free space. All compilation runs on the developer's PC.

```bash
bash packaging/linux/bazzite/build-local.sh
bash packaging/linux/bazzite/drivers/build-drivers.sh
```

The app builder bootstraps a cached Fedora 44/GCC 14 image for CUDA 12.9. The
first build is lengthy. Use the RPM filename printed by the driver builder
where DRIVER_RPM_FILENAME appears below. The driver builder uses the host kernel's compiler
family for the qualified 7.2.11 kernel by default. It verifies
the driver submodule revisions and derives the exact kernel package dependency
from RPM file ownership. It reuses the app builder image for packaging.

App output: `local-output/Vibepollo.rpm`. Driver output and logs are under
`${XDG_DATA_HOME:-$HOME/.local/share}/vibepollo-bazzite/`. The report includes
source revision, kernel dependency, certificate and module/RPM hashes. The
signing helper requests sudo and keeps the private key in root-only
`/var/lib/vibepollo-module-signing/`. Only the public certificate is exported.
Builders do not install packages, enroll certificates, enable services or reboot.

## Manual installation

Substitute the driver output directory for `DRIVER_OUTPUT`. Preserve a known-good
Bazzite deployment. Keep Secure Boot and SELinux enabled.

```bash
sudo mokutil --import DRIVER_OUTPUT/MOK.der
```

Set an enrollment password, reboot, confirm Enroll MOK in MokManager, and return
to the desktop. Check `mokutil --test-key DRIVER_OUTPUT/MOK.der` and
`mokutil --sb-state`. On the tested image the key appeared in
`sudo keyctl list %:.machine`, nested under the secondary trusted keyring;
`mokutil` emitted a keyring lookup warning despite successful enrollment.

```bash
sudo rpm-ostree install "$PWD/local-output/Vibepollo.rpm" DRIVER_OUTPUT/DRIVER_RPM_FILENAME
```

After successful staging, reboot, then check and configure:

```bash
uname -r
rpm -q vibepollo vibepollo-kmod
modinfo -F signer vibeshine_drm
modinfo -F signer vibeshine_ds5
sudo /usr/libexec/vibeshine/vibepollo-bazzite-driver-check --load
sudo modprobe vibeshine_ds5
sudo vibepollo configure "$(id -un)" &&
  sudo systemctl enable --now vibepollo-session-controller.service
systemctl --no-pager status vibepollo-session-controller.service vibeshine-vkms.service vibepollo.service
```

Enable the controller only after configuration succeeds. Open
`https://localhost:47990`, create credentials, and pair Artemis using the host's
LAN address. No blanket firewall changes are made. Scope any necessary firewall
rules to the intended network.

For package revisions, the tested replacement operation was
`rpm-ostree install --uninstall=vibepollo --uninstall=vibepollo-kmod APP_RPM DRIVER_RPM`,
followed by a reboot. Reuse the signing key for future driver builds.

## Preparing a future kernel (prototype; not yet hardware-qualified)

Keep the current working deployment while preparing an update. Resolve the
stable image digest on the host (requires skopeo):

```bash
skopeo inspect --format '{{.Digest}}' docker://ghcr.io/ublue-os/bazzite-nvidia-open:stable
```

Use that exact digest, not the moving stable tag:

```bash
bash packaging/linux/bazzite/drivers/build-drivers.sh --target-image ghcr.io/ublue-os/bazzite-nvidia-open@sha256:DIGEST
```

This pulls the target image locally and checks its kernel RPM owner, matching
kernel-devel and compiler. If those tools/headers are absent, it stops; it does
not modify the image or install packages into the host to compensate. Building
uses that image's kernel headers and compiler in an unprivileged, offline
container. Signing executes only the root-owned host sign-file tool, reusing
the existing root-only key; update mode refuses to create a replacement key.
The target image has no access to that key. RPM releases include the target
kernel and source revision to distinguish future packages.

After signature/metadata/payload checks succeed, report.json records the exact
image digest, kernel package dependency, source revision and hashes.
STAGE-COMMAND.txt contains a proposed rpm-ostree rebase command combining that
signed image digest with replacement of the old local driver RPM in one
transaction. The tool **does not execute it**, reboot, stop streaming, or change
boot selection. Review the report and validate the host's rebase options before
staging. Pin the known-good booted deployment before the first real update test.
After staging succeeds, inspect the pending deployment before rebooting, then
verify both modules load and repeat streaming/reconnect checks. Preserve the
old deployment for recovery if the kernel or driver fails.

This is supervised preparation, not integration with Bazzite's automatic updater.
It has policy tests but no real future-kernel/container/transaction qualification
yet. The already installed exact-kernel package continues to block an ordinary
incompatible update until an appropriate driver replacement is supplied.

## Remaining work

- Qualify future-image preparation on real hardware, including compiler/header
  availability and one-transaction staging. Automatic updater integration remains
  unfinished. Keep the exact kernel dependency; do not bypass it to force upgrades.
- Clean install/reinstall and uninstall/rollback recovery qualification.
  No automated cleanup is provided; preserve credentials and pairings.
- Windows-style Force Quit/disconnect action in the Linux tray.
- Other machines/images, codecs, HDR, controllers and performance testing.
- Clean qualification of this consolidated checkout and upstream review.

The user requested local builds only. Publication commits use `[skip ci]`;
this draft adds no hosted Bazzite workflow and does not claim CI passed.

## Automatic sleep and Force Close (2026-10-31)

PowerDevil treats `ChangeScreenSettings` as also inhibiting session interruption.
The former retained-display hold therefore prevented automatic suspend after a
client disconnected. Only pending/active captures now own a power inhibitor;
retaining a virtual topology does not. Legacy retain-helper modes remain accepted
but no longer acquire an inhibitor or wake an output. Reconnect still acquires
an active inhibitor and wakes DPMS before preparing the display.

On the qualified host, rejecting only `Vibepollo / Retained remote display` through
KDE's `SetInhibitionAllowed` restored automatic suspend, followed by successful
Artemis Wake PC and reconnect. That establishes the diagnosis and workaround;
installation/runtime qualification of this source change is still pending.
Normal idle display-off with a retained virtual-only topology needs a longer
regression test too: the removed hold was originally intended to avoid an NVIDIA
GSP hang. A two-minute sleep test alone does not qualify that path.

The native tray adds **Force close client**, using the existing token-authorized
`POST /api/apps/close` scope with `{"force":true}`. It drains RTSP/WebRTC transports before calling the native
Force Close application cleanup. All streams end, applications
may lose unsaved progress, clients stay paired, and the host stays available.
The existing Stop action and empty/null/`{}` API bodies keep graceful behavior.

After installing and rebooting into the updated app RPM, remove the diagnostic
KDE exception so the source fix is tested without it:

```bash
busctl --user call org.kde.Solid.PowerManagement.PolicyAgent \
  /org/kde/Solid/PowerManagement/PolicyAgent \
  org.kde.Solid.PowerManagement.PolicyAgent SetInhibitionAllowed ssb \
  "Vibepollo" "Retained remote display" true
```

Then test automatic sleep with the short timeout, Artemis wake/reconnect, Force
Close on a disposable desktop stream, and reconnect. Also leave a disconnected
session through its normal screen-off timeout before testing reconnect. Restore
the preferred sleep timeout afterward. No driver rebuild or MOK enrollment is
required for these app-only changes.

Remove the temporary PowerDevil debug override when diagnosis is complete:

```bash
rm -f "$XDG_RUNTIME_DIR/systemd/user/plasma-powerdevil.service.d/90-sleep-debug.conf"
systemctl --user daemon-reload
systemctl --user restart plasma-powerdevil.service
```

For the original qualified cached checkout, use the reviewed commit SHA with
`update-runtime.sh SHA [SOURCE_DIRECTORY]`. It downloads that commit's narrow
runtime/test patch, checks applicability before applying it, reuses the existing
Fedora/GCC/CUDA container and CMake cache, runs focused regressions, and writes
`runtime-fixed-output/Vibepollo.rpm` (app release `4.bazzite`). It installs nothing.
The original checkout may still have local account/home-path fixes; they remain
in place. If patch applicability fails, stop and inspect the log rather than
resetting the checkout. This compatibility launcher is intended for the original
single-machine qualification checkout, not as an automatic updater.
