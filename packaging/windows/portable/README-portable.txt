Windows portable streaming

Extract the entire archive to a folder owned and writable by your Windows user.
Run start-portable.cmd normally. Keep its console open while streaming; Ctrl+C
stops the host. The launcher runs sunshine.exe as your current user and starts
no service. It uses the physical desktop, preserves its display settings, and
installs no driver. Optional installer/driver scripts bundled with the runtime
are not needed for this route and are not run by the launcher.

Open https://localhost:47990, create your credentials, then add this PC's IP
address in Moonlight and enter its pairing PIN in the web interface. Windows
Firewall/network policy must already permit Moonlight connections; this package
does not change firewall rules. Follow your organization's software policy.

Settings, certificates, pairing state and logs remain in the config folder.
Protect that folder as private account data. To upgrade, extract a new archive
into a separate writable folder and copy your old config folder over its clean
one while the host is stopped. To remove, stop the host and delete its folder.

Capture requires an active physical display in your interactive Windows session.
Virtual display, virtual gamepad and virtual audio features require their drivers
to have been installed separately by an administrator. Ordinary keyboard/mouse
input cannot control elevated applications. Portable launch does not request
administrator access or install these optional features.
