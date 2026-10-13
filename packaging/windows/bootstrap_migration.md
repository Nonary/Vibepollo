# Focused 2.0.0 Windows upgrade compatibility

The released 2.0.0 installer registers a visible `uninstall.exe` entry alongside
its hidden Windows Installer product. The visible entry is an uninstall facade,
not an independent NSIS installation. Treating it as a separate installation can
block an upgrade before the incoming MSI runs.

The bootstrapper resolves Vibeshine/Vibepollo facades only against one live,
per-machine MSI of the same product kind at the same verified installation root.
It reads the cached MSI's `sunshine.exe` component and asks Windows Installer for
the registered component path; an ARP icon under `Windows/Installer` is not an
installation directory. Missing, cross-product, per-user, and ambiguous matches
do not authorize facade coalescing. Quiet upgrades preserve the verified source
directory unless the caller explicitly supplies `INSTALL_ROOT`.

This lets 2.0.0 installations enter the existing transactional major upgrade to
2.0.1-daily. The incoming MSI owns replacement and rollback. The old 1.14.8
version-specific pre-uninstall workaround is removed. Existing legacy-payload
downgrade recovery remains.

Sunshine/Apollo competing-product uninstall dispatch, synchronous NSIS handling,
MSI conflict checks, the Sunshine uninstall prompt/wait actions, configuration
migration, service/firewall actions, driver selection, and explicit factory reset
remain available. Vibepollo also retains its existing Apollo ARP cleanup.
No broad bootstrap data checkpoint or migration preflight blocks unrelated
fresh, legacy, cross-product, or per-user installations.

Regression tests compile production installer code, exercise real read-only MSI
metadata queries, and execute the competing-product dispatcher with registry and
process boundaries stubbed. The extracted published Vibepollo 2.0.0 MSI has
ProductCode `{01A0F036-8DBA-7607-86C4-4534667DA302}`, ProductVersion `2.0.99.0`,
and UpgradeCode `{E3FA501A-85F8-4187-85A7-D6E6BDC7EDA1}`. Its executable component
is verified using the actual artifact. Full live installation and rollback still
require a disposable Windows environment; the regression suite never installs a
product or alters services, drivers, or certificates.
