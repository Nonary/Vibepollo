# Vibepollo Linux Machine-Service Guide

This document describes the supported Linux deployment. The repository-level
`AGENTS.md` remains authoritative for the exact build, test, staging, and host
installation commands.

## Ownership model

Vibepollo follows the normal machine-wide Linux layout:

- `/usr/bin/vibepollo`, `/usr/libexec/vibeshine`, and
  `/usr/share/vibepollo` are immutable, root-owned program files.
- `/etc/vibepollo` is root-owned administrator policy. Ordinary desktop and
  greeter users must not be able to change it.
- `/var/lib/vibepollo` is persistent machine state, owned only by the
  unprivileged `vibepollo` service account and mode `0700`.
- `/run/vibepollo` contains short-lived root-created coordination records.
- User homes are neither the authoritative configuration store nor a runtime
  dependency after a one-time legacy migration.

Do not grant write access to `/usr/share/vibepollo`, add ordinary users to the
service group, or restore the old per-user service design. Do not put file
capabilities on the public executable or its client helper.

## Runtime trust boundary

`vibepollo-session-controller.service` is the only login-session authority. It
observes logind and accepts only the active, local, seat0 KDE Wayland desktop
or greeter session. It never activates a session, mutates PAM, writes a user's
environment, or keeps a stream alive across a login transition. A transition
stops the machine host and its generation-bound applications before binding a
fresh session. Disconnect and reconnect is intentional.

The controller writes a root-owned, `root:vibepollo` mode `0640` session
record. `vibepollo.service` runs as the dedicated `vibepollo` account and owns
the network protocol, machine configuration, and persistent state. Its
capability-free supervisor invokes a distinct root-owned, `root:vibepollo`
mode `0750` private host inode with only `CAP_SYS_ADMIN` and `CAP_SYS_NICE` in
the file-permitted set. The loader and parser enter with effective,
inheritable, and ambient sets empty; the literal first Linux statement verifies
that boundary and sets `no_new_privs` before configuration or logging.

Session-specific display, PipeWire, provider, and application operations go
through the root-owned `SOCK_SEQPACKET` socket. The controller alone opens the
static socket after publishing a valid binding. The root-only mode `0700`
broker carries only permitted `CAP_SETUID`, `CAP_SETGID`, and `CAP_KILL`; it
enters with effective/inheritable/ambient sets empty and raises an exact
effective set only around identity drop or cross-UID cancellation. The broker
accepts only the exact service UID/GID, validates the current root session
record and generation, authorizes semantic operations, drops to the selected
session identity, clears capabilities, and sets `no_new_privs`. Broker workers
and launched application scopes must be cancelled during every session
transition and package lifecycle operation. Each application scope runs the
capability-free `vibepollo-app-supervisor`; a broker-worker-owned watchdog pipe
forces bounded descendant cleanup even if the worker or broker is killed.

KWin creates the session's Xwayland display and authority file after the user
manager has already started it. A native, unprivileged `ExecStartPost` helper
reads those exact generated arguments from the session KWin process, validates
the runtime sockets and authority file, and publishes `WAYLAND_DISPLAY`,
`DISPLAY`, and `XAUTHORITY` through the standard D-Bus activation environment
command before the desktop KWin unit is considered started. Publication failure
is logged but kept nonfatal to the compositor; session readiness must therefore
reject missing display credentials. The Wayland-only greeter unit does not use
this Xwayland helper. The session controller remains a passive observer and
must not mutate the user's environment.

Provider discovery uses `/usr/libexec/vibeshine/vibepollo-provider-scan`. It
runs as the selected unprivileged session user and returns a bounded, path-free
description to the machine host. Separate numeric-ID artwork requests convert
local covers under that same session identity and return bounded PNG bytes to
the service-owned cache; user artwork paths never cross into the host. Linux always enables Steam and never launches
the Windows-only Playnite integration.

Stream-owned Steam launch policy uses the semantic `steam-direct` broker
operation. The broker accepts only a catalog-authorized numeric AppID and
bounded policy values, enters the selected desktop UID, then executes the
capability-free `/usr/libexec/vibeshine/vibepollo-steam-launch`. Only that
unprivileged helper reads Steam paths or launch options; incomplete metadata
must fail instead of falling back to the already-running Steam process.

The default Steam Big Picture open/close commands are fixed broker actions.
Their exact shipped command strings map to fixed Steam URIs after desktop
identity and endpoint validation, without a shell or command-manifest entry.
HDR streams use the `app-wayland-hdr` verb for the same catalog commands; Big
Picture still maps those two strings to the same helper. The helper runs in
the selected desktop session, matching other application launches: the broker
namespace cannot start Steam, write the session baseline, or see the user's
games. Fresh installations can have an empty administrator command manifest
before the host creates its default application catalog. Arbitrary application
commands still require administrator authorization; upgrades preserve that
policy rather than automatically trusting newly saved commands.

## Legacy profile migration

Migration is one-time and fail-closed. Root creates a private staging parent,
then `vibepollo-profile-import` drops permanently to the source user before it
opens or traverses the user's legacy profile. The importer uses confined
`openat2` resolution, rejects links and mount crossings, accepts only
directories and regular files, and enforces depth, count, byte, and time
limits. Root validates and atomically publishes only the staged copy; it must
never recursively copy a live user-controlled tree.

The migration retains existing machine identity, credentials, pairing state,
configuration, applications, and covers where valid. Policy and command
authorization remain administrator-controlled. Use the explicit
`vibepollo-machine-host reset` operation only when an administrator intends to
erase machine state; package removal does not erase it, while package purge
does.

## Package lifecycle

DEB, Arch, and RPM installations must contain the controller, socket, broker,
application supervisor, profile importer, provider scanner, Steam launcher,
machine host, KWin session-environment helper and desktop drop-in, and system units. Before
replacing files, lifecycle hooks close socket admission, stop all broker
instances, stop the controller and host, perform controller cleanup, and prove
that no worker, host cgroup, or trusted session record remains. First install
must also succeed when the managed virtual-display pool has never existed.

After installation, both `/usr/bin/vibepollo` and
`/usr/libexec/vibeshine/vibepollo-session-exec` must report an empty `getcap`.
The private host/broker must have only their exact permitted sets and private
modes described above. Only the controller is enabled; it owns the static
socket and starts the host after binding an authoritative session.

## Managed display and readiness

Desktop restoration uses a serialized background dispatcher and the existing
generation-bound `vibepollo-session-exec` display helper. Immediate restores,
delayed restores and persistence resets return after request admission; their
boolean result does not assert that the compositor has completed restoration.
New display ownership supersedes queued restore generations. The worker checks
capture and retained Remote Monitor ownership under the lifecycle gate before
mutating outputs, preserves the saved topology on failure, and clears reset
state only after restoration succeeds.

The Linux desktop baseline is saved in the service-owned auxiliary state before
private connector hotplug. Its payload is schema-validated and bound to the
selected session UID and role; another user's or greeter's baseline is ignored.
Idle startup baselines remain unarmed: physical-only streams and idle settings
changes preserve the user's current layout. Private display acquisition saves
restore intent before changing connectors; successful restoration clears that
intent, including for always-connected explicit outputs. A genuinely idle
desktop refreshes the baseline before the snapshot gate, so old failed restores
do not override newly disabled monitors. Restart recovery
uses the same asynchronous, capture-verified handoff and retires orphan managed
connectors only after a distinct saved guard is ready. Missing/unusable saved
intent can fall back only to currently enabled physical outputs. If every
physical monitor is disabled and no saved baseline survives, the private image
is retained: recovering an older, already-orphaned desktop requires an operator
to enable the intended physical monitor first.

The session broker gives KScreen five seconds, then kills/reaps its helper
before acknowledging cancellation. Cancelled display-apply requests still
report unknown compositor completion, since killing a D-Bus client does not
retract an already delivered mutation. The host allows ten seconds for the reply;
if mutation completion cannot be confirmed it refuses further display mutations
until host restart. Failed read-only queries do not fence later mutations.
Restore requests have a thirty-second admission budget beginning at worker execution, including waiting for lifecycle/display locks. Each helper
operation reserves its full safe reply interval before starting. Shutdown
closes mutation admission, cancels queued
machine-host restores, and joins the owned worker before platform teardown.
Standalone shutdown drains its final restore. Validate real topology and Web UI
responsiveness across suspend/resume before calling an installed fix complete.

The privileged virtual-display helpers and units use fixed root-owned `/usr`
paths. `vibeshine-drm-setup.service` rebuilds the installed module when its
source fingerprint changes, but cannot replace a module already used by the
compositor. Compare the installed `modinfo` version with
`/sys/module/vibeshine_drm/version` and reboot when they differ.

On this KDE/Wayland/NVIDIA host, keep `capture = kms`. A healthy deployment
requires the event-driven Vibepollo DRM capture message and successful H.264
encoder discovery; HEVC and AV1 are reported when the GPU supports them but are
not required for readiness. Unit activity, TCP listeners, or a reachable Web
UI alone are not proof that remote display works. Test both the greeter and the
desktop after a reboot, expecting the greeter stream to disconnect at login.

## Diagnostics

Inspect the system services, not the obsolete user unit:

```bash
sudo systemctl --no-pager --full status \
  vibepollo-session-controller.service vibepollo-session-exec.socket \
  vibepollo.service
sudo journalctl -u vibepollo-session-controller.service \
  -u vibepollo-session-exec@.service -u vibepollo.service \
  --since '-5 minutes' --no-pager
sudo stat -c '%U:%G:%a:%F %n' /usr/bin/vibepollo \
  /usr/libexec/vibeshine/vibepollo-app-supervisor \
  /etc/vibepollo/machine.conf /var/lib/vibepollo \
  /run/vibepollo/session.env
getcap /usr/bin/vibepollo /usr/libexec/vibeshine/vibepollo-session-exec \
  /usr/libexec/vibeshine/vibepollo-host \
  /usr/libexec/vibeshine/vibepollo-session-broker
ss -lntup | rg ':(47984|47989|47990|48010)\b'
```

Never restart the controller or host without warning the user: doing so
terminates the active stream.
