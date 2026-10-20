# Experimental Hyprland physical-display host

The native machine package has an administrator opt-in for a limited Hyprland
session. KDE/Plasma remains the default. This backend has component coverage;
live Hyprland/NVIDIA capture, application, login and suspend validation is still
required before claiming this platform works on a particular machine.

Use a Hyprland session started by SDDM through **Hyprland (uwsm-managed)**,
using UWSM's `hyprland.desktop` entry. The controller still requires the chosen
local desktop account, active Wayland logind session and seat0. UWSM must publish
its current `WAYLAND_DISPLAY` to the user manager and finish starting
`wayland-wm@hyprland.desktop.service`,
`wayland-session@hyprland.desktop.target` and `graphical-session.target`.
The controller observes these units and probes the validated user-owned socket.
Its peer UID must match the selected user, and its process must belong to the
fixed UWSM unit's cgroup, including the packaged start-hyprland watchdog;
it does not start a compositor or import arbitrary environment files. Each UWSM
compositor invocation, peer PID and process start time are bound to the trusted machine session record, so a
watchdog child restart or unit restart replaces the host generation even if the socket name repeats.
An observed stopped unit or changed invocation revokes the previous generation
even when its replacement socket is unavailable. Failed metadata queries alone
retain suspend-transition retry behavior and never admit an unverified replacement.
See [Hyprland's systemd startup guide](https://wiki.hypr.land/Useful-Utilities/Systemd-start/).

Install the native package manually following [the Linux installation guide](install.md#install-manually).
The recommended installer script retains its Plasma requirements. After package
configuration selects the intended desktop account, an administrator can add
this setting to the existing root-owned mode 0600 `/etc/vibepollo/machine.conf`:

```ini
desktop_backend=hyprland-kms
```

Keep the existing `desktop_user` line. The package's configure/upgrade path
preserves this selection; omitting the setting selects `kwin`. A policy or
session change stops the host and its applications before binding a new
generation, interrupting active streams. The Plasma Login greeter continues to
use the KWin backend, independent of the desktop policy. SDDM greeter streaming
is not provided.

The supported slice captures an already enabled **physical SDR output through
KMS**, with audio/input and administrator-authorized native Wayland application
commands. Set `output_name` to an exact physical connector name from display
enumeration before streaming; empty and numeric output aliases are rejected.
Capture checks that selected connector's current DRM `HDR_OUTPUT_METADATA`
state at initialization and before delivering frames. A zero blob or valid
static metadata with SDR EOTF is required; missing/unreadable metadata,
unsupported metadata types and every HDR EOTF fail closed. Display enumeration
alone does not certify SDR state. The user must disable HDR in Hyprland on that
connector; the host does not change its color pipeline or perform HDR-to-SDR
conversion. Connectors without a readable standard HDR property are excluded
from this experimental capture policy. The backend forces KMS and disables the global
private-display default for that host launch. Saved Plasma output topology is
not applied. Existing managed kernel outputs and configured reserved outputs
are excluded from capture selection.

These operations are explicitly unsupported: wlr capture in the machine-service
account, private/Remote Monitor creation, display modes/layout/scale/HDR changes,
DPMS wake/power control, touchscreen mapping and KWin application focus. Physical
outputs must be enabled by the desktop user. HDR or private-output stream requests
fail with an explanation. The backend advertises no independent-output capability.
KScreen and managed-connector operations are fenced even if called outside the
normal backend route.

The application environment is Wayland-only: `DISPLAY` and `XAUTHORITY` are not
imported, and inherited user-manager values are removed from launched services.
Hyprland's Xwayland server does not publish KWin's generated Xauthority contract;
Steam/Big Picture, Steam-direct, Lutris, HDR compatibility and other X11-oriented
broker operations therefore return an explicit unsupported error. Launch native
Wayland games through authorized application commands or capture the already
running desktop. This subset does not promise stock Steam/Proton game launching.

Before promoting support, test physical KMS/NVENC or VAAPI capture, audio/input,
a native Wayland application, logout/login and greeter transitions, compositor
restart with a reused socket, suspend/resume and upgrade cleanup. Verify that
private/HDR/Steam requests fail without changing outputs, that old compositor
applications are cancelled, and that switching the policy back to `kwin`
preserves the existing Plasma behavior. No live compositor test is implied by
the component fixtures.
