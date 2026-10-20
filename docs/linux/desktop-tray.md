# Desktop tray

Native Linux packages install `vibepollo-tray`, an unprivileged desktop
companion that starts at login. Install PySide6's QtWidgets module to enable
it (`pyside6` on Arch; `python3-pyside6.qtwidgets` on Debian/Ubuntu where
available). On RPM systems install the distribution's PySide6 QtWidgets
package. Missing Qt does not affect the streaming service. The tray requires
a desktop that implements the system tray; KDE Plasma supports it on Wayland.
AppImage, Flatpak, and SteamOS packages keep their existing tray behavior.

The tray reports machine-service state without authentication. Green means
the services are running; it does not prove that capture/encoding is healthy.
With an API token, it also shows Ready, Streaming, and Paused using the same
Apollo icons as Windows and reports state changes through desktop notifications.
`--no-notifications` disables notifications. Status is refreshed every five
seconds. `vibepollo-tray --port PORT` supports a non-default Web UI HTTPS port.

Choose **Connect API token…**, verify the local certificate fingerprint
against the certificate shown by your browser, and create a token in the
Web UI's **API Tokens** page with these exact scopes:

| Path | Method |
| --- | --- |
| `/api/session/status` | GET |
| `/api/apps/close` | POST |

Paste the token into the tray dialog. It is validated before being saved in
`$XDG_CONFIG_HOME/vibepollo-tray/PORT/settings.json` (defaulting to
`~/.config/vibepollo-tray/PORT/settings.json`). The directory is private and
the file is mode 0600. The tray never stores a Web UI password, reads the
service's private profile, or bypasses API authentication. TLS certificates
are pinned before sending credentials, including on each reconnection.
A replaced certificate requires reconnecting and approval; redirects are
not followed. **Forget API token** deletes the local token. Revoke it in the
Web UI as well to invalidate it at the host.

The menu provides:

- **Open Vibepollo** and **Pair a client** open the local Web UI.
- **Stop [application]** uses the authenticated API, with confirmation.
- **Check for Update** fetches public GitHub release metadata, displays the
  installed version and latest published stable/prerelease tags, and offers
  their release pages. It does not install anything or claim a nightly/stable
  version ordering. No API token is sent to GitHub.
- **Start Vibepollo**, **Restart**, and **Quit Vibepollo** operate on the
  session controller through systemd's existing polkit authorization.
  Restart and Quit ask for confirmation because they end active streams and
  the running application. They never stop/start the static host directly:
  the controller retains its ordered admission, host, and broker cleanup.
- **Exit tray** closes only the desktop companion; streaming continues.

Quit stops the controller for the current boot, leaving its boot enablement
unchanged. Start resumes it. The tray does not change startup settings.
To disable the tray at login, disable **Vibepollo Tray** in your desktop's
Autostart settings, or override the packaged desktop entry with a user entry
at `~/.config/autostart/vibepollo-tray.desktop` containing `Hidden=true`.

Development checks (from the repository root):

```sh
python3 tests/unit/platform/linux/test_desktop_tray.py
python3 packaging/linux/vibepollo-tray.py
```

Manually verify token setup, streaming/paused/ready transitions, application
stop, denied/cancelled polkit requests, and Quit/Start with a disposable stream.
Do not restart a live host as part of automated tests.
