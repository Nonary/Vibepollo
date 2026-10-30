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
first build is lengthy. The driver builder uses the host kernel's compiler
family and intentionally accepts only the qualified 7.2.11 kernel. It verifies
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
sudo rpm-ostree install "$PWD/local-output/Vibepollo.rpm" DRIVER_OUTPUT/vibepollo-kmod-2.0.2-2.bazzite.x86_64.rpm
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

## Remaining work

- Kernel-update automation: the exact kernel dependency deliberately blocks
  using this package with another kernel. Do not remove it to force an upgrade.
- Clean install/reinstall and uninstall/rollback recovery qualification.
  No automated cleanup is provided; preserve credentials and pairings.
- Windows-style Force Quit/disconnect action in the Linux tray.
- Other machines/images, codecs, HDR, controllers and performance testing.
- Clean qualification of this consolidated checkout and upstream review.

The user requested local builds only. Publication commits use `[skip ci]`;
this draft adds no hosted Bazzite workflow and does not claim CI passed.
