# Getting Started

The recommended method for running Vibepollo is to use the [binaries](#binaries) included in the
[latest release][latest-release], unless otherwise specified.

[Pre-releases](https://github.com/Nonary/Vibepollo/releases) are also available. These should be considered beta,
and release artifacts may be missing when merging changes on a faster cadence.

## Binaries

Binaries of Vibepollo are created for each release. Availability varies by platform while the distribution channels are being established.
Binaries can be found in the [latest release][latest-release].

> [!NOTE]
> Some third party packages also exist.
> See [Third Party Packages](third_party_packages.md) for more information.
> No support will be provided for third party packages!

## Install

### Docker

> [!WARNING]
> The Docker images are not recommended for most users.

Docker images are available on [Dockerhub.io](https://hub.docker.com/repository/docker/lizardbyte/sunshine)
and [ghcr.io](https://github.com/orgs/LizardByte/packages?repo_name=sunshine).

See [Docker](../DOCKER_README.md) for more information.

### FreeBSD

#### Install
1. Download the appropriate package for your architecture

   | Architecture  | Package                                                                                                                                |
   |---------------|----------------------------------------------------------------------------------------------------------------------------------------|
   | amd64/x86_64  | [Sunshine-FreeBSD-14.4-amd64.pkg](https://github.com/LizardByte/Sunshine/releases/latest/download/Sunshine-FreeBSD-14.3-amd64.pkg)     |
   | arm64/aarch64 | [Sunshine-FreeBSD-14.4-aarch64.pkg](https://github.com/LizardByte/Sunshine/releases/latest/download/Sunshine-FreeBSD-14.3-aarch64.pkg) |

2. Open terminal and run the following command.
   ```sh
   sudo pkg install ./Sunshine-FreeBSD-14.4-{arch}.pkg
   ```

#### Uninstall
```sh
sudo pkg delete Sunshine
```

### Linux

Linux support is in **beta** and ships as a native package for **Arch Linux and CachyOS** (x86_64).
Vibepollo is developed and tested on **CachyOS with KDE Plasma 6 on Wayland**; other Arch-based
distributions are supported on a best-effort basis. AppImage, Flatpak, Debian/Ubuntu,
Fedora/openSUSE, Homebrew, and Docker builds are not produced for this beta.

The complete guide, including manual installation, verification, troubleshooting, and the file
layout, is [docs/linux/install.md](linux/install.md).

#### Arch Linux and CachyOS

##### Requirements

- **KDE Plasma 6 on Wayland**, started by **SDDM** or **Plasma Login Manager**. GNOME, X11
  sessions, other compositors, and remote logins are not streamed.
- **Linux 6.16 or newer with matching kernel headers** (for example `linux-cachyos-headers`).
  The managed virtual-display driver is built with DKMS during installation.
- **A GPU with an H.264 hardware encoder.** NVIDIA uses NVENC from `nvidia-utils`; AMD and Intel
  use VAAPI (`libva-mesa-driver` or `intel-media-driver`). Pre-login streaming is NVIDIA-only.
- **A single interactive desktop account**, or run
  `sudo vibepollo configure USER` once to choose the owner.

##### Install

Download and run the installer script. It checks the requirements, installs the headers for the
running kernel, installs the package from the signed repository (or the latest GitHub release when
the repository is unavailable), opens firewalld or ufw, and tells you whether to reboot:

```bash
curl -fsSLO https://raw.githubusercontent.com/Nonary/Vibepollo/vibe-test/scripts/linux_install.sh
sudo bash linux_install.sh
```

To install a specific release, pass `--version 1.19.0-beta.5`. To install a package you already
downloaded from the [releases page](https://github.com/Nonary/Vibepollo/releases), pass
`--package ./vibepollo-*.pkg.tar.zst`. Manual repository and `pacman -U` steps are in the
[Linux install guide](linux/install.md#install-manually).

##### After installation

1. **Reboot if asked.** A kernel that still holds an older driver, or a one-time Secure Boot key
   enrollment, needs one reboot.
2. **Log in to Plasma (Wayland) and pair.** Open `https://localhost:47990` on the machine, create
   the Web UI login, then pair Moonlight with the PIN. Pairing works only from a logged-in
   desktop; the pre-login stream reuses that pairing.
3. **Open the firewall** if the script did not do it for you:

   ```bash
   # firewalld
   sudo firewall-cmd --permanent --add-service=vibepollo && sudo firewall-cmd --reload
   # ufw
   sudo ufw allow Vibepollo
   ```

   Vibepollo listens on TCP 47984, 47989, 47990, and 48010, and UDP 47998 to 48000 and 48010.
4. **Log out and back in once** so the PipeWire audio drop-in the package installs takes effect.

Check the host with:

```bash
sudo systemctl status vibepollo-session-controller.service vibepollo.service
sudo journalctl -u vibepollo-session-controller.service -u vibepollo.service -b
```
##### Uninstall
```bash
sudo pacman -R vibepollo
```

### macOS

> [!IMPORTANT]
> Sunshine on macOS is experimental. Gamepads do not work.

#### DMG

##### Install

1. Download and install based on your architecture:

   | Architecture          | Package                                                                                                                |
   |-----------------------|------------------------------------------------------------------------------------------------------------------------|
   | arm64 (Apple Silicon) | [Sunshine-macOS-arm64.dmg](https://github.com/LizardByte/Sunshine/releases/latest/download/Sunshine-macOS-arm64.dmg)   |
   | x86_64 (Intel)        | [Sunshine-macOS-x86_64.dmg](https://github.com/LizardByte/Sunshine/releases/latest/download/Sunshine-macOS-x86_64.dmg) |

2. Open the downloaded `.dmg` file.
3. Drag `Sunshine.app` into the `Applications` folder.
4. Eject the disk image.

##### Uninstall
1. Quit Sunshine if it is running.
2. Open `Finder`, navigate to `Applications`, and drag `Sunshine.app` to the Trash.

#### Homebrew
This package requires that you have [Homebrew](https://docs.brew.sh/Installation) installed.

##### Install
```bash
brew update
brew upgrade
brew tap LizardByte/homebrew
brew install sunshine
```

##### Uninstall
```bash
brew uninstall sunshine
```

> [!TIP]
> For beta you can replace `sunshine` with `sunshine-beta` in the above commands.

### Windows

> [!NOTE]
> Sunshine supports ARM64 on Windows; however, this should be considered experimental. This version does not properly
> support GPU scheduling and any hardware acceleration.

#### Installer (recommended)

> [!CAUTION]
> The msi installer is preferred moving forward. Before using a different type of installer, you should manually
> uninstall the previous installation.

1. Download and install based on your architecture:

   | Architecture          | Installer                                                                                                                                    |
   |-----------------------|----------------------------------------------------------------------------------------------------------------------------------------------|
   | AMD64/x64 (Intel/AMD) | [Sunshine-Windows-AMD64-installer.msi](https://github.com/LizardByte/Sunshine/releases/latest/download/Sunshine-Windows-AMD64-installer.msi) |
   | AMD64/x64 (Intel/AMD) | [Sunshine-Windows-AMD64-installer.exe](https://github.com/LizardByte/Sunshine/releases/latest/download/Sunshine-Windows-AMD64-installer.exe) |
   | ARM64                 | [Sunshine-Windows-ARM64-installer.msi](https://github.com/LizardByte/Sunshine/releases/latest/download/Sunshine-Windows-ARM64-installer.msi) |
   | ARM64                 | [Sunshine-Windows-ARM64-installer.exe](https://github.com/LizardByte/Sunshine/releases/latest/download/Sunshine-Windows-ARM64-installer.exe) |

> [!TIP]
> Installer logs can be found in the following locations.<br>
> | File | log paths |
> | ---- | --------- |
> | .exe | `%%PROGRAMFILES%/Sunshine/install.log` (AMD64 only)<br>`%%TEMP%/Sunshine/logs/install/` |
> | .msi | `%%TEMP%/Sunshine/logs/install/` |

> [!CAUTION]
> You should carefully select or unselect the options you want to install. Do not blindly install or
> enable features.

To uninstall, find Sunshine in the list <a href="ms-settings:installed-apps">here</a> and select "Uninstall" from the
overflow menu. Different versions of Windows may provide slightly different steps for uninstall.

#### Standalone (lite version)

> [!WARNING]
> By using this package instead of the installer, performance will be reduced. This package is not
> recommended for most users. No support will be provided!

1. Download and extract based on your architecture:

   | Architecture          | Installer                                                                                                                                  |
   |-----------------------|--------------------------------------------------------------------------------------------------------------------------------------------|
   | AMD64/x64 (Intel/AMD) | [Sunshine-Windows-AMD64-portable.zip](https://github.com/LizardByte/Sunshine/releases/latest/download/Sunshine-Windows-AMD64-portable.zip) |
   | ARM64                 | [Sunshine-Windows-ARM64-portable.zip](https://github.com/LizardByte/Sunshine/releases/latest/download/Sunshine-Windows-ARM64-portable.zip) |

2. Open command prompt as administrator
3. Firewall rules

   Install:
   ```bash
   cd /d {path to extracted directory}
   scripts/add-firewall-rule.bat
   ```

   Uninstall:
   ```bash
   cd /d {path to extracted directory}
   scripts/delete-firewall-rule.bat
   ```

4. Windows service

   Install:
   ```bash
   cd /d {path to extracted directory}
   scripts/install-service.bat
   scripts/autostart-service.bat
   ```

   Uninstall:
   ```bash
   cd /d {path to extracted directory}
   scripts/uninstall-service.bat
   ```

## Initial Setup
After installation, some initial setup is required.

### FreeBSD

#### Virtual Input Devices

> [!IMPORTANT]
> To use virtual input devices (keyboard, mouse, gamepads), you must add your user to the `input` group.

The installation process creates the `input` group and configures permissions for `/dev/uinput`.
To allow your user to create virtual input devices, run:

```bash
pw groupmod input -m $USER
```

After adding yourself to the group, log out and log back in for the changes to take effect.

### Linux

#### Services

The Arch/CachyOS package installs Vibepollo as machine-wide system services.
The session controller is enabled during installation and starts the streaming host whenever the
configured user's KDE Plasma Wayland session is active:

```bash
sudo systemctl status vibepollo-session-controller.service vibepollo.service
sudo journalctl -u vibepollo-session-controller.service -u vibepollo.service --since '-10 minutes'
```

Do not start `vibepollo.service` directly; the controller binds the login session first. If the
installer could not pick the desktop account automatically, choose it once and enable the
controller:

```bash
sudo vibepollo configure USER
sudo systemctl enable --now vibepollo-session-controller.service
```

There is no per-user unit on Linux. Never enable `app-io.github.Nonary.vibepollo` with
`systemctl --user`, and never add file capabilities to `/usr/bin/vibepollo`; the packaged host
already carries the capabilities it needs.

### macOS
The first time you start Sunshine, you will be asked to grant access to screen recording and your microphone.

Sunshine supports native system audio capture on macOS 14.0 (Sonoma) and newer via Apple’s Audio Tap API.
To use it, simply leave the **Audio Sink** setting blank.

If you prefer to manage your own loopback device, you can still use
[Soundflower](https://github.com/mattingalls/Soundflower) or
[BlackHole](https://github.com/ExistentialAudio/BlackHole)
and enter its device name in the [audio_sink](configuration.md#audio_sink) field.

> [!NOTE]
> Command Keys are not forwarded by Moonlight. Right Option-Key is mapped to CMD-Key.

> [!CAUTION]
> Gamepads are not currently supported.

### Windows
In order for virtual gamepads to work, you must install ViGEmBus. You can do this from the troubleshooting tab
in the web UI, as long as you are running Sunshine as a service or as an administrator. After installation, it is
recommended to restart your computer.

![ViGEmBus Installation](images/vigembus-installer.png)

## Usage

### Basic usage
On Windows and Linux, Vibepollo runs as a service that the installer sets up; you do not start it by
hand. On Linux the session controller starts the host whenever the configured user's Plasma Wayland
session is active (see the [Linux install guide](linux/install.md)). Elsewhere, start it with:

```bash
vibepollo
```

> [!NOTE]
> Running multiple instances of Vibepollo is not advised.

### Specify config file
```bash
vibepollo <directory of conf file>/vibepollo.conf
```

> [!NOTE]
> This step is optional, you do not need to specify a config file.
> If no config file is entered, the default location will be used.
> The configuration file specified will be created if it doesn't exist.

### Headless Linux hosts
Vibepollo does not stream X11 sessions or manually started hosts. On a headless Linux machine keep
the SDDM or Plasma Login Manager greeter running: NVIDIA hosts stream the login screen itself and
you sign in from Moonlight, while AMD and Intel hosts need an autologin or a local sign-in before
the stream starts. The managed virtual display replaces dummy plugs.

### Configuration

Sunshine is configured via the web ui, which is available on [https://localhost:47990](https://localhost:47990)
by default. You may replace *localhost* with your internal ip address.

> [!NOTE]
> Ignore any warning given by your browser about "insecure website". This is due to the SSL certificate
> being self-signed.

> [!CAUTION]
> If running for the first time, make sure to note the username and password that you created.

1. Change the web-ui to your desired theme, using the dropdown menu in the navbar.
   ![Theme Selection](images/split-themes.png)
2. Add games and applications.
   ![Applications](images/applications.png)
3. Adjust any configuration settings as needed. You can search for options in the search bar.
   ![Configuration](images/configuration-search.png)
4. Find Moonlight clients and other tools for Sunshine in the `Featured Apps` tab.
   ![Featured Apps](images/featured-apps.png)
5. In Moonlight, you may need to add the PC manually.
6. When Moonlight requests for you insert the pin:

   - Login to the web-ui
   - Go to "PIN" in the Navbar
   - Type in your PIN and press `Enter`, and enter a name of your choosing for the device.
     You should get a Success Message!
   - In Moonlight, select one of the Applications listed

7. If you run into issues, logs are available in the `Troubleshooting` tab.
   You can navigate through each warning/error message for clues to the issue.
   ![Logs](images/troubleshooting-logs.png)

### Arguments
To get a list of available arguments, run the following command.

```bash
vibepollo --help
```

### Shortcuts
All shortcuts start with `Ctrl+Alt+Shift`, just like Moonlight.

* `Ctrl+Alt+Shift+N`: Hide/Unhide the cursor (This may be useful for Remote Desktop Mode for Moonlight)
* `Ctrl+Alt+Shift+F1/F12`: Switch to different monitor for Streaming

### Application List
* Applications should be configured via the web UI
* A basic understanding of working directories and commands is required
* You can use Environment variables in place of values
* `$(HOME)` will be replaced by the value of `$HOME`
* `$$` will be replaced by `$`, e.g. `$$(HOME)` will be become `$(HOME)`
* `env` - Adds or overwrites Environment variables for the commands/applications run by Sunshine.
  This can only be changed by modifying the `apps.json` file directly.

### Considerations
* On Windows, Sunshine uses the Desktop Duplication API which only supports capturing from the GPU used for display.
  If you want to capture and encode on the eGPU, connect a display or HDMI dummy display dongle to it and run the games
  on that display.
* When an application is started, if there is an application already running, it will be terminated.
* If any of the prep-commands fail, starting the application is aborted.
* When the application has been shutdown, the stream shuts down as well.

  * For example, if you attempt to run `steam` as a `cmd` instead of `detached` the stream will immediately fail.
    This is due to the method in which the steam process is executed. Other applications may behave similarly.
  * This does not apply to `detached` applications.

* The "Desktop" app works the same as any other application except it has no commands. It does not start an application,
  instead it simply starts a stream. If you removed it and would like to get it back, just add a new application with
  the name "Desktop" and "desktop.png" as the image path.
* If inputs (mouse, keyboard, gamepads...) aren't working after connecting:

  * On Linux the packaged host already belongs to the `vibepollo-uinput` group that owns
    `/dev/uinput` and `/dev/uhid`; check `journalctl -u vibepollo.service` for uinput errors.
  * On FreeBSD, add the user running Vibepollo to the `input` group.

* The FreeBSD version of Sunshine is missing some features that are present on Linux.
  The following are known limitations.

  * Only X11 and Wayland capture are supported
  * DualSense/DS5 emulation is not available due to missing uhid features


### HDR Support
Streaming HDR content is officially supported on Windows hosts and experimentally supported for Linux hosts.

* General HDR support information and requirements:

  * HDR must be activated in the host OS, which may require an HDR-capable physical display, an EDID
    emulator dongle, or a managed Vibepollo HDR virtual display connected to the desktop session.
  * You must also enable the HDR option in your Moonlight client settings, otherwise the stream will be SDR
    (and probably overexposed if your host is HDR).
  * A good HDR experience relies on proper HDR display calibration both in the OS and in game. HDR calibration can
    differ significantly between client and host displays.
  * You may also need to tune the brightness slider or HDR calibration options in game to the different HDR brightness
    capabilities of your client's display.
  * Some GPUs video encoders can produce lower image quality or encoding performance when streaming in HDR compared
    to SDR.

Additional information:

@tabs{
  @tab{ Windows |
  - HDR streaming is supported for Intel, AMD, and NVIDIA GPUs that support encoding HEVC Main 10 or AV1 10-bit profiles.
  - We recommend calibrating the display by streaming the Windows HDR Calibration app to your client device and saving an HDR calibration profile to use while streaming.
  - Older games that use NVIDIA-specific NVAPI HDR rather than native Windows HDR support may not display properly in HDR.
  }

@tab{ Linux |
  - HDR streaming is supported for Intel and AMD GPUs using VAAPI and NVIDIA GPUs using NVENC when
    the encoder supports HEVC Main 10 or AV1 10-bit profiles.
  - Managed HDR virtual displays use direct DRM/KMS capture so their 10-bit scanout reaches the
    encoder. KWin ScreenCast remains recommended for managed SDR capture. NvFBC and X11 capture do
    not support HDR.
  - You will need a desktop environment with a compositor that supports HDR rendering, such as Gamescope or KDE Plasma 6.
  - Native Vibepollo installations can provide private HDR10 virtual outputs through the
    `vibeshine_drm` module. It requires Linux 6.16 or newer and matching kernel headers. Its EDID
    advertises BT.2020, PQ, and HDR static metadata, while its connector and planes support 10-bit output.
    The driver notifies direct KMS capture when a presentation completes and exports the exact pinned
    primary-plane DMA-BUF for that sequence, so sparse changes are captured immediately and bursts are
    coalesced to the stream's requested maximum frame rate without re-querying KMS state. The managed
    output exposes only a primary plane, forcing the compositor to include cursors and overlays in that
    final framebuffer. Older modules without the frame-export ABI are rejected rather than polled.

  Native packages build the module and start the managed virtual-display pool automatically.
  To retry the build by hand or inspect the installed module:

  ```bash
  sudo vibepollo driver install
  modinfo vibeshine_drm
  ```

  Updating the module on disk does not replace one already held open by the
  compositor. Compare `modinfo -F version vibeshine_drm` with
  `cat /sys/module/vibeshine_drm/version`; reboot before testing when they
  differ. A working event-capable stream logs `Using event-driven KMS capture
  for Vibepollo DRM CRTC`.

  Native packages and `vibeshine-drm-setup.service` attempt the module build automatically; the
  first command retries it manually. Privileged helpers always install under the fixed, root-owned
  `/usr/libexec/vibeshine` path even when the application uses a custom prefix. The pool service
  provisions four dormant private outputs using the custom GPU-attached backend. If the module
  cannot be built or loaded, managed virtual displays remain unavailable rather than falling back
  to CPU-backed stock VKMS.

  On Arch Linux and CachyOS, install matching headers for every kernel you boot. If they are
  absent, the package asks pacman which installed package owns the running kernel and prints the
  exact `sudo pacman -S --needed <kernel-package>-headers` command. This covers standard, LTS,
  and CachyOS kernel variants without guessing a package name. The native package uses DKMS and
  signs rebuilt modules automatically and verifies the signer embedded in every installed module.
  On stock Arch and CachyOS kernels this all happens during package installation: accept pacman's
  normal install confirmation and no separate signing or enrollment command is required, including
  when Secure Boot uses a direct Limine or systemd-boot chain.

  Only a custom kernel configured to enforce trusted module signatures needs additional
  authorization. When that is detected, the package installation launches the one-time signing-key
  confirmation automatically. After confirming it, reboot and approve the pending firmware
  confirmations once; future kernel and Vibepollo updates remain automatic. If a noninteractive
  package frontend cannot display the prompt, retry the package installation from a terminal.

  @seealso{[Arch wiki on HDR Support for Linux](https://wiki.archlinux.org/title/HDR_monitor_support) and
  [Reddit Guide for HDR Support for AMD GPUs](https://www.reddit.com/r/linux_gaming/comments/10m2gyx/guide_alpha_test_hdr_on_linux)}
  }
}

### Tutorials and Guides
Tutorial videos are available [here](https://www.youtube.com/playlist?list=PLMYr5_xSeuXAbhxYHz86hA1eCDugoxXY0).

Guides are available [here](guides.md).

@admonition{Community! |
Tutorials and Guides are community generated. Want to contribute? Reach out to us on our discord server.}

### Version Status Messages
The Web UI provides detailed context about how your locally built Sunshine instance relates to the latest public release:

* Ahead: Your build's commit is ahead of the latest release tag (extra commits not yet part of a release). You will not be prompted to update.
* Behind: Your build is a number of commits behind the latest release; an update is recommended.
* Pre-release / Development: Non-`master` branch builds or builds that embed a short commit hash (or have a `.dirty` suffix) are treated as pre-release builds.
* Unknown Distance: If the GitHub compare API cannot be reached, a neutral message is shown instead of an update prompt.

Commit distance is determined using the GitHub compare endpoint between the latest release tag and the compiled commit hash.

<div class="section_buttons">

| Previous                 |                      Next |
|:-------------------------|--------------------------:|
| [Overview](../README.md) | [Changelog](changelog.md) |

</div>

<details style="display: none;">
  <summary></summary>
  [TOC]
</details>

[latest-release]: https://github.com/Nonary/Vibepollo/releases/latest
