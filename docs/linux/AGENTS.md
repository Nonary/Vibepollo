# Vibeshine Linux Agent Guide

This file is automatically loaded by AI coding assistants (OpenCode, Claude Code, etc.) when working in this repository. It contains authoritative knowledge about building, configuring, and troubleshooting Vibeshine on Linux — specifically Arch Linux / CachyOS with NVIDIA GPU and Wayland.

For full details on every topic, see **`LEARNINGS.md`**. This file is the quick-reference distillation.

---

## Repository Context

- **What**: Vibeshine — a Sunshine fork by Nonary with CUDA/NVENC, virtual display, WebRTC, and Playnite integration
- **Branch**: `vibe` (main development), `fix/linux-build-boost-1.89` (Linux build fixes)
- **Binary**: Installed at `~/.local/bin/sunshine` (symlink → versioned binary)
- **Build dir**: `~/vibeshine-build/build/`
- **Config**: `~/.config/sunshine/sunshine.conf`

---

## 1. Building on Linux (Arch / CachyOS)

### Required packages
```bash
sudo pacman -S cmake ninja gcc cuda nvidia-utils libva libdrm \
    avahi miniupnpc openssl opus libpulse pipewire libevdev \
    libcap libnotify npm doxygen graphviz boost
```

### Build commands
```bash
cd ~/vibeshine-build
mkdir -p build && cd build
cmake .. \
  -DCMAKE_INSTALL_PREFIX=~/.local \
  -DSUNSHINE_ENABLE_CUDA=ON \
  -DCUDA_TOOLKIT_ROOT_DIR=/opt/cuda
cmake --build . --parallel
cmake --install .
```

### After install — set capabilities (required for KMS capture)
```bash
sudo setcap cap_sys_admin+p ~/.local/bin/sunshine
```

### Boost 1.89+ patches (already applied on `fix/linux-build-boost-1.89`)
If building on a fresh clone and seeing Boost errors:

| File | Fix |
|------|-----|
| `src/boost_process_shim.h` | Add `#include <boost/process/v2/stdio.hpp>` and `<boost/process/v2/start_dir.hpp>` |
| `src/platform/linux/misc.cpp` | `v2::start_dir` → `v2::process_start_dir` |
| `src/process.cpp` | Wrap `display_helper_integration` in `#ifdef _WIN32` |
| `src/nvhttp.cpp` | Wrap `VirtualDisplayDriverReady` in `#ifdef _WIN32` |
| `src/config.cpp` | Wrap `apply_playnite()` in `#ifdef _WIN32` |
| `src/webrtc_stream.cpp` | Wrap WebRTC-only code in `#ifdef SUNSHINE_ENABLE_WEBRTC` |
| `third-party/Simple-Web-Server/CMakeLists.txt` | Remove `boost_system` (header-only in 1.89+) |

---

## 2. Virtual Display Setup

Vibeshine streams to a **virtual display** on HDMI-A-2 (a physically disconnected port) using a custom EDID loaded by the kernel at boot.

### How it works
1. A custom EDID binary is embedded in the initramfs
2. Kernel params `drm.edid_firmware=HDMI-A-2:edid/<file>` + `video=HDMI-A-2:e` force-enable HDMI-A-2 at boot
3. Sunshine is configured with `output_name = HDMI-A-2`
4. On client connect, `global_prep_cmd` switches to HDMI-A-2; on disconnect, restores HDMI-A-1

### EDID files
```
/usr/lib/firmware/edid/samsung-q800t-hdmi2.1   # patched: 2560x1600@120 as DTD2
/usr/lib/firmware/edid/y700-virtual.bin         # same patch applied
```
Both are patched from the original Samsung Q800T EDID to replace `2560x1440@120` → `2560x1600@120` (CVT-RB, 552.75 MHz).

### CRITICAL: Boot parameter persistence (Limine)
**`mkinitcpio -P` wipes custom kernel params from `limine.conf`.**
The fix is `/etc/kernel/cmdline` — this file is the persistent source for `limine-entry-tool`:
```
quiet nowatchdog splash drm.edid_firmware=HDMI-A-2:edid/samsung-q800t-hdmi2.1 video=HDMI-A-2:e rw rootflags=subvol=/@ root=UUID=<YOUR-UUID>
```
After any `mkinitcpio` run, verify: `sudo grep 'cmdline:' /boot/limine.conf | head -2`

### Display IDs (this machine)
- `HDMI-A-1` connector_id=133 → Physical Samsung LS27A600U, 2560x1440@75Hz
- `HDMI-A-2` connector_id=140 → Virtual display, 2560x1600@120Hz

### `/etc/mkinitcpio.conf`
```
FILES=(/usr/lib/firmware/edid/samsung-q800t-hdmi2.1)
```

---

## 3. Display Switching on Stream Connect/Disconnect

### Architecture
```
Boot              → ExecStartPre: enable HDMI-A-2, set mode
Client connects   → global_prep_cmd "do": switch-to-virtual.sh
Client disconnects → global_prep_cmd "undo": switch-to-physical.sh
Service stops     → ExecStopPost: restore HDMI-A-1
```

### `sunshine.conf` entry
```ini
global_prep_cmd = [{"do":"/home/$USER/.config/sunshine/scripts/switch-to-virtual.sh","undo":"/home/$USER/.config/sunshine/scripts/switch-to-physical.sh"}]
```

### Scripts location
```
~/.config/sunshine/scripts/switch-to-virtual.sh
~/.config/sunshine/scripts/switch-to-physical.sh
```

The scripts use `kscreen-doctor` and dynamically look up mode IDs by resolution string (not hardcoded index, since mode IDs can shift between boots).

### Systemd service override
```
~/.config/systemd/user/sunshine.service.d/override.conf
```

---

## 4. Audio Configuration

### PipeWire quantum — MUST match Sunshine's frame size
Sunshine reads audio in 5ms frames = **240 samples at 48kHz**.
Default PipeWire quantum (1024 samples) causes buffer mismatch → crackling.

```bash
# ~/.config/pipewire/pipewire.conf.d/99-sunshine-audio.conf
context.properties = {
    default.clock.rate = 48000
    default.clock.quantum = 240
    default.clock.min-quantum = 240
    default.clock.max-quantum = 2048
}
```
Verify: `pw-metadata -n settings 2>/dev/null | grep quantum`

### Use `virtual_sink` not `audio_sink`
```ini
# sunshine.conf — Sunshine creates and manages the sink lifecycle
virtual_sink = sink-sunshine-stereo
```
`virtual_sink` → Sunshine creates the null-sink, sets it as default, captures from it, restores on disconnect.
`audio_sink` → Sunshine captures from an existing sink but doesn't manage it.

### Audio format
The virtual sink is created as `float32le 2ch 48000Hz` (hardcoded in `src/platform/linux/audio.cpp`).
PipeWire handles conversion to your physical device format automatically.

---

## 5. Sunshine Configuration Reference

```ini
# ~/.config/sunshine/sunshine.conf (working config)

# Network
origin_web_ui_allowed = lan
upnp = on

# Display
output_name = HDMI-A-2          # Virtual display connector name
adapter_name = /dev/dri/renderD128

# FPS
fps = [30, 60, 90, 120]

# Audio
virtual_sink = sink-sunshine-stereo

# Encoder (NVENC + KMS for lowest latency)
encoder = nvenc
capture = kms
nvenc_preset = 1
nvenc_twopass = disabled
nvenc_latency_over_power = enabled
hevc_mode = 2

# Streaming
minimum_fps_target = 30
fec_percentage = 40
max_bitrate = 80000

# Display switching
global_prep_cmd = [{"do":"/home/$USER/.config/sunshine/scripts/switch-to-virtual.sh","undo":"/home/$USER/.config/sunshine/scripts/switch-to-physical.sh"}]
```

---

## 6. Firewall (UFW)

```bash
sudo ufw allow 47984/tcp comment 'Sunshine RTSP'
sudo ufw allow 47989/tcp comment 'Sunshine GameStream'
sudo ufw allow 47990/tcp comment 'Sunshine Web UI'
sudo ufw allow 48010/tcp comment 'Sunshine Video'
sudo ufw allow 47998/udp comment 'Sunshine Video 1'   # CRITICAL
sudo ufw allow 47999/udp comment 'Sunshine Control'   # CRITICAL
sudo ufw allow 48000/udp comment 'Sunshine Video 2'   # CRITICAL
sudo ufw allow 48002/udp comment 'Sunshine Video 3'
sudo ufw allow 5353/udp  comment 'mDNS'
sudo ufw allow from 192.168.0.0/16 comment 'Local Network'
sudo ufw reload
```

UDP 47998/48000 are the most critical — without them, "no video received" error appears on client.

---

## 7. Common Diagnostics

```bash
# Service status and recent logs
systemctl --user status sunshine
journalctl --user -u sunshine -f

# Check which display Sunshine detected
journalctl --user -u sunshine | grep -E "connector|Monitor|HDMI"

# Check virtual display is present
kscreen-doctor -o | grep -E "Output:|enabled|2560"

# Check kernel loaded EDID
cat /proc/cmdline | grep edid
dmesg | grep -i "edid\|HDMI-A-2"

# PipeWire quantum
pw-metadata -n settings 2>/dev/null | grep quantum

# Audio sinks
pactl list sinks short

# Capabilities
getcap ~/.local/bin/sunshine

# mDNS discovery
avahi-browse -r _nvstream._tcp -t
```

---

## 8. Known Issues & Workarounds

| Issue | Cause | Fix |
|-------|-------|-----|
| Virtual display gone after kernel update | `mkinitcpio` wipes limine.conf params | Create `/etc/kernel/cmdline` with full cmdline |
| Sunshine uses HDMI-A-1 instead of HDMI-A-2 | HDMI-A-2 not initialized at boot | Check `/proc/cmdline` for `drm.edid_firmware` |
| Audio crackling on tablet | PipeWire quantum mismatch with Sunshine's 5ms frames | Set quantum=240 in pipewire conf |
| "no video received" on client | UDP ports blocked | Open 47998, 47999, 48000 UDP in UFW |
| KMS capture "Failed to gain CAP_SYS_ADMIN" | Binary missing capability | `sudo setcap cap_sys_admin+p ~/.local/bin/sunshine` |
| Resolution still 2560x1440 after EDID patch | EDID not in initramfs or boot params missing | `sudo mkinitcpio -P`, verify `/proc/cmdline` |

---

## 9. Files That Matter

| File | Purpose |
|------|---------|
| `~/.config/sunshine/sunshine.conf` | Main Sunshine config |
| `~/.config/systemd/user/sunshine.service.d/override.conf` | Systemd service customization |
| `~/.config/sunshine/scripts/switch-to-virtual.sh` | Enable virtual display on connect |
| `~/.config/sunshine/scripts/switch-to-physical.sh` | Restore physical display on disconnect |
| `~/.config/pipewire/pipewire.conf.d/99-sunshine-audio.conf` | PipeWire quantum tuning |
| `/etc/kernel/cmdline` | Persistent kernel boot params (Limine) |
| `/usr/lib/firmware/edid/samsung-q800t-hdmi2.1` | Patched EDID (2560x1600@120 as DTD2) |
| `/etc/mkinitcpio.conf` | Must include EDID in `FILES=` |
| `~/vibeshine-build/LEARNINGS.md` | Full detailed learnings log |

---

## 10. System Info (Reference Machine)

| Component | Value |
|-----------|-------|
| OS | CachyOS (Arch Linux) |
| Kernel | linux-cachyos 6.19.5 |
| GPU | NVIDIA RTX 3080 Ti |
| Driver | 590.48.01 |
| CUDA | 13.1 (at /opt/cuda) |
| Display server | Wayland (KDE Plasma) |
| Bootloader | Limine |
| Init system | systemd |
| Audio | PipeWire 1.4.10 (PulseAudio compat) |
| Physical monitor | HDMI-A-1, Samsung LS27A600U, 2560x1440@75Hz |
| Virtual display | HDMI-A-2, 2560x1600@120Hz (EDID firmware) |
| Streaming target | Lenovo Y700 tablet, 2560x1600, 120Hz |
