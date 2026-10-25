# Driver installation safety

Windows display, legacy SudoVDA, and VHF gamepad installation compare the candidate INF's numeric four-part DriverVer component version with active CIM DriverVersion values and matching Driver Store INF versions. Dates, application versions, release tags and timestamps never decide replacement. Equal/newer active or staged components remain unchanged, including all certificate trust. Unknown versions, unreadable INF files and failed inventory queries block changes.

For Sunshine virtual display installation, retaining a package no longer skips device recovery. A present device reporting PnP problem code zero with an equal/newer active version remains untouched. A missing or failed device uses a separate repair path with the highest retained INF version at least as new as the bundled and inventoried active versions. The selected Windows Driver Store INF, DLL and catalog must pass the existing hardware, publisher and catalog-membership checks and already be trusted by Windows. Repair creates a missing node or restarts a failed present node, binds the retained INF, and verifies presence, PnP status and active version afterward. It never enters bundled-payload reconciliation, removes packages, provisions trust or forces replacement. Disabled devices are preserved with an actionable warning; reboot-required states defer further work. This does not establish streaming readiness or repair the separate installer freeze. Legacy SudoVDA and VHF retained-device recovery are outside this partial fix.

Display and SudoVDA admit missing or genuinely newer reviewed self-signed candidates after pinned public release-certificate identity, INF hardware/class identity, independent CMS signature and INF/DLL catalog membership checks. Eligible self-signed display candidates provision only the exact pinned certificate in LocalMachine Root and TrustedPublisher before Windows staging; equal/newer components add no trust. Provisioning or pre-replacement staging failures undo newly added certificates and retain pre-existing trust and drivers. Trusted production display catalogs retain their expected publisher identity and valid Windows chain; they add only TrustedPublisher. VHF can install a missing or genuinely newer self-signed component after independent package validation and informed recipient approval for the exact bundled certificate. Display and gamepad selections default on for fresh installations and older installers without saved preferences. Explicit opt-outs persist across upgrade, repair and cancel/reopen; selecting a display backend does not override its installation opt-out. Selecting the beta gamepad also authorizes persistent machine-wide trust for the independently verified reusable certificate, disclosed next to that selection. There is no separate certificate checkbox. GUI approval and the bundled MSI default bind only thumbprint `7F1868BF86698A8674F40E686B02C8C702BD3B88`; runtime rejects a changed beta signer before adding trust. Explicit MSI driver properties override stored defaults. Same-product maintenance reinstalls a dedicated registry-only preference feature, ensuring machine choices are updated without forcing unrelated files or settings to reinstall; existing caller repair options remain intact. Existing exact trust requires no new import.

Before consent or import, VHF checks manifest payload hashes, release-lock identity and script hashes, real INF version, matching certificate identities for catalog/setup tool, public DER certificate format, certificate validity and code-signing-only/non-CA restrictions. SignedCms verifies the catalog and embedded executable signature cryptographically. WinVerifyTrust verifies executable integrity and actual catalog membership of INF/DLL using the open member file and Windows SIP hash; only success or an otherwise valid untrusted-root chain is eligible for consent. Missing signatures, changed bytes, wrong identities and invalid chains fail before trust changes.

Recipient consent covers persistent LocalMachine Root and TrustedPublisher trust for that exact public certificate. Root provides signature-chain trust; TrustedPublisher approves silent PnP publisher installation. This can authorize other code signed with the same key. Daily builds reuse the pinned signer; key rotation requires an explicit reviewed identity update before it can add trust. Application-only updates with equal/newer driver components add nothing. Only missing stores receive the certificate. Installation failure removes only additions successfully made by that attempt; preexisting and unrelated trust remains unchanged. Rollback errors report the exact store/thumbprint requiring attention. No Secure Boot, test-signing, service or boot-policy changes are used to make a rejected driver load.

VHF production publisher handling remains behind the guard with DailyUnsignedPolicy=0. DailyUnsignedPolicy=1 suppresses VHF production publisher imports; recipient-approved local VHF trust is separate. Display trust uses the independently verified expected release signer described above for both stable and daily packages. No SignPath service is invoked by these installation paths. Windows policy may still refuse a validated driver; the installer reports that outcome without weakening security settings.

Driver actions targeting System64Folder PowerShell use WixQuietExec64, because an x64 MSI can still contain a 32-bit custom-action DLL. Cleanup also resolves Sysnative explicitly when called from a 32-bit host. Before stopping driver services or removing existing nodes, Windows stages and verifies catalog members. SudoVDA staging failure preserves its node. Bootstrapper MSI pre-uninstalls set UPGRADINGPRODUCTCODE to preserve shared driver devices. Legacy non-MSI migrations fail safely when preservation cannot be guaranteed. Explicit requested uninstall remains available.

USB/IP uses the pinned original Microsoft-signed package and retains existing shared devices, services and staged packages. It does not invoke the vendor's destructive upgrade installer. ViGEm is not replaced by the VHF installer.

Run `powershell -NoProfile -File packaging/windows/test_driver_install_safety.ps1` before daily publication. It includes the numeric version/signature matrix, mocked exact-certificate consent/import/staging/rollback tests, compiled bootstrapper consent tests, and read-only native crypto tests against existing signed public fixtures and modified copies. These tests never import certificates, execute a driver installer or generate keys. Publication additionally compares extracted MSI script hashes with this source.

The initial Sunshine and SudoVDA inventories retain a 15-second fast-path budget.
A confirmed child-process timeout now permits one fresh, complete read-only
snapshot with a 60-second recovery budget. No retry occurs for provider errors,
malformed results or uncertain child termination; partial first-attempt output
never enters version or trust decisions. Existing bounded stage diagnostics are
logged before recovery. Each timed-out attempt permits up to five seconds for
termination and one second for stdout drain, so the two execution budgets plus
cleanup may take about 87 seconds per selected display installer (excluding
process-launch/scheduling overhead). Selecting both drivers can incur this twice.
This mitigates slow/transient inventory, not a deterministic provider hang, and
does not change the separate VHF inventory or retained-device health budgets.
Native Windows provider behavior and MSI cancellation need Windows qualification.

The retained-display production guard and repair fixtures also run on Linux with `pwsh -NoProfile -File packaging/windows/test_retained_display_device.ps1`. Only Windows inventory, signature verification and process boundaries are mocked; these fixtures do not perform native driver operations.

Windows documentation:

PowerShell may report `UnknownError` for an intact local gamepad signature whose
self-signed root is untrusted. The local gamepad path may defer that status
to the independent verifier. CMS signer identity, payload locks and hashes,
native Windows signature integrity (success or `CERT_E_UNTRUSTEDROOT` only), and
catalog membership must all pass before exact-certificate consent is considered.
Post-consent Windows trust must still be `Valid` before staging. Display packages use the pinned expected release certificate and independent verifier before their separate trust provisioning; unreviewed self-signed catalogs remain rejected. Windows trust must be Valid after provisioning and before staging. Optional rejection/failure warnings explicitly identify unchanged driver state and possible unavailable virtual display.

For a normalized extraction of an existing signed nightly package, run
`powershell -NoProfile -File packaging/windows/test_gamepad_published_package.ps1 -FixtureRoot <path> -RepositoryRoot <repo>`.
This read-only test binds the current installer script into a scratch release
lock, verifies the existing signed payload, and rejects tampered or missing
payloads and incorrect pinned identity. It does not install or change trust.

- https://learn.microsoft.com/en-us/windows-hardware/drivers/install/trusted-root-certification-authorities-certificate-store
- https://learn.microsoft.com/en-us/windows-hardware/drivers/install/test-signing
- https://learn.microsoft.com/en-us/windows/win32/api/wintrust/ns-wintrust-wintrust_catalog_info

Legacy display certificate cleanup remains advisory. The packaged
`scripts/plan-legacy-driver-trust-cleanup.ps1` performs read-only inventory and
prints JSON; it has no apply mode, never imports/removes trust, and is not called
by MSI custom actions. Run it from native 64-bit Windows PowerShell in an elevated
terminal so DISM can inspect the online image. Any permission/query/signature or
parsing failure produces `Preserve` decisions and no removal proposal.

The planner pins only Sunshine
`37E100B2B382866FA7CE04EE33FE53DF53AEDC6D` and SudoVDA
`3C918FC73525AD8B1521B6DB26B71F694277CC49`. It inventories every DISM `-All`
package and active CIM INF binding, and cryptographically checks every catalog
under both `System32/DriverStore/FileRepository` and `System32/CatRoot`.
Inspecting only selected product INF names or versions would miss retained or
shared packages. Inbox INFs may omit CatalogFile, so their registered system
catalogs must also be included. Every literal third-party CatalogFile declaration
must resolve in the scan; unsupported/decorated syntax, missing catalogs,
reparse points, pending reboot/replacements, failed reads and unresolved active
bindings prevent all proposals. All embedded/nested certificate identities are
preserved conservatively, even if a legacy certificate is only present in a
catalog certificate set or the catalog no longer belongs to a selected product.
No subject-name matching or certificate-expiry shortcut is used.

Two identical complete snapshots, including INF/catalog hashes and trust
identities, are required before the report can list exact LocalMachine
Root/TrustedPublisher entries as `ReviewRemoval`. Inventory changes preserve
both pins. Unrelated publisher and VHF trust never enter that list. This is an
advisory observation, not a lock on Windows servicing or permission to apply an
old plan. Native Windows must still qualify completeness of the catalog roots,
DISM/CIM mappings, all supported OS/package forms and concurrent-servicing
behavior before installer cleanup can be designed/enabled. This read-only
planner alone does not resolve obsolete trust left by an upgrade or authorize
manual certificate deletion.

Run `pwsh -NoProfile -File packaging/windows/test_legacy_driver_trust_plan.ps1`
for the portable behavior fixtures. They execute the actual filesystem scanner,
CMS verifier and planner against existing public signed catalogs and changed
copies; DISM, CIM, reboot state and read-only trust-store queries are mocked.
They verify preservation for active/staged/shared legacy catalogs and unknown
state, exact-thumbprint proposals, inventory changes, malformed evidence, and
cryptographic tampering. These fixtures do not qualify native Windows inventory
or the installed driver outcome.

Inventory contract references: [Driver Store](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/driver-store),
[Catalog files and digital signatures](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/catalog-files),
[INF Version section](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-version-section),
and [Get-WindowsDriver](https://learn.microsoft.com/en-us/powershell/module/dism/get-windowsdriver).
