# Installing Vibepollo on Linux (beta)

Vibepollo's Linux host is in beta. It ships as a native package for **Arch Linux and CachyOS**,
and it is developed and tested on **CachyOS with KDE Plasma 6 on Wayland**. Other Arch-based
distributions should work but get less testing. There is no AppImage, Flatpak, Debian, Fedora, or
Docker build in this beta.

## Requirements

| Requirement | Detail |
| --- | --- |
| Distribution | Arch Linux or CachyOS, x86_64. Vibepollo is tuned for CachyOS. |
| Desktop | KDE Plasma 6 on **Wayland**, started by **SDDM** or **Plasma Login Manager**. GNOME, other compositors, X11 sessions, and remote logins are not streamed. |
| Kernel | Linux **6.16 or newer**, plus the headers for the kernel you boot (for example `linux-cachyos-headers`). The virtual-display driver is built with DKMS during installation. |
| GPU | Any GPU with a hardware **H.264** encoder. NVIDIA uses NVENC through `nvidia-utils`; AMD needs `libva-mesa-driver`; Intel needs `intel-media-driver`. HEVC and AV1 are used when available. |
| Pre-login streaming | NVIDIA only. AMD and Intel machines stream after you log in. |
| Accounts | One interactive desktop account. Machines with several accounts choose the streaming owner once (see [Choosing the streaming user](#choosing-the-streaming-user)). |
| Secure Boot | Supported. The package signs its kernel module; on kernels that enforce module signatures you approve a one-time MOK enrollment at the next reboot. Do not disable Secure Boot. |

Vibepollo runs as **machine-wide system services**, not as a per-user service. Do not run it with
`systemctl --user`, and do not add file capabilities to `/usr/bin/vibepollo`.

## Install with the script (recommended)

The installer checks the requirements above, installs the kernel headers for your running kernel,
installs the package, opens the firewall if one is active, and tells you whether a reboot is needed.

```bash
curl -fsSLO https://raw.githubusercontent.com/Nonary/Vibepollo/master/scripts/linux_install.sh
less linux_install.sh            # optional: read what it does
sudo bash linux_install.sh
```

The script prefers the signed Nonary pacman repository. When the repository is not reachable it
downloads the newest release package from GitHub and installs it with `pacman -U`. Useful options:

| Option | Effect |
| --- | --- |
| `--version 1.19.0-beta.5` | Install that exact release. |
| `--stable` | Ignore pre-releases. |
| `--package ./vibepollo-*.pkg.tar.zst` | Install a package you already downloaded. |
| `--no-repo` | Skip the pacman repository and use GitHub releases. |
| `--yes` | Answer pacman prompts automatically. |

Re-running the script is safe. It only installs what is missing and repeats the checks.

## Install manually

### From the signed repository

Import and locally trust the Nonary repository key:

```bash
curl -fsSLo /tmp/nonary-vibepollo.gpg \
  https://nonary.github.io/Vibepollo/arch/x86_64/nonary-vibepollo.gpg
curl -fsSLo /tmp/nonary-vibepollo-fingerprint.txt \
  https://nonary.github.io/Vibepollo/arch/x86_64/nonary-vibepollo-fingerprint.txt
sudo pacman-key --add /tmp/nonary-vibepollo.gpg
sudo pacman-key --lsign-key "$(tr -d '[:space:]' </tmp/nonary-vibepollo-fingerprint.txt)"
```

Add the repository and install:

```bash
sudo install -Dm644 /dev/stdin /etc/pacman.d/vibepollo.conf <<'EOF'
[vibepollo]
SigLevel = Required
Server = https://nonary.github.io/Vibepollo/arch/x86_64
EOF
grep -qxF 'Include = /etc/pacman.d/vibepollo.conf' /etc/pacman.conf || \
  printf '\nInclude = /etc/pacman.d/vibepollo.conf\n' | sudo tee -a /etc/pacman.conf
sudo pacman -Syu vibepollo
```

Later releases then arrive through the normal `pacman -Syu`.

### From a release package

Every release on the [releases page](https://github.com/Nonary/Vibepollo/releases) carries a
`vibepollo-<version>-1-x86_64.pkg.tar.zst` asset. Install the kernel headers first so the driver
builds during the transaction, then install the package:

```bash
sudo pacman -S --needed "$(pacman -Qqo /usr/lib/modules/$(uname -r)/vmlinuz)-headers"
sudo pacman -Syu
sudo pacman -U ./vibepollo-*.pkg.tar.zst
```

Packages installed this way are upgraded by downloading the next release and repeating the
`pacman -U` step, or by switching to the repository above.

### Build from source

The `PKGBUILD` in the repository is a template; a CMake configure step fills in the commit and
version before `makepkg` can use it. Install `cuda` first if you want NVENC (the build detects it):

```bash
git clone --branch 1.19.0-beta.5 https://github.com/Nonary/Vibepollo.git
cd Vibepollo
cmake -S . -B build -DSUNSHINE_CONFIGURE_ONLY=ON -DSUNSHINE_CONFIGURE_PKGBUILD=ON
mkdir pkg && cp build/PKGBUILD build/vibepollo.install pkg/
cd pkg && makepkg -si
```

The build takes a while: it compiles the web interface with `npm`, runs the test suite, and needs
`nodejs`, `npm`, `ninja`, and `gcc15` (the compiler CUDA supports). Pass `--nocheck` to
`makepkg` to skip the tests.

## After installation

The package prints its remaining steps at the end of the pacman transaction. On a typical machine
there are four.

1. **Reboot if asked.** A kernel that still holds an older driver, or a pending Secure Boot key
   enrollment, needs one reboot. On the Secure Boot path the firmware shows the blue MOK Manager
   screen: choose **Enroll MOK**, continue, and enter the password you typed during installation.
2. **Log in to Plasma (Wayland) and pair.** Open <https://localhost:47990> on the machine, create
   the Web UI username and password, then pair Moonlight with the PIN. Pairing works only from a
   logged-in desktop. The pre-login stream reuses that pairing and cannot pair new clients.
3. **Open the firewall** if one is enabled. The package ships definitions for both common firewalls:

   ```bash
   # firewalld
   sudo firewall-cmd --permanent --add-service=vibepollo && sudo firewall-cmd --reload
   # ufw
   sudo ufw allow Vibepollo
   ```

   Vibepollo listens on TCP 47984, 47989, 47990, and 48010, and on UDP 47998 to 48000 and 48010.
   The UDP ports carry the video, audio, and control streams and are required even when the Web UI
   already works.
4. **Log out and back in once.** The package installs a PipeWire drop-in
   (`/usr/share/pipewire/pipewire.conf.d/50-vibepollo-audio.conf`, 240 samples at 48 kHz) that the
   audio stream expects. A new login, or `systemctl --user restart pipewire`, applies it.

### Choosing the streaming user

Installation picks the single interactive desktop account automatically. If the machine has several
accounts the package prints an **ACTION REQUIRED** line instead. Choose the owner once and enable
the controller:

```bash
sudo /usr/libexec/vibeshine/vibepollo-machine-host configure USER
sudo systemctl enable --now vibepollo-session-controller.service
```

## Verify

```bash
sudo systemctl status vibepollo-session-controller.service vibepollo.service
sudo journalctl -u vibepollo-session-controller.service -u vibepollo.service -b
sudo /usr/libexec/vibeshine/vibeshine-drm-install status
```

A healthy host logs `Screencasting with KMS`, `Using event-driven KMS capture for Vibepollo DRM
CRTC`, and at least `Found H.264 encoder`. HEVC and AV1 lines appear only when the GPU supports
them. Open ports alone do not prove streaming works; check for those lines.

The controller starts the host only while the configured user's Plasma Wayland session (or the
NVIDIA pre-login greeter) is active. Do not start `vibepollo.service` by hand. Restarting either
unit disconnects any client that is streaming.

## Troubleshooting

| Symptom | What to do |
| --- | --- |
| `kernel headers ... are missing` during install | Run the printed `sudo pacman -S --needed <kernel>-headers`, then `sudo /usr/libexec/vibeshine/vibeshine-drm-install install`. |
| Install says the running kernel still holds the old module | Reboot. `modinfo -F version vibeshine_drm` and `/sys/module/vibeshine_drm/version` must match. |
| MOK enrollment queued | Reboot and approve it in MOK Manager. `mokutil --list-new` shows what is pending. |
| Controller logs `unsupported-session` | You are not in a Plasma **Wayland** session started by SDDM or Plasma Login Manager. Pick "Plasma (Wayland)" at the login screen. |
| No HEVC or AV1 encoder found | Normal on older GPUs. H.264 is enough for the host to report ready. |
| No pre-login stream | Expected on AMD and Intel GPUs. Log in first. |
| Audio crackles or drifts | Log out and in again so the PipeWire drop-in applies. Keep Vibepollo's virtual sink; do not pin a static `audio_sink`. |
| Moonlight sees the host but the stream fails | Open the UDP ports (see above). |
| `could not start the Vibepollo session controller safely` at install | Reboot, then check `systemctl status vibepollo-session-controller.service`. Report the journal output if it still fails. |

Keep `capture = kms` in `/var/lib/vibepollo/vibepollo.conf`. The `wlr` and portal capture paths are
not used by the machine host and a portal probe can block unattended startup.

## Where things live

| Path | Purpose |
| --- | --- |
| `/usr/bin/vibepollo` | Public command-line client, unprivileged. |
| `/usr/libexec/vibeshine/` | Privileged helpers (host, session broker, controller, driver installer). |
| `/etc/vibepollo/` | Administrator policy, including the chosen desktop user in `machine.conf`. |
| `/var/lib/vibepollo/` | Host state: `vibepollo.conf`, credentials, pairings, application list. |
| `/usr/src/vibeshine-drm-*` | DKMS source for the virtual-display driver. |

## Upgrade

Repository installs upgrade with `sudo pacman -Syu`. Release-package installs upgrade with
`sudo pacman -U` on the next package. The upgrade hook stops the running host, so finish any stream
first. If the hook says the kernel still holds the old driver, reboot before streaming again.

## Uninstall

```bash
sudo pacman -R vibepollo
```

Removal stops the services and removes the DKMS module but keeps `/var/lib/vibepollo` and
`/etc/vibepollo`. To wipe them as well, run
`sudo /usr/libexec/vibeshine/vibepollo-machine-host reset` before removing the package.
