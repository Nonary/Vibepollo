# Linux Machine-Service Architecture: Lessons Learned

This file records the design constraints behind Vibepollo's supported Linux
pre-login implementation. It intentionally replaces the old per-user setup
notes; use the repository `AGENTS.md` for current commands.

## Why the old model failed

A user service cannot be the machine's pre-login authority. Its home-scoped
state disappears from the greeter's view, its display and audio credentials
belong to one login session, and carrying those credentials through PAM or a
handoff races the compositor. Restarting that service during login also caused
long stalls and X connection errors. File capabilities on a user-writable or
public executable made the trust boundary worse without solving session
ownership.

The supported model therefore has one persistent machine identity and treats
each greeter or desktop as a temporary execution context. Login intentionally
ends the greeter stream. The controller waits for the new compositor to become
authoritative, writes a new generation record, and starts a fresh host. No
stream or session credential is passed across the transition.

The user manager's inherited environment is not proof that graphical clients
can connect. KWin may generate a new Xwayland display and Xauthority file only
after its service begins, leaving later D-Bus- or systemd-activated programs
with missing or stale values even while the desktop itself appears healthy.
Publish the compositor-generated `WAYLAND_DISPLAY`, `DISPLAY`, and `XAUTHORITY`
as part of KWin's own start transaction, after validating the exact session
KWin process and runtime objects. A bounded native `ExecStartPost` helper keeps
that repair on the desktop compositor lifecycle boundary. It is deliberately
nonfatal so a publication error cannot terminate KWin; the error remains in the
unit journal and missing credentials remain a readiness failure. The greeter
does not start Xwayland and must not receive this hook. Polling daemons,
application wrappers, PAM handoffs, and controller-side environment mutation
do not belong on this path.

## Files, state, and policy have different owners

Linux's equivalent of the relevant Windows split is:

- program code and immutable assets: root-owned `/usr`;
- administrator policy: root-owned `/etc/vibepollo`;
- service data shared across logins: `/var/lib/vibepollo`, accessible only to
  the dedicated service account;
- transient orchestration: root-created `/run/vibepollo`.

Making `/usr/share/vibepollo` writable is not necessary for shared settings and
would allow code/data replacement. Shared state belongs in `/var/lib`; only an
administrator may change machine policy. Ordinary users are not members of a
privileged Vibepollo group.

## Privilege belongs at narrow boundaries

The public host binary and broker client have no file capabilities. A separate
private host inode has only the two permitted file capabilities KMS capture
and scheduling require. Its loader enters with no effective, inheritable, or
ambient capabilities; startup verifies that exact set and locks in
`no_new_privs` before parsing configuration.

A root-only broker similarly enters with permitted-only identity/kill
capabilities and raises an effective capability only around the exact syscall
that needs it. It performs only enumerated display, audio, provider, and
application operations. It authenticates the service peer,
validates the root session record and generation, then permanently drops to
the selected login identity before touching that session's runtime. Paths are
not authority: operations are semantic and inputs are bounded. Worker cgroups
must be killed and proven gone whenever the bound session changes.

Application cancellation cannot depend only on a successful systemd control
request. A capability-free supervisor in the user service also watches a pipe
whose sole writer belongs to the generation-bound broker worker. Worker or
broker death closes that pipe and triggers bounded TERM/KILL cleanup of the
controlled application descendants.

## Never have root traverse a live user tree

Checking a path and then recursively copying it as root is unsafe because a
user can replace entries between validation and use. Legacy migration instead
creates a root-controlled staging area and runs a tiny importer that drops to
the source user before opening the profile. Confined `openat2` traversal,
regular-file-only policy, snapshot checks, and strict resource limits reduce
both path races and denial-of-service risk. Root only validates and publishes
the resulting isolated copy.

Migration is not reset. Valid identity, certificates, pairing state, settings,
applications, and covers must survive an upgrade. State deletion requires an
explicit administrator reset or package purge and must refuse unexpected
mounts.

## Package lifecycle is part of the security model

DEB, Arch, and RPM hooks must close the broker socket first, stop every broker
worker, stop the controller and host, run cleanup, and verify the cgroups and
trusted session record are gone before replacing binaries. The same procedure
must converge on first install, partial previous installs, upgrade, removal,
and retry. Every package format must ship the same broker, application
supervisor, importer, provider scanner, units, and capability policy.

An explicit `BUILD_VERSION` must win over an old cache or missing environment
variable. Before release, inspect the locally staged package for the intended
version and complete manifest; never use remote CI as the first validation of
a packaging change.

## Pre-login has a deliberately smaller feature surface

The greeter may expose only the passive Desktop stream. New pairing requests,
arbitrary application launch, Steam/Lutris synchronization, and policy changes
belong to an authenticated desktop/admin context. Pairing requests and HTTP
bodies need hard size/count limits and timer-driven expiry rather than cleanup
only when another request arrives.

Every enabled client in the service-owned pairing database is authorized
before login; there is deliberately no second machine allowlist. Disabled,
unknown, malformed, or ambiguous pairing state fails closed, and new pairing
and PIN requests remain unavailable at the greeter.

Provider scanning runs as the selected session user. Its response is bounded
and path-free so the machine host never consumes user-controlled executable or
filesystem authority. Linux enables Steam; Playnite remains Windows-only.

## Readiness is end-to-end

`active (running)`, a reachable Web UI, and open TCP ports do not prove a
working stream. On this host, readiness requires:

- the controller bound the exact active local seat0 KDE Wayland session;
- the managed virtual output exists and KMS uses event-driven Vibepollo DRM
  capture;
- the H.264 encoder probe succeeds (HEVC and AV1 are optional and depend on
  the GPU generation; requiring them would lock out every pre-AV1 GPU);
- required UDP ports are listening/reachable;
- a real client can connect at the greeter, disconnect at login, and reconnect
  to the desktop.

The controller bounds its compositor probes and treats an unhealthy KWin as a
session-readiness failure. It closes broker admission before cleanup and will
not republish a binding until host, broker, and application cgroups are proven
empty. A GPU process stuck in uninterruptible kernel sleep can still require a
reboot, but it cannot leave a trusted half-running host advertised by the new
architecture.

The Web restart action must also return to that controller boundary. A machine
host exits instead of re-executing itself in place, because an in-place exec
would bypass the wrapper's one-time KMS and encoder readiness gate. The
controller then starts a fresh wrapper and repeats the complete proof.

The installed and loaded `vibeshine_drm` versions may differ after a package
update because the compositor holds the old module. A reboot, not merely a
Vibepollo restart, is required in that case.

## Review discipline

Architecture and privilege changes receive a defensive Daybreak review before
installation. Build, full tests, package generation, manifest inspection, and
static lifecycle tests run locally. After the user confirms an actual
pre-login connection on the installed system, a final Daybreak audit covers
the live units, ownership, capabilities, logs, and runtime behavior before the
release is committed and pushed.
