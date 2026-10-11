# Driver installation safety

Windows display, legacy SudoVDA, and VHF gamepad installation compare the candidate INF's numeric four-part DriverVer component version with active CIM DriverVersion values and matching Driver Store INF versions. Dates, application versions, release tags and timestamps never decide replacement. Equal/newer active or staged components remain unchanged, including all certificate trust. Unknown versions, unreadable INF files and failed inventory queries block changes.

Display and SudoVDA admit missing or genuinely newer reviewed self-signed candidates after pinned public release-certificate identity, INF hardware/class identity, independent CMS signature and INF/DLL catalog membership checks. Eligible self-signed display candidates provision only the exact pinned certificate in LocalMachine Root and TrustedPublisher before Windows staging; equal/newer components add no trust. Provisioning or pre-replacement staging failures undo newly added certificates and retain pre-existing trust and drivers. Trusted production display catalogs retain their expected publisher identity and valid Windows chain; they add only TrustedPublisher. VHF can install a missing or genuinely newer self-signed component after independent package validation and informed recipient approval for the exact bundled certificate. The MSI and bootstrapper consent checkbox default to unchecked. Noninteractive installation requires explicit AllowLocalGamepadCertificateTrust=1 plus ConsentCertificateThumbprint matching the validated public certificate; a generic or mismatched approval cannot add trust. Existing exact trust requires no new import.

Before consent or import, VHF checks manifest payload hashes, release-lock identity and script hashes, real INF version, matching certificate identities for catalog/setup tool, public DER certificate format, certificate validity and code-signing-only/non-CA restrictions. SignedCms verifies the catalog and embedded executable signature cryptographically. WinVerifyTrust verifies executable integrity and actual catalog membership of INF/DLL using the open member file and Windows SIP hash; only success or an otherwise valid untrusted-root chain is eligible for consent. Missing signatures, changed bytes, wrong identities and invalid chains fail before trust changes.

Recipient consent covers persistent LocalMachine Root and TrustedPublisher trust for that exact public certificate. Root provides signature-chain trust; TrustedPublisher approves silent PnP publisher installation. This can authorize other code signed with the same key. A new signer per daily build requires new consent and can accumulate certificate entries, but application-only updates with equal/newer driver components add nothing. Only missing stores receive the certificate. Installation failure removes only additions successfully made by that attempt; preexisting and unrelated trust remains unchanged. Rollback errors report the exact store/thumbprint requiring attention. No Secure Boot, test-signing, service or boot-policy changes are used to make a rejected driver load.

VHF production publisher handling remains behind the guard with DailyUnsignedPolicy=0. DailyUnsignedPolicy=1 suppresses VHF production publisher imports; recipient-approved local VHF trust is separate. Display trust uses the independently verified expected release signer described above for both stable and daily packages. No SignPath service is invoked by these installation paths. Windows policy may still refuse a validated driver; the installer reports that outcome without weakening security settings.

Driver actions targeting System64Folder PowerShell use WixQuietExec64, because an x64 MSI can still contain a 32-bit custom-action DLL. Cleanup also resolves Sysnative explicitly when called from a 32-bit host. Before stopping driver services or removing existing nodes, Windows stages and verifies catalog members. SudoVDA staging failure preserves its node. Bootstrapper MSI pre-uninstalls set UPGRADINGPRODUCTCODE to preserve shared driver devices. Legacy non-MSI migrations fail safely when preservation cannot be guaranteed. Explicit requested uninstall remains available.

USB/IP uses the pinned original Microsoft-signed package and retains existing shared devices, services and staged packages. It does not invoke the vendor's destructive upgrade installer. ViGEm is not replaced by the VHF installer.

Run `powershell -NoProfile -File packaging/windows/test_driver_install_safety.ps1` before daily publication. It includes the numeric version/signature matrix, mocked exact-certificate consent/import/staging/rollback tests, compiled bootstrapper consent tests, and read-only native crypto tests against existing signed public fixtures and modified copies. These tests never import certificates, execute a driver installer or generate keys. Publication additionally compares extracted MSI script hashes with this source.

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
