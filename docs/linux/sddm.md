# SDDM login-screen streaming

Vibepollo can attach to an active, local seat0 SDDM greeter running KWin on
Wayland, with autologin disabled. Pre-login capture currently requires NVIDIA.
Choosing a Wayland desktop session does not configure the login screen itself.
An SDDM X11 or Weston greeter remains usable locally, but cannot be streamed by
this backend. Plasma Login Manager continues to use its existing integration.

## Check the greeter configuration

Read `/etc/sddm.conf`, `/etc/sddm.conf.d/*.conf`, and the distribution's files in
`/usr/lib/sddm/sddm.conf.d/`. SDDM loads distribution settings first, then local
drop-ins, then `/etc/sddm.conf`; later settings take precedence. Its upstream
defaults are `DisplayServer=x11` and `CompositorCommand=weston --shell=kiosk`.

Vibepollo observes the actual session and socket rather than assuming these
defaults. The controller journal identifies an unsupported X11 greeter or a
Wayland socket served by a non-KWin compositor and names the required settings:

```bash
journalctl -b -u vibepollo-session-controller.service --no-pager
```

Vibepollo never rewrites SDDM configuration. If the greeter already launches
KWin directly on Wayland, updating Vibepollo supplies the detection helper;
there is no additional Vibepollo greeter service or environment hook to install.
The helper rejects a socket owned by `kwin_wayland_wrapper`; launch
`kwin_wayland` directly. A wrapper that merely executes KWin without owning the
socket does not change the observed KWin identity.

## Optional administrator configuration

For an installation that needs a different greeter compositor, first review the
distribution's existing KWin/SDDM integration. An administrator can adapt this
example in a local drop-in such as `/etc/sddm.conf.d/90-local-wayland.conf`:

```ini
[General]
DisplayServer=wayland
GreeterEnvironment=QT_WAYLAND_SHELL_INTEGRATION=layer-shell

[Wayland]
CompositorCommand=/usr/bin/kwin_wayland --no-global-shortcuts --no-lockscreen --locale1
```

Preserve other distribution and administrator settings. In particular, merge
any existing `GreeterEnvironment` entries rather than replacing them. A value in
`/etc/sddm.conf` overrides a drop-in. Apply login-manager changes at a planned
reboot: restarting SDDM ends graphical sessions and active streams. Removing
Vibepollo leaves these administrator-owned settings intact; revert only the
local changes you chose to make if restoring your previous greeter.

## Verify the complete experience

With autologin disabled, boot into the greeter and check both the controller
binding and the host's DRM capture and H.264 encoder readiness. Connect from a
paired client and verify video, mouse, keyboard and password entry. Pair a new
client through the LAN Web UI before login, then test login, logout, KWin
restart and suspend/resume. Test install, upgrade and removal at the greeter.

Login and logout revoke the old host and its workers before attaching to the
new session. Streaming disconnects during that handoff. Machine identity and
pairing remain in `/var/lib/vibepollo`, so re-pairing is unnecessary; automatic
stream reconnection depends on the client. A readiness probe or passing unit
test alone does not prove real capture, display mutation or input injection.

References: [SDDM configuration manual](https://github.com/sddm/sddm/blob/develop/data/man/sddm.conf.rst.in)
and [KDE's SDDM Wayland configuration](https://github.com/KDE/plasma-workspace/blob/master/sddm-wayland-session/plasma-wayland.conf).
