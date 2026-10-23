# Troubleshooting

## General

### Forgotten Credentials
If you forgot your credentials to the web UI, try this.

@tabs{
  @tab{General | ```bash
    vibepollo --creds {new-username} {new-password}
    ```
  }
  @tab{AppImage | ```bash
    ./vibepollo.AppImage --creds {new-username} {new-password}
    ```
  }
  @tab{Flatpak | ```bash
    flatpak run --command=vibepollo io.github.Nonary.vibepollo --creds {new-username} {new-password}
    ```
  }
}

> [!TIP]
> Remember to replace `{new-username}` and `{new-password}` with your new credentials.
> Do not include the curly braces.

### Unusual Mouse Behavior
If you experience unusual mouse behavior, try attaching a physical mouse to the Sunshine host.

### Web UI Access
Can't access the web UI?

1. Enter the full **HTTPS** URL. On the host, the default is
   [https://localhost:47990](https://localhost:47990). From another LAN device, use
   the host's LAN IP address, for example `https://192.168.1.10:47990` (replace the
   example address with your host's address). Do not omit `https://`: a bare
   `192.168.1.10:47990` can make the browser use plain HTTP and display
   `ERR_EMPTY_RESPONSE` instead of opening the Web UI. The Web UI port accepts HTTPS,
   not plain HTTP, and does not redirect HTTP requests to HTTPS.
2. Check the host log. The warning
   `Configuration HTTPS listener: plain HTTP received on the TLS port; use an https:// URL.`
   means the request reached the listener using the wrong protocol. Correct the URL;
   changing `origin_web_ui_allowed` to `wan` or opening firewall ports will not fix
   that protocol mismatch. If you changed the base `port` setting, use the Web UI
   port in the `Configuration UI available at` startup message. When connecting
   from another device, replace a loopback host in that message with the host's LAN
   IP address, keeping the HTTPS scheme and port.
3. A certificate warning after switching to HTTPS is a separate trust issue: the
   default certificate is self-signed. See the [configuration introduction](getting_started.md#configuration).
   Do not enter credentials if you cannot verify the host's identity. Plain HTTP is
   not a safe workaround for accessing the authenticated administration interface.
4. If the correct HTTPS URL still cannot connect, check that Vibepollo is running,
   that its bind address and `origin_web_ui_allowed` permit the intended LAN access,
   and that firewall rules allow the configured Web UI port on the intended network.

### Zero-filled auxiliary state prevents crash dismissal or token saves

If both `vibeshine_state.json` and `vibeshine_state.json.bak` are empty or contain
only zero bytes after an unexpected shutdown, their contents cannot be recovered.
The host deliberately refuses to overwrite them. A newer file writer prevents
known write/flush failures from replacing an existing target, but does not repair
already-lost data or establish power-loss durability on every storage device.

The native `--auxiliary-recovery` command can prepare an **explicit reset**, not a
repair. It does not start the host or install anything. Resetting auxiliary state
loses API tokens, Web UI sessions, app-ID aliases, display metadata and crash/update
notification metadata. API integrations need new tokens, the Web UI needs a fresh
login, and display settings/launch shortcuts may need reconfiguration. It does
**not** reset the Web UI username/password or Moonlight pairings.

1. Stop the host/service and keep a private copy of the entire configuration
   directory. This disconnects active streams. Confirm that the configured
   auxiliary file is the separate `vibeshine_state.json`, **not** the pairing or
   credentials file. Custom/shared filenames are unsupported by this command;
   do not rename a pairing/credential file to bypass this check. The command
   runs before host configuration is parsed and cannot verify this separation
   from zero-filled copies. Copy the auxiliary
   file and its `.bak` into `C:\PrivateRecovery`. Never upload either snapshot.
2. In PowerShell, inspect those two copies:

   ```powershell
   & 'C:\Program Files\Sunshine\sunshine.exe' --auxiliary-recovery inspect 'C:\PrivateRecovery\vibeshine_state.json'
   ```

   Adjust the executable path for your installation. Both snapshots must exist
   and be entirely empty/zero-filled. A valid, partially readable, missing or
   inaccessible snapshot requires separate investigation, not this reset. Copy
   the `snapshot_token` from the report.
3. If you accept the listed data loss, prepare the reset. Replace `TOKEN` with
   that value; `C:\PrivateRecovery\reset` must not exist:

   ```powershell
   & 'C:\Program Files\Sunshine\sunshine.exe' --auxiliary-recovery prepare-reset 'C:\PrivateRecovery\vibeshine_state.json' --expect-snapshot TOKEN --accept-token-and-metadata-loss --confirm-separate-auxiliary-state --output 'C:\PrivateRecovery\reset'
   ```

   Success creates private copies of both originals, two identical reset
   candidates and `recovery.json`. Check that the manifest says `complete: true`.
   Do not install files from an incomplete output directory. No source file,
   pairing state, password or running configuration was changed by the command.
4. With the host still stopped, copy `vibeshine_state.reset.json` and
   `vibeshine_state.reset.json.bak` from the output directory to the actual
   configuration directory as `vibeshine_state.json` and `vibeshine_state.json.bak`.
   Replace **only these two auxiliary files**, retaining the originals privately.
   Start the host, sign in again, check crash dismissal and re-create API tokens
   as needed. Do not install an old token-bearing backup over the reset later.

### Duplicate paired-client records after upgrading from 1.x

Vibeshine 1.x added a new paired-client record every time an already-paired
Moonlight client paired again, and could re-import legacy certificates the same
way. It always used the first record for a client certificate: later copies were
never used for authorization, the client's identity or its settings.

Builds up to 2.0.2-daily.d20261020.4 refused to start with these duplicates
(`root.named_devices[N].cert duplicates ...`, then "HTTP interface is stopping")
and the service restarted them in a loop. Startup now collapses them without any
manual steps:

- The first record for each certificate, the one 1.x used, keeps its UUID, name
  and settings. Later copies are removed and each removal is logged.
- If any copy was disabled, the kept record is disabled. Re-enable it in the Web
  UI, or pair the client again, if that is intended.
- The original file is saved once as `sunshine_state.json.before-dedupe.bak` next
  to the pairing state. Do not upload it: it contains credential hashes.

If pairing state cannot be loaded for any other reason, such as malformed JSON
without a valid recovery copy, the host and Web UI stay up but refuse pairing and
streaming, log the reason and retry automatically.

#### Choosing a different record offline

To keep a different duplicate than startup would, the offline recovery command
prepares a separate candidate without starting the host or changing its
configuration.

1. Arrange a maintenance window and stop the host/service before copying or
   replacing its state. Stopping the host disconnects active clients. Keep a
   private backup of the full configuration directory, including credentials,
   applications, auxiliary state and both pairing-state snapshots.
2. Work on copies of `sunshine_state.json` and, when present,
   `sunshine_state.json.bak` in the same private directory. Supply the actual
   configured pairing-state filename if `file_state` overrides the default.
   Do not upload these files: they can contain credential hashes and tokens.
3. Inspect the copies. For example, from PowerShell on Windows:

   ```powershell
   & 'C:\Program Files\Apollo\sunshine.exe' --pairing-recovery inspect 'C:\PrivateRecovery\sunshine_state.json'
   ```

   The report lists duplicate groups, existing UUIDs, names and enabled status,
   plus a `snapshot_token` bound to both input snapshots. It does not print
   certificates or credentials. Compare each group's original records privately,
   including per-client settings and any unknown fields, before choosing which
   existing UUID to retain.
4. Prepare a candidate, repeating `--keep` once for each duplicate group:

   ```powershell
   & 'C:\Program Files\Apollo\sunshine.exe' --pairing-recovery prepare 'C:\PrivateRecovery\sunshine_state.json' --expect-snapshot TOKEN --keep EXISTING-UUID --output 'C:\PrivateRecovery\candidate'
   ```

   Replace `TOKEN` and `EXISTING-UUID` with values from inspection. The output
   directory must not exist. Changed inputs require a fresh inspection. If any
   record in a group is disabled, the survivor must also be disabled; intentional
   reauthorization belongs in the normal pairing flow after recovery.
5. Review `recovery.json` and `sunshine_state.reconciled.json`. A complete output
   includes `primary.original.json`, `backup.original.json` when a backup existed,
   and a manifest marked `complete`. Missing or unreadable files, a missing
   manifest, or an unexpected hash mean the candidate must not be installed.
   The original copies remain byte-for-byte unchanged. Credential fields, host
   identity, unrelated records and unknown JSON values are retained in the
   candidate; only selected duplicate records and their JSON separators are
   removed. All other input bytes, including unknown numeric values, survive.
6. With the host still stopped, verify that live state has not changed since
   it was copied. Preserve its current primary and backup outside the live
   filenames. Install the reviewed candidate as the configured primary using
   the existing owner and file permissions, then restart the host. Do not restore
   an older backup over newer credentials or revocations. Successful startup
   refreshes the validated recovery copy through the existing state writer.
   Verify retained pairings, disabled-client rejection, Web UI credentials and
   client settings, then restart once more to verify persistence.

The command requires an explicit `--keep` choice for every group of different
UUIDs. Only an entire byte-identical record repeated under the same UUID is
collapsed without a choice. Conflicting records under one UUID, invalid
certificates and malformed state are refused. The command never installs a
candidate, alters live pairing data or falls back to stale authorization.

Vibepollo also treats a native permission mask with no recognized permissions as
disabled. An imported `enabled: false` record remains disabled even if its mask
grants permissions. Recovery retains the selected record's exact permission
mask, client preferences and unknown fields; it never combines permission masks.

The output directory and files are private to the creating account on POSIX and
to the owner, Administrators and LocalSystem on Windows. Protect the input copies
and retained backups with equivalent permissions.

### Controller works on Steam but not in games
One trick might be to change Steam settings and check or uncheck the configuration to support Xbox/PlayStation
controllers and leave only support for Generic controllers.

Also, if you have many controllers already directly connected to the host, it might help to disable them so that the
Sunshine-provided controller (connected to the guest) is the "first" one. In Linux this can be achieved on USB
devices by finding the device in `/sys/bus/usb/devices/` and writing `0` to the `authorized` file.

### Network performance test

For real-time game streaming the most important characteristic of the network
path between server and client is not pure bandwidth but rather stability and
consistency (low latency with low variance, minimal or no packet loss).

The network can be tested using the multi-platform tool [iPerf3](https://iperf.fr).

On the Sunshine host `iperf3` is started in server mode:

```bash
iperf3 -s
```

On the client device iperf3 is asked to perform a 60-second UDP test in a reverse
direction (from server to client) at a given bitrate (e.g. 50 Mbps):

```bash
iperf3 -c {HostIpAddress} -t 60 -u -R -b 50M
```

Watch the output on the client for packet loss and jitter values. Both should be
(very) low. Ideally, packet loss remains less than 5% and jitter below 1 ms.

For Android clients use
[PingMaster](https://play.google.com/store/apps/details?id=com.appplanex.pingmasternetworktools).

For iOS clients use [HE.NET Network Tools](https://apps.apple.com/us/app/he-net-network-tools/id858241710).

If you are testing a remote connection (over the internet), you will need to
forward the port 5201 (TCP and UDP) from your host.

### Packet loss (Buffer overrun)
If the host PC (running Sunshine) has a much faster connection to the network
than the slowest segment of the network path to the client device (running
Moonlight), massive packet loss can occur: Sunshine emits its stream in bursts
every 16 ms (for 60 fps), but those bursts can't be passed on fast enough to the
client and must be buffered by one of the network devices inbetween. If the
bitrate is high enough, these buffers will overflow and data will be discarded.

This can easily happen if e.g., the host has a 2.5 Gbit/s connection and the
client only 1 Gbit/s or Wi-Fi. Similarly, a 1 Gbps host may be too fast for a
client having only a 100 Mbps interface.

As a workaround the transmission speed of the host NIC can be reduced: 1 Gbps
instead of 2.5 or 100 Mbps instead of 1 Gbps. A technically more advanced
solution would be to configure traffic shaping rules at the OS level, so that
only Sunshine's traffic is slowed down.

Such a solution on Linux could look like that:

```bash
# 1) Remove existing qdisc (pfifo_fast)
sudo tc qdisc del dev <NIC> root

# 2) Add HTB root qdisc with default class 1:1
sudo tc qdisc add dev <NIC> root handle 1: htb default 1

# 3) Create class 1:1 for full 10 Gbit/s (all other traffic)
sudo tc class add dev <NIC> parent 1: classid 1:1 htb \
    rate 10000mbit ceil 10000mbit burst 32k

# 4) Create class 1:10 for Sunshine game stream at 1 Gbit/s
sudo tc class add dev <NIC> parent 1: classid 1:10 htb \
    rate 1000mbit ceil 1000mbit burst 32k

# 5) Filter UDP source port 47998 into class 1:10
sudo tc filter add dev <NIC> protocol ip parent 1: prio 1 \
    u32 match ip protocol 17 0xff \
    match ip sport 47998 0xffff flowid 1:10
```

In that way only the Sunshine traffic is limited by 1 Gbit. This is not persistent on reboots.
If you use a different port for the game stream, you need to adjust the last command.

Sunshine versions > 0.23.1 include improved networking code that should
alleviate or even solve this issue (without reducing the NIC speed).

### Packet loss (MTU)
Although unlikely, some guests might work better with a lower
[MTU](https://en.wikipedia.org/wiki/Maximum_transmission_unit) from the host.
For example, an LG TV was found to have 30–60% packet loss when the host had MTU
set to 1500 and 1472, but 0% packet loss with a MTU of 1428 set in the network card
serving the stream (a Linux PC). It's unclear how that helped precisely, so it's a last
resort suggestion.

## Linux

### Hardware Encoding fails
Due to legal concerns, Mesa has disabled hardware decoding and encoding by default.

```txt
Error: Could not open codec [h264_vaapi]: Function not implemented
```

If you see the above error in the Sunshine logs, compiling *Mesa* manually may be required. See the official Mesa3D
[Compiling and Installing](https://docs.mesa3d.org/install.html) documentation for instructions.

> [!IMPORTANT]
> You must re-enable the disabled encoders. You can do so by passing the following argument to the build
> system. You may also want to enable decoders, however, that is not required for Sunshine and is not covered here.
> ```bash
> -Dvideo-codecs=h264enc,h265enc
> ```

> [!NOTE]
> Other build options are listed in the
> [meson options](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/meson_options.txt) file.

### Input not working
After installation, the `udev` rules need to be reloaded. Our post-install script tries to do this for you
automatically, but if it fails, you may need to restart your system.

If the input is still not working, you may need to add your user to the `input` group.

```bash
sudo usermod -aG input $USER
```

#### Multiseat

If you run multiple concurrent Wayland sessions on separate logind seats (e.g. `seat0`, `seat1`),
your compositor may ignore injected input unless Sunshine's virtual devices are assigned to the correct seat.

Sunshine determines its target seat from `XDG_SEAT`, which is typically set automatically by your display manager.
If needed, you can override it manually in your systemd service file or shell environment before starting Sunshine.

When the seat is not `seat0`, Sunshine appends the seat name to its virtual device names, for example:

- Keyboard passthrough (seat1)
- Sunshine PS5 (virtual) pad (seat1)

Sunshine creates two mouse devices: a relative one and an absolute one.

To assign Sunshine's virtual devices to the correct seat, create this udev rules file
(/etc/udev/rules.d/72-sunshine-virtual-seat.rules):
```udev
SUBSYSTEM=="input", KERNEL=="input*", ATTR{name}=="*(seat1)*", TAG+="seat", ENV{ID_SEAT}="seat1"
```

Then reload udev:

```bash
sudo udevadm control --reload-rules && sudo udevadm trigger -s input
```

### KMS Streaming fails
KMS screencasting requires elevated privileges which are not allowed for Flatpak or AppImage packages.
This means that you must install Sunshine using the native package format of your distribution, if available.
KMS capture will soon be phased out in favour of XDG Portal Capture (which works with all package types).

### KMS Streaming; some windows flicker/disappear on KDE Plasma 6.5+
KWin's overlay support interferes with KMS capture. As of KWin 6.5 this is not yet set by default, but
for future versions that enables this by default, you may be able to disable again via a special
[environment variable](https://invent.kde.org/plasma/kwin/-/wikis/Environment-Variables#kwin_use_overlays):

```bash
export KWIN_USE_OVERLAYS=0
```

> [!NOTE]
> Disabling overlays will reduce KWin's rendering efficiency. Consider using XDG Portal Capture instead.

### KMS streaming fails on Nvidia GPUs
If KMS screen capture results in a black screen being streamed, you may need to
set the parameter `modeset=1` for Nvidia's kernel module. This can be done by
adding the following directive to the kernel command line:

```bash
nvidia_drm.modeset=1
```

Consult your distribution's documentation for details on how to do this. (Most
often grub is used to load the kernel and set its command line.)

### AMD encoding latency issues
If you notice unexpectedly high encoding latencies (e.g., in Moonlight's
performance overlay) or strong fluctuations thereof, your system's Mesa
libraries are outdated (<24.2). This is particularly problematic at higher
resolutions (4K).

Starting with Mesa-24.2, applications can request a
[low-latency mode](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/30039)
by running them with a special
[environment variable](https://docs.mesa3d.org/envvars.html#envvar-AMD_DEBUG):
```bash
export AMD_DEBUG=lowlatencyenc
```
Sunshine sets this variable automatically, no manual
configuration is needed.

To check whether low-latency mode is being used, one can watch the VCLK and DCLK
frequencies in amdgpu_top. Without this encoder tuning both clock frequencies
will fluctuate strongly, whereas with active low-latency encoding they will stay
high as long as the encoder is used.

### Gamescope compatibility
Some users have reported stuttering issues when streaming games running within Gamescope.

## macOS

### Dynamic session lookup failed
If you get this error:

> Dynamic session lookup supported but failed: launchd did not provide a socket path, verify that
> org.freedesktop.dbus-session.plist is loaded!

Try this.
```bash
launchctl load -w /Library/LaunchAgents/org.freedesktop.dbus-session.plist
```

## Windows

### No gamepad detected
You must install ViGEmBus to use virtual gamepads. You can install this from the troubleshooting tab of the web UI.

Alternatively, you can manually install it from
[ViGEmBus releases](https://github.com/nefarius/ViGEmBus/releases/latest). You must use version 1.17 or newer.

After installation, it is recommended to restart your computer.

### Permission denied
Since Sunshine runs as a service on Windows, it may not have the same level of access that your regular user account
has. You may get permission denied errors when attempting to launch a game or application from a non-system drive.

You will need to modify the security permissions on your disk. Ensure that user/principal SYSTEM has full
permissions on the disk.

### Stuttering
If you experience stuttering using NVIDIA, try disabling `vsync:fast` in the NVIDIA Control Panel.

<div class="section_buttons">

| Previous      |                    Next |
|:--------------|------------------------:|
| [API](api.md) | [Building](building.md) |

</div>

<details style="display: none;">
  <summary></summary>
  [TOC]
</details>
