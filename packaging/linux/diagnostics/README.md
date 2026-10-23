# Shutdown diagnostics on the development host

The broker socket uses `DefaultDependencies=no` to remove its implicit
`Before=sockets.target` edge. Its controller starts after `basic.target`, so
the default edge formed an ordering cycle. Explicit sysinit and shutdown
dependencies retain normal socket startup prerequisites and shutdown cleanup.
Keep host → broker → controller stop ordering intact.

There are two diagnostic paths:

- `gpu-incident-recorder.service` starts before `basic.target`, keeping its
  journal watcher alive while ordinary services stop. It still stops before
  journald and the writable log filesystem. Bundles remain under
  `/var/log/gpu-incidents`.
- `gpu-shutdown-diagnostics.service` configures kernel netconsole and arms
  PID 1 debug logging to kmsg at the beginning of shutdown. `systemd-shutdown`
  inherits the log options, including when it enters the shutdown initramfs.
  Kernel logging therefore continues after Python, journald, and writable
  filesystems are gone. The existing EFI pstore setup and `initcall_debug`
  remain enabled.

Install the host components alongside the existing GPU incident recorder and
GPU diagnostics setup services:

```sh
sudo install -m644 packaging/linux/vibepollo-session-exec.socket /usr/lib/systemd/system/
sudo install -m644 packaging/linux/diagnostics/gpu-incident-recorder.service /etc/systemd/system/
sudo install -m644 packaging/linux/diagnostics/gpu-shutdown-diagnostics.service /etc/systemd/system/
sudo install -m755 scripts/linux_shutdown_diagnostics.py /usr/local/libexec/gpu-shutdown-diagnostics
sudo install -m644 packaging/linux/diagnostics/gpu-shutdown-diagnostics.conf /etc/sysctl.d/99-gpu-shutdown-diagnostics.conf
printf 'NETCONSOLE_TARGET=steamdeck.local\n' | sudo tee /etc/default/gpu-shutdown-diagnostics
sudo sysctl -p /etc/sysctl.d/99-gpu-shutdown-diagnostics.conf
sudo systemctl daemon-reload
sudo systemctl restart gpu-incident-recorder.service
sudo systemctl enable --now gpu-shutdown-diagnostics.service
```

No streaming-service restart or reboot is needed to load the unit change.
The supplied sysctls enable soft/hard lockup reports and a 30-second blocked
task threshold. They do not enable panic or automatic reboot. They override
the host's `nowatchdog` boot default at runtime on each boot.

On the separate receiver, install `scripts/linux_netconsole_receiver.py` as
`~/.local/libexec/vibepollo-netconsole-receiver`, create
`~/.config/vibepollo-netconsole.conf` containing the sender's IPv4 address:

```ini
NETCONSOLE_SOURCE=192.168.0.140
```

Install the companion receiver unit in `~/.config/systemd/user/`, then run:

```sh
systemctl --user daemon-reload
systemctl --user enable --now vibepollo-netconsole-receiver.service
```

The receiver requires Python 3 and, for its optional desktop sleep inhibitor,
dbus-python and the desktop's `org.freedesktop.PowerManagement` service. The
Steam Deck has both. The unit blocks logind idle sleep, and the receiver holds
a desktop sleep-inhibition cookie. Stopping the unit releases both. Logs are
in `~/.local/state/vibepollo-netconsole/`, restricted to the configured sender,
fsynced per packet, rotated daily or at 16 MiB, with at most 32 files retained.
The user unit starts with that user's systemd manager; it needs a logged-in
desktop session for the desktop inhibitor.

Validate with a harmless SysRq-W dump or the host helper's `arm` mode, never
by invoking `systemd-shutdown` directly. `arm` changes live PID 1 logging:

```sh
sudo env NETCONSOLE_TARGET=steamdeck.local /usr/local/libexec/gpu-shutdown-diagnostics arm
# Confirm the marker, PID 1 debug messages, and "sysrq: Show Blocked State"
# reached the receiver. Restore the normal manager settings after this test:
sudo systemd-analyze log-level info
sudo systemd-analyze log-target journal-or-kmsg
```

For the next incident, retain the receiver's latest log as well as local
incident bundles, the previous boot journal, and archived EFI pstore. Keep the
receiver awake and reachable during shutdown. Netconsole is UDP and stops
when the sender's NIC stops working; a final device/firmware stall can still
outlast its coverage. No reboot test or recurrence-free claim is implied by a
successful transport self-test.

On AmbidexL, pre-change socket and recorder units were saved under
`/var/lib/gpu-shutdown-diagnostics/backup-20261015/`. To stop remote collection,
disable the receiver unit on the Deck. To remove host instrumentation, disable
`gpu-shutdown-diagnostics.service`, remove its exact unit/helper/environment
and sysctl files, restore the saved recorder unit, reload systemd, restart the
recorder, and restore normal manager logging. Before instrumentation the
kernel watchdog and NMI watchdog were 0 and the hung-task timeout was 120.
Retain the repaired broker socket when removing diagnostic instrumentation.
