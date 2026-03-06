# Vibeshine Linux Build & Setup - Learnings

## Overview

This document captures all learnings from building and configuring Vibeshine (a Sunshine fork) on Arch Linux with Boost 1.89+, NVIDIA GPU, and Wayland.

---

## 1. Build Fixes for Linux (Boost 1.89+)

### Problem
Vibeshine/Sunshine fails to build on Linux with Boost 1.89+ due to API changes and Windows-only code not properly guarded.

### Files Modified

| File | Issue | Fix |
|------|-------|-----|
| `src/boost_process_shim.h` | Missing Boost.Process v2 headers | Added `#include <boost/process/v2/stdio.hpp>` and `<boost/process/v2/start_dir.hpp>` |
| `src/platform/linux/misc.cpp` | API rename in Boost 1.89 | Changed `v2::start_dir` → `v2::process_start_dir` |
| `src/process.cpp` | Windows-only code | Wrapped `display_helper_integration` in `#ifdef _WIN32` |
| `src/nvhttp.cpp` | Windows-only VDISPLAY code | Wrapped `VirtualDisplayDriverReady` in `#ifdef _WIN32` |
| `src/config.cpp` | Windows-only Playnite | Wrapped `apply_playnite()` in `#ifdef _WIN32` |
| `src/webrtc_stream.cpp` | WebRTC-only code | Wrapped in `#ifdef SUNSHINE_ENABLE_WEBRTC` |
| `third-party/Simple-Web-Server/CMakeLists.txt` | `boost_system` link error | Removed `boost_system` (header-only in Boost 1.89+) |

### Build Commands
```bash
# Install dependencies (Arch Linux)
sudo pacman -S cmake ninja gcc cuda nvidia-utils libva libvdpau \
    avahi miniupnpc openssl opus libpulse libpipewire libdrm \
    libevdev libcap libnotify libayatana-appindicator

# Clone and build
git clone https://github.com/Nonary/vibeshine.git
cd vibeshine
git checkout vibe
git checkout -b fix/linux-build-boost-1.89
# Apply patches...
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=~/.local -DSUNSHINE_ENABLE_CUDA=ON
cmake --build . --parallel
cmake --install .
```

---

## 2. Network Configuration

### Problem
Vibeshine not discoverable on other devices - no video streaming working.

### Root Causes
1. **UFW Firewall** was blocking required ports
2. **mDNS** blocked between wired/wireless clients on some routers
3. **UDP ports** not open for video streaming

### Required Ports

| Port | Protocol | Purpose |
|------|----------|---------|
| 47989 | TCP | GameStream pairing |
| 47990 | TCP | Web UI (HTTPS) |
| 47984 | TCP | RTSP |
| 48010 | TCP | Video/control |
| **47998** | **UDP** | **Video streaming (CRITICAL)** |
| **47999** | **UDP** | **Control channel (CRITICAL)** |
| **48000** | **UDP** | **Video streaming (CRITICAL)** |
| 5353 | UDP | mDNS/Bonjour discovery |

### UFW Firewall Rules
```bash
# TCP ports
sudo ufw allow 47989/tcp comment 'Sunshine GameStream'
sudo ufw allow 47990/tcp comment 'Sunshine Web UI'
sudo ufw allow 47984/tcp comment 'Sunshine RTSP'
sudo ufw allow 48010/tcp comment 'Sunshine Video'

# UDP ports (CRITICAL for video)
sudo ufw allow 47998/udp comment 'Sunshine Video 1'
sudo ufw allow 47999/udp comment 'Sunshine Control'
sudo ufw allow 48000/udp comment 'Sunshine Video 2'
sudo ufw allow 48002/udp comment 'Sunshine Video 3'
sudo ufw allow 5353/udp comment 'mDNS'

# Local network
sudo ufw allow from 192.168.0.0/16 comment 'Local Network'

sudo ufw reload
```

### Sunshine Configuration (`~/.config/sunshine/sunshine.conf`)
```ini
origin_web_ui_allowed = lan
upnp = on
```

---

## 3. KMS Capture Setup (Wayland)

### Problem
`Error: Failed to gain CAP_SYS_ADMIN` - KMS capture not working on Wayland.

### Solution
Set capabilities on the Sunshine binary:
```bash
sudo setcap cap_sys_admin+p $(readlink -f $(which sunshine))
```

Or for custom install:
```bash
sudo setcap cap_sys_admin+p ~/.local/bin/sunshine
```

### Alternative: Use sunshine-kms service
The logs recommend using `sunshine-kms` service instead of regular `sunshine` service for KMS capture. Check Sunshine documentation for setup.

---

## 4. Video Scaling Errors

### Problem
```
Error: Couldn't scale frame: Invalid argument
Error: Could not convert image
```

### Causes
1. **Resolution mismatch**: Monitor is 2560x1440 but capture is 1920x1080
2. **DMA-BUF format issues**: XDG portal returning incompatible buffer formats
3. **Color space conversion errors**: NVENC expecting different format

### Potential Fixes
1. **Match resolutions**: Set monitor to 1920x1080 or configure Sunshine to capture at native resolution
2. **Try different capture method**: Switch between XDG Portal, KMS, or NvFBC
3. **Check NVIDIA driver**: Ensure latest drivers with proper DMA-BUF support
4. **Set encoder color space**: Configure encoder to match captured format

### In Web UI, check:
- Video > Encoder: `nvenc`
- Video > Capture: Try `KMS` or `XDG Portal`
- Video > Resolution: Match or lower than monitor

---

## 5. System Requirements

### User Groups
```bash
# Add user to required groups
sudo usermod -aG input $USER
sudo usermod -aG video $USER
sudo usermod -aG render $USER
```

### Service Setup
```bash
# Create systemd override for custom binary
mkdir -p ~/.config/systemd/user/sunshine.service.d
cat > ~/.config/systemd/user/sunshine.service.d/override.conf << 'EOF'
[Service]
ExecStart=
ExecStart=/home/$USER/.local/bin/sunshine
EOF

# Enable and start
systemctl --user daemon-reload
systemctl --user enable sunshine
systemctl --user start sunshine
```

---

## 6. mDNS/Avahi Debugging

### Check Broadcasting
```bash
# Check if Sunshine is broadcasting
avahi-browse -r _nvstream._tcp -t

# Should show:
# hostname = [hostname.local]
# address = [192.168.x.x]
# port = [47989]
```

### Manual Connection
If mDNS doesn't work (router blocking), add host manually:
1. Open Moonlight client
2. Add host: `192.168.x.x`
3. Pair with PIN from Web UI

---

## 7. Debugging Commands

```bash
# Check service status
systemctl --user status sunshine

# View logs
journalctl --user -u sunshine -f

# Check open ports
ss -tulnp | grep sunshine

# Check mDNS
avahi-browse -a -t | grep nvstream

# Test Web UI
curl -sk https://localhost:47990 | head -5

# Check capabilities
getcap $(readlink -f ~/.local/bin/sunshine)

# Check groups
groups $USER | grep -E "input|video|render"
```

---

## 8. Files Created

| File | Purpose |
|------|---------|
| `~/setup-sunshine-ufw.sh` | UFW firewall setup script |
| `~/.config/sunshine/sunshine.conf` | Sunshine configuration |
| `~/.config/systemd/user/sunshine.service.d/override.conf` | Systemd service override |
| `~/0001-fix-Linux-build-compatibility-for-Boost-1.89.patch` | Build fix patch |

---

## 9. Key Learnings Summary

1. **Boost 1.89+ requires code changes** - Many API changes from older Boost versions
2. **Windows-only code needs `#ifdef _WIN32`** - Playnite, VDISPLAY, display_helper are Windows-only
3. **UDP ports are critical** - TCP alone won't work; video streams over UDP
4. **UFW blocks by default** - Must explicitly allow all Sunshine ports
5. **KMS capture needs CAP_SYS_ADMIN** - Set capabilities or use sunshine-kms service
6. **mDNS may be blocked by router** - Manual IP connection works around this
7. **Resolution mismatch causes scaling errors** - Match capture to display resolution

---

## 10. References

- [Sunshine Documentation](https://docs.lizardbyte.dev/projects/sunshine/latest/)
- [Vibeshine GitHub](https://github.com/Nonary/vibeshine)
- [Moonlight Game Streaming Ports](https://portforward.com/moonlight-game-streaming/)
- [Sunshine Getting Started](https://docs.lizardbyte.dev/projects/sunshine/latest/md_docs_2getting__started.html)

---

## 11. Virtual Display Setup (EDID Method)

### Problem
Need to stream without physical monitor, or stream while monitor is off.

### Solution: EDID Virtual Display
Load a custom EDID file via kernel parameters to create a virtual display on an unused video output.

### Step 1: Find Available Output
```bash
for p in /sys/class/drm/*/status; do con=${p%/status}; echo -n "${con#*/card?-}: "; cat $p; done
# Look for "disconnected" outputs
```

### Step 2: Download EDID File
```bash
sudo mkdir -p /usr/lib/firmware/edid

# Option A: Use existing monitor's EDID
sudo cat /sys/class/drm/card1-HDMI-A-1/edid > /tmp/monitor.bin
sudo cp /tmp/monitor.bin /usr/lib/firmware/edid/virtual-display.bin

# Option B: Download 4K 120Hz HDR EDID (samsung-q800t-hdmi2.1)
curl -sL "https://git.linuxtv.org/v4l-utils.git/plain/utils/edid-decode/data/samsung-q800t-hdmi2.1" -o /tmp/samsung-q800t-hdmi2.1
sudo cp /tmp/samsung-q800t-hdmi2.1 /usr/lib/firmware/edid/
```

### Step 3: Configure Kernel Parameters

#### For GRUB:
```bash
sudo nano /etc/default/grub
# Add to GRUB_CMDLINE_LINUX:
drm.edid_firmware=HDMI-A-2:edid/samsung-q800t-hdmi2.1 video=HDMI-A-2:e

sudo update-grub
```

#### For Limine (CachyOS):
```bash
sudo nano /boot/limine.conf
# Add to cmdline:
drm.edid_firmware=HDMI-A-2:edid/samsung-q800t-hdmi2.1 video=HDMI-A-2:e
```

### Step 4: Add EDID to Initramfs
```bash
sudo nano /etc/mkinitcpio.conf
# Add to FILES:
FILES=(/usr/lib/firmware/edid/samsung-q800t-hdmi2.1)

sudo mkinitcpio -P
```

### Step 5: Configure Sunshine
In Sunshine Web UI (https://localhost:47990):
1. Go to **Configuration > Audio/Video**
2. Set **Output Name** to the virtual display (e.g., `HDMI-A-2`)
3. Or edit `~/.config/sunshine/sunshine.conf`:
```ini
output_name = HDMI-A-2
```

### Step 6: Reboot
```bash
sudo reboot
```

### Verify Virtual Display
```bash
# Check if virtual display is detected
xrandr --query | grep connected
# or
cat /sys/class/drm/card*/status
```

### Alternative Methods

#### Headless Sway (Dynamic Resolution)
- Runs separate Wayland compositor for streaming
- Dynamic resolution matching to client
- See: https://github.com/daaaaan/sunshine-headless-sway

#### Dummy Display Script (evtest)
- Automated toggle with physical display
- See: https://github.com/TheRealHoobi/Sunshine-Dummy-Display-for-linux

---

## 12. Auto-Login Setup (SDDM)

### Disable Login Screen
```bash
sudo nano /etc/sddm.conf
```
Add:
```ini
[Autologin]
User=$USER
Session=plasma
```

---

## 13. Passwordless Sudo (Optional)

### Full Passwordless Sudo
```bash
echo "$USER ALL=(ALL) NOPASSWD: ALL" | sudo tee /etc/sudoers.d/$USER
```

### Specific Commands Only
```bash
echo "$USER ALL=(ALL) NOPASSWD: /usr/bin/ufw, /usr/bin/setcap, /usr/bin/mkinitcpio" | sudo tee /etc/sudoers.d/sunshine-commands
```

---

## 14. Complete Setup Summary

After all steps, you should have:

| Component | Status | Verification |
|-----------|--------|--------------|
| Vibeshine binary | Installed | `which sunshine` |
| UDP ports | Open | `sudo ufw status` |
| CAP_SYS_ADMIN | Set | `getcap $(which sunshine)` |
| User groups | Added | `groups $USER` |
| Virtual display | Configured | `xrandr --query` |
| Auto-login | Enabled | `/etc/sddm.conf` |
| Sunshine service | Running | `systemctl --user status sunshine` |

### Quick Test
1. Open Moonlight on client device
2. Connect to host (auto-discover or manual IP)
3. Enter PIN from https://localhost:47990
4. Stream!

---

## 15. Troubleshooting

### No video received
1. Check UDP ports: `sudo ufw status | grep udp`
2. Check Sunshine logs: `journalctl --user -u sunshine -f`
3. Verify virtual display: `xrandr --query`

### "Failed to gain CAP_SYS_ADMIN"
```bash
sudo setcap cap_sys_admin+p $(readlink -f $(which sunshine))
```

### Scaling errors
1. Match Sunshine resolution to virtual display
2. Try different capture method (KMS vs XDG Portal)
3. Check NVIDIA driver version

### Virtual display not detected
1. Verify EDID file in initramfs: `lsinitcpio /boot/initramfs-linux.img | grep edid`
2. Check kernel parameters: `cat /proc/cmdline`
3. Try different output port
4. **Critical**: If using Limine bootloader, kernel params may be wiped on kernel update — see §16

---

## 16. Boot Parameter Persistence (Limine Bootloader)

### Problem
On CachyOS/Arch with Limine bootloader, running `mkinitcpio -P` (e.g. after updating EDID in initramfs) triggers `limine-entry-tool`, which **overwrites** `limine.conf` and strips any custom kernel parameters like `drm.edid_firmware` and `video=HDMI-A-2:e`. The virtual display then silently disappears on next boot.

### Symptom
- `cat /proc/cmdline` is missing `drm.edid_firmware` after reboot
- HDMI-A-2 no longer appears in `kscreen-doctor -o`
- Sunshine logs show connector ID 133 (physical) instead of 140 (virtual)

### Fix: Persist params via `/etc/kernel/cmdline`
`limine-entry-tool` reads `/etc/kernel/cmdline` if it exists, using it as the base for all future `limine.conf` writes.
```bash
# Create this file with your full desired cmdline
sudo tee /etc/kernel/cmdline << 'EOF'
quiet nowatchdog splash drm.edid_firmware=HDMI-A-2:edid/samsung-q800t-hdmi2.1 video=HDMI-A-2:e rw rootflags=subvol=/@ root=UUID=<YOUR-UUID>
EOF
```
After this, `mkinitcpio -P` will no longer wipe your custom params.

### Also fix limine.conf immediately
If params were already wiped, restore them manually before the next reboot:
```bash
sudo grep -n 'cmdline:' /boot/limine.conf | head -5
# Edit lines for your main boot entries (not snapshot entries)
sudo nano /boot/limine.conf
# Add: drm.edid_firmware=HDMI-A-2:edid/samsung-q800t-hdmi2.1 video=HDMI-A-2:e
```

---

## 17. Custom EDID Resolution (Patching Binary EDID)

### Problem
The stock Samsung Q800T EDID (used for virtual display) only advertises `2560x1440@120` as its highest non-4K mode. Streaming to a Lenovo Y700 tablet (native `2560x1600`) meant the virtual display never offered that resolution.

### Solution: Patch the EDID binary
The EDID DTD (Detailed Timing Descriptor) is an 18-byte structure. Replace the `2560x1440@120` DTD with a `2560x1600@120` CVT Reduced Blanking entry:

```python
#!/usr/bin/env python3
# Patch EDID: replace 2560x1440@120 with 2560x1600@120 (CVT-RB)
# CVT-RB modeline: 552.75 2560 2608 2640 2720 1600 1603 1609 1694 +hsync -vsync

old_dtd = bytes.fromhex('6fc200a0a0a0555030203500501d7400001a')  # 2560x1440@120
new_dtd = bytes.fromhex('ebd700a0a0405e6030203600501d7400001a')  # 2560x1600@120 RB

data = bytearray(open('/usr/lib/firmware/edid/samsung-q800t-hdmi2.1', 'rb').read())
i = 0
while i <= len(data) - 18:
    if data[i:i+18] == old_dtd:
        data[i:i+18] = new_dtd
    i += 1

# Recalculate checksums for each 128-byte block
for block in range(len(data) // 128):
    offset = block * 128
    s = sum(data[offset:offset+127]) % 256
    data[offset+127] = (256 - s) % 256

open('/usr/lib/firmware/edid/samsung-q800t-hdmi2.1', 'wb').write(data)
```

After patching, regenerate initramfs: `sudo mkinitcpio -P`

### Key numbers
| Field | Value |
|-------|-------|
| Pixel clock | 552.75 MHz (fits in EDID 1.3's 655 MHz max) |
| CVT modeline | `552.75 2560 2608 2640 2720 1600 1603 1609 1694 +hsync -vsync` |
| Old DTD hex | `6fc200a0a0a0555030203500501d7400001a` |
| New DTD hex | `ebd700a0a0405e6030203600501d7400001a` |

### Verify
```bash
edid-decode /usr/lib/firmware/edid/samsung-q800t-hdmi2.1 2>/dev/null | grep 'DTD 2'
# Expected: DTD 2:  2560x1600  119.96 Hz  16:10   203.217 kHz    552.750000 MHz
```

---

## 18. Virtual Display Switching: global_prep_cmd vs ExecStartPre

### Architecture
There are two distinct moments to switch displays:

| Moment | Mechanism | Use |
|--------|-----------|-----|
| Service start (boot) | `ExecStartPre` in systemd override | Enable virtual display at boot so Sunshine can detect it |
| Client connect/disconnect | `global_prep_cmd` in `sunshine.conf` | Switch to virtual on connect, restore physical on disconnect |

Both are needed. `ExecStartPre` ensures HDMI-A-2 is active when Sunshine starts and scans displays. `global_prep_cmd` handles the per-stream lifecycle.

### global_prep_cmd format
```ini
# sunshine.conf
global_prep_cmd = [{"do":"/path/to/switch-to-virtual.sh","undo":"/path/to/switch-to-physical.sh"}]
```
- `do` runs when a client **connects** (before stream starts)
- `undo` runs when the **last client disconnects** (after stream ends)
- If `do` exits non-zero, stream is aborted

### switch-to-virtual.sh
```bash
#!/bin/bash
export WAYLAND_DISPLAY=wayland-0
export XDG_RUNTIME_DIR=/run/user/1000
export DISPLAY=:0
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus

kscreen-doctor "output.HDMI-A-2.enable" "output.HDMI-A-2.primary" "output.HDMI-A-1.disable" 2>&1

# Set correct resolution (mode ID varies — look it up dynamically)
sleep 1
MODE_ID=$(kscreen-doctor -o 2>/dev/null | grep -oP '\d+:2560x1600@120\.\d+' | head -1 | cut -d: -f1)
if [ -n "$MODE_ID" ]; then
    kscreen-doctor "output.HDMI-A-2.mode.$MODE_ID" 2>&1
fi
```

### switch-to-physical.sh
```bash
#!/bin/bash
export WAYLAND_DISPLAY=wayland-0
export XDG_RUNTIME_DIR=/run/user/1000
export DISPLAY=:0
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus

kscreen-doctor "output.HDMI-A-1.enable" "output.HDMI-A-1.primary" "output.HDMI-A-2.disable" 2>&1
```

### systemd override
```ini
# ~/.config/systemd/user/sunshine.service.d/override.conf
[Service]
ExecStart=
ExecStartPre=/bin/sleep 5
ExecStartPre=/bin/bash -c 'export WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus DISPLAY=:0 && kscreen-doctor output.HDMI-A-2.enable output.HDMI-A-2.primary output.HDMI-A-1.disable 2>/dev/null; sleep 2; MODE_ID=$(kscreen-doctor -o 2>/dev/null | grep -oP "\\d+:2560x1600@120\\.\\d+" | head -1 | cut -d: -f1); [ -n "$MODE_ID" ] && kscreen-doctor "output.HDMI-A-2.mode.$MODE_ID" 2>/dev/null; true'
ExecStart=/home/$USER/.local/bin/sunshine
ExecStopPost=/bin/bash -c 'export WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus DISPLAY=:0 && kscreen-doctor output.HDMI-A-1.enable output.HDMI-A-1.primary output.HDMI-A-2.disable 2>/dev/null'
```

---

## 19. Audio Crackling Fix: PipeWire Quantum Mismatch

### Root Cause
Vibeshine reads audio using PulseAudio's `pa_simple` API with `fragsize = frame_size * channels * sizeof(float)`.

- Sunshine's audio frame size = `packetDuration * sampleRate / 1000` = `5ms * 48000 / 1000` = **240 samples**
- PipeWire's default quantum was **1024 samples** (~21ms)
- `pa_simple_read()` blocks until `fragsize` bytes are available, but PipeWire delivers data in 1024-sample chunks
- This misalignment causes timing jitter and buffer underruns → **crackling artifacts**

### Fix: Set PipeWire quantum to 240
```bash
mkdir -p ~/.config/pipewire/pipewire.conf.d
cat > ~/.config/pipewire/pipewire.conf.d/99-sunshine-audio.conf << 'EOF'
context.properties = {
    default.clock.rate = 48000
    # Match Sunshine's 5ms audio frame (240 samples @ 48kHz)
    default.clock.quantum = 240
    default.clock.min-quantum = 240
    default.clock.max-quantum = 2048
}
EOF
systemctl --user restart pipewire pipewire-pulse
```

### Verify
```bash
pw-metadata -n settings 2>/dev/null | grep quantum
# Expected: clock.quantum = '240'
```

---

## 20. Audio Config: virtual_sink vs audio_sink

### Difference
| Setting | Behaviour |
|---------|-----------|
| `audio_sink = <name>` | Capture from an existing sink's monitor. Host audio is NOT muted. You manage the sink yourself. |
| `virtual_sink = <name>` | Sunshine creates the virtual null-sink, sets it as system default, captures from its monitor, and restores host audio on disconnect. |

### Recommendation: use `virtual_sink`
```ini
# ~/.config/sunshine/sunshine.conf
virtual_sink = sink-sunshine-stereo
```
Sunshine will create `sink-sunshine-stereo` (float32le, 2ch, 48kHz) via PulseAudio's `module-null-sink` and manage its lifecycle. No manual sink setup needed.

### How Vibeshine creates the sink (source reference)
In `src/platform/linux/audio.cpp`:
```cpp
// Format: PA_SAMPLE_FLOAT32, 48000 Hz, 2ch (stereo) or 6/8ch (surround)
pa_sample_spec ss {PA_SAMPLE_FLOAT32, sample_rate, (std::uint8_t) channels};
// fragsize matches Sunshine's frame size exactly
pa_buffer_attr pa_attr = { .fragsize = uint32_t(frame_size * channels * sizeof(float)) };
```
The virtual sink runs at `float32le` — PipeWire handles format conversion to your physical audio device automatically.

---

## 21. CUDA Build on Arch Linux

### Install CUDA
```bash
sudo pacman -S cuda
# CUDA installs to /opt/cuda
```

### Build with CUDA
```bash
cmake .. \
  -DCMAKE_INSTALL_PREFIX=~/.local \
  -DSUNSHINE_ENABLE_CUDA=ON \
  -DCUDA_TOOLKIT_ROOT_DIR=/opt/cuda
cmake --build . --parallel
```

### Verify CUDA symbols in binary
```bash
nm ~/.local/bin/sunshine | grep -i cuda | head -5
# Should show CUDA symbols if compiled correctly
```

### Set capabilities after install
```bash
sudo setcap cap_sys_admin+p ~/.local/bin/sunshine
# Verify
getcap ~/.local/bin/sunshine
# Expected: /home/user/.local/bin/sunshine cap_sys_admin=p
```

---

## 22. Complete Working Configuration

### System Setup Summary (Arch Linux / CachyOS, NVIDIA, Wayland/KDE)

#### Hardware
- Host GPU: NVIDIA RTX 3080 Ti, Driver 590.48.01
- Physical monitor: Samsung LS27A600U (HDMI-A-1), 2560x1440@75Hz
- Virtual display: HDMI-A-2 (via EDID firmware), 2560x1600@120Hz
- Streaming target: Lenovo Y700 tablet, 2560x1600, 120Hz

#### `~/.config/sunshine/sunshine.conf`
```ini
# Network
origin_web_ui_allowed = lan
upnp = on

# Display - stream to virtual display
output_name = HDMI-A-2
adapter_name = /dev/dri/renderD128

# FPS
fps = [30, 60, 90, 120]

# Audio - Sunshine manages virtual sink lifecycle
virtual_sink = sink-sunshine-stereo

# Encoder
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

# Display switching on client connect/disconnect
global_prep_cmd = [{"do":"/home/$USER/.config/sunshine/scripts/switch-to-virtual.sh","undo":"/home/$USER/.config/sunshine/scripts/switch-to-physical.sh"}]
```

#### `/etc/kernel/cmdline` (persistent boot params)
```
quiet nowatchdog splash drm.edid_firmware=HDMI-A-2:edid/samsung-q800t-hdmi2.1 video=HDMI-A-2:e rw rootflags=subvol=/@ root=UUID=<UUID>
```

#### `~/.config/pipewire/pipewire.conf.d/99-sunshine-audio.conf`
```ini
context.properties = {
    default.clock.rate = 48000
    default.clock.quantum = 240
    default.clock.min-quantum = 240
    default.clock.max-quantum = 2048
}
```

#### `/etc/mkinitcpio.conf` (EDID in initramfs)
```
FILES=(/usr/lib/firmware/edid/samsung-q800t-hdmi2.1)
```
