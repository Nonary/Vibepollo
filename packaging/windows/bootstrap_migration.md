# Bootstrap-owned supported installation migration

The concrete supported 2.0.0 upgrade is MSI-backed. Published VibepolloSetup-v2.0.0.exe SHA256: `7b3500ec0c774644ce5a435a48f61c046c48494d0f18b67afa0b3561931794b7`. Embedded MSI SHA256: `83ce98d7e0e515d3518b734abe40ed16e03cf5e1ed42738a2af21e25be0ee20c`. Its ProductCode is `{01A0F036-8DBA-7607-86C4-4534667DA302}`, ProductVersion `2.0.99.0`, and UpgradeCode `{E3FA501A-85F8-4187-85A7-D6E6BDC7EDA1}`. It has the transactional replacement marker and authors a named HKLM Uninstall/Vibepollo entry without WindowsInstaller=1 pointing at WPF uninstall.exe; the actual MSI entry is hidden. This is a facade, not a separate NSIS install.

raidflex's log exits1603 at the bootstrap preservation guard before MSI starts. Its state path is C:/Program Files/Apollo/config/vibeshine_state.json, consistent with the released package. The attachment lacks a registry inventory, so additional stale entries on that machine cannot be excluded. This failure is distinct from another reporter's successful-install USB/IP warning.

## Ownership and sequence

1. Bootstrap resolves the facade to one live machine MSI using its actual cached sunshine.exe component and registered component path, not an ARP icon directory. Product DEFAULT=5 and component LOCAL=3 are distinct native states. Source/incoming family identity and cached driver-removal upgrade guards must be verified.
2. Bootstrap locks a protected source-specific checkpoint, records running root-owned application services before quiescing, and snapshots source and destination data with hashes and atomic manifests. Entries include config/, credentials/, covers/, apps.json, sunshine.conf, sunshine_state.json and backup, and vibeshine_state.json. Drivers, Driver Store and trust remain untouched by migration.
3. Bootstrap preflights destination conflicts, seeds data, prepares configuration layout and the selected display backend, and hardens credentials before MSI. Flat originals and logs are retained. .NET file APIs handle paths; no installed legacy script/uninstaller is executed. Reparse points and volume roots are rejected. GUI/CLI share verified root selection and CLI forwards one normalized INSTALL_ROOT.
4. MSI performs normal files/services/registry installation and same-family major upgrade. Its ordinary transaction owns binary/service rollback. Driver versions, signing/trust consent, opt-outs, selection persistence, reboot outcomes and genuine failures remain unchanged. There is no independent same-family pre-uninstall gap.
5. Bootstrap commits after validated MSI registration. On failed/canceled install, it retains failed data, restores both preimages and missing layout state, re-hardens credentials, and restarts only originally running root-owned application services. Pre-MSI preparation failure also restores the checkpoint. Busy/timeout outcomes retain evidence without restoring while MSI may still be active.
6. Retry validates/reuses interrupted checkpoints and restoration journals. After rollback, a fresh baseline preserves intervening user edits. Completed checkpoints do not overwrite committed settings. Recovery evidence is retained outside the old MSI-cache stash sweeper.

## Removed MSI workarounds

| Removed workaround | Replacement / coverage |
| --- | --- |
| MigrateConfig/SetMigrateConfig; RunInstallerMigrations/SetRunInstallerMigrations | Bootstrap layout/backend/credential preparation under checkpoint, before MSI. Production tests verify UTF-8 data, idempotency, retained originals and rollback. Ancient schema expansion is outside this change. |
| RemoveConflictingProducts VBScript; SKIP_REMOVE_CONFLICTING_PRODUCTS shim | Bootstrap owns classification/preflight. Released 2.0.0 facade resolves once to its MSI. Source XML and packaged-table tests verify no MSI migration actions remain. |
| Sunshine uninstaller prompt/poll scripts | No MSI action invokes an arbitrary legacy uninstaller. |
| Apollo RemoveLegacyApolloUninstallKey component | No blind HKLM/HKCU Apollo/Apollo_is1 registry deletion. Unknown entries are conservatively blocked, not erased. |
| Independent same-family/special-source pre-uninstall | Skipped only after verified transactional identity. Normal major upgrade handles product replacement. |

Portable/manual migration scripts remain for separate existing callers, but MSI no longer invokes them for upgrades. Unrelated firewall/cache recovery, normal MSI service/file installation, driver policies, and explicitly requested factory reset remain. No ancient NSIS/cross-product migration framework was added.

## Validation boundaries

Tests run production filesystem/layout code and real read-only MSI queries, including the exact released 2.0.0 package. Installed-component lookup and service/MSI outcomes are mocked. No live install/uninstall, service/game disruption, driver or certificate mutation occurred. Real inventory preservation, power-loss/cancel/service rollback require disposable-environment validation. The bootstrap source lock does not serialize unrelated external MSI processes for the entire pre-MSI interval. Unknown, cross-product and per-user cases remain conservative without blocking the verified supported path.

Publication to the primary branches was authorized after candidate validation. No release or live installation is part of this change.
