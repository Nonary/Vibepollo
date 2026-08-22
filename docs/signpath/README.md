# SignPath signing configuration

This directory documents how the Windows installer is code-signed through
[SignPath](https://signpath.io) and keeps **review copies** of the account-side
artifact-configuration XML so the signing rules are auditable in version control.

> **The SignPath portal is the source of truth.** The XML files here are copies
> for review. Changing them does **not** change signing behavior — you must edit
> the matching *artifact configuration* in the SignPath portal
> (Organization → Project `Vibepollo` → Artifact configurations).

## Why two signing requests ("the whole package, setup and all")

The shipped artifact is a single self-contained `VibepolloSetup.exe`. It is a
**custom C# bootstrapper** (`packaging/windows/bootstrapper/VibeshineInstaller.cs`)
that embeds the MSI as a **.NET managed manifest resource** named `Payload.msi`
(`build_bootstrapper.ps1` compiles with `csc /resource:<msi>,Payload.msi`; the
installer reads it back via `Assembly.GetManifestResourceStream("Payload.msi")`).

SignPath can only recurse into recognized container formats (MSI, CAB, ZIP,
NuGet, VSIX, APPX/MSIX, OPC, JAR, APK, directories). It **cannot** descend into a
.NET assembly's managed resources. Therefore a single recursive request over the
setup EXE can Authenticode-sign the outer EXE but can never reach the embedded
MSI or the binaries inside it.

So the whole package is signed with **two origin-verified requests**, in order:

1. **Deep-sign the MSI** (slug `msi-file-apollo`): signs every first-party PE *inside*
   the MSI (`Apollo\sunshine.exe`, `Apollo\uninstall.exe`, the
   `Apollo\tools\` executables, bundled DLLs), then signs the MSI container
   itself.
2. **Sign the setup EXE** (slug `setup-exe`): the bootstrapper is rebuilt
   embedding the already-signed MSI, then the outer EXE is Authenticode-signed.

Both requests use SignPath's GitHub trusted-build connector (origin verification):
the unsigned artifact is uploaded to GitHub Actions first and submitted by
`github-artifact-id`, so SignPath verifies GitHub produced the build. See
`.github/workflows/ci-windows.yml`.

Signing runs use the `release-signing` policy. Because that policy requires
manual approval, each signing job waits up to 5 hours 45 minutes for approval
and completion. Approve both requests in order: the MSI request first, followed
by the setup-EXE request after the signed MSI has been embedded.

## The Sunshine virtual-display catalog is signed in the MSI request

The Sunshine virtual-display package historically used a self-signed catalog
and installed its certificate into the machine trust stores. The MSI request
now signs `drivers/sunshine/SunshineVirtualDisplayDriver.cat` itself. The
catalog-bound `SunshineVirtualDisplayDriver.dll` is deliberately left byte-for-
byte unchanged; signing that DLL would invalidate the catalog. The installer
adds the exact SignPath publisher certificate to `TrustedPublisher` before PnP
installation, while leaving existing legacy trust and unchanged driver
payloads alone.

The artifact configuration requires exactly one catalog match. This is
intentional: a path mismatch must fail signing rather than publish an MSI that
falls back to an interactive driver-install prompt.

Windows PnP also requires the catalog's end-entity Authenticode certificate to
be established in the local machine `TrustedPublisher` store for a silent
third-party driver install. After validating the production catalog, the
driver installer imports only the exact `SignPath Foundation` signer
certificate there before invoking `pnputil`; it does not install a new root CA
or trust an arbitrary catalog signer.

A literal single request would require migrating off the custom bootstrapper to a
**WiX Burn** bundle (which SignPath can deep-sign), losing the custom installer
UX. That is intentionally out of scope.

## The VHF gamepad driver is signed here, not upstream

SignPath is authorised for this repository. It is **not** authorised for
`Nonary/libvirtualgamepad`, so that project cannot produce a production-signed
driver release no matter how its own CI is arranged. Waiting for an
origin-signed package would mean waiting forever.

So the driver package ships **unsigned** and is signed as part of request 1,
the MSI deep-sign, alongside the first-party PEs. Two files, and only two:

| File | Element | Why |
| --- | --- | --- |
| `driver/VibeshineVhfGamepad.cat` | `<catalog-file>` | A driver package is trusted through its catalogue. This is what makes it installable. |
| `tools/VibeshineVhfGamepadDeviceSetup.exe` | `<pe-file>` | Not listed in the INF's `CopyFiles`, so the catalogue does not hash it and signing it is safe. |

`VibeshineVhfGamepad.dll` is **never** signed. The catalogue hashes it, so an
Authenticode signature on the DLL changes the bytes the catalogue attests to
and the driver stops installing. The DLL's integrity comes from the signed
catalogue, which is how Windows validates a driver package anyway.

### What this changes about verification

The consumer workflow fetches the immutable public producer prerelease once in
a dedicated job, validates its lightweight tag target, exact four-asset set,
GitHub asset digests, external checksum, release lock, evidence, case-sensitive
ZIP layout, manifest and fresh-Inf2Cat hash binding, then passes that verified directory to
the Windows build as an internal artifact. The consumer never compiles the
driver, setup tool, or package.

What moves is *when* Authenticode is checked:

- **At ingest** (`download_libvirtualgamepad_release.ps1` and
  `refresh_driver_package.ps1`): CAT, DLL, and setup tool must explicitly be
  `NotSigned` with no signer. Every producer hash and fresh-Inf2Cat evidence
  hash is verified before staging, and an administrative extraction verifies
  the complete unsigned MSI payload before it is submitted to SignPath.
- **After signing** (CI "Verify SignPath signatures", and `install.ps1` on the
  user's machine): the catalogue and setup tool must carry a valid signature,
  and their manifest hashes are deliberately **not** checked, because signing
  rewrote them. The INF, DLL, manifest, `install.ps1`, and `cleanup.ps1` remain
  byte-pinned; production signing also rechecks catalog membership.

Before noninteractive PnP staging, the installed VHF script requires the CAT
and setup tool to have valid signatures from the same certificate and requires
both signer subjects to equal the stable `SignPath Foundation` production
identity already required for the virtual-display catalog. Only then does it
add that exact validated end-entity certificate to
`LocalMachine\TrustedPublisher` and verifies it was persisted. This trust step
is owned by the VHF package and does not depend on the optional virtual-display
driver having installed the same publisher first. A failure stops VHF staging;
the MSI's best-effort wrapper may report it as a driver warning, but never as a
successful VHF installation.

The package declares this with `signing.channel = "msi-request-signing"` and
lists the affected files under `signing.signed_downstream`, so the consumer does
not infer which hashes are expected to go stale.

### Approval order

Unchanged, and it matters more now: approve the **MSI request first**. The
driver catalogue is signed inside it, and the setup EXE request embeds the
already-signed MSI. Approving out of order ships an installer whose driver
cannot install.

## Slugs

| Slug | File | Used by | Purpose |
| --- | --- | --- | --- |
| `msi-file-apollo` | [`msi-file-apollo.artifact-config.xml`](msi-file-apollo.artifact-config.xml) | `ci-windows.yml` (MSI request), `scripts/signpath_sign.ps1` | Deep-sign nested first-party PEs, the VHF CAT/setup tool, then the MSI |
| `setup-exe` | [`setup-exe.artifact-config.xml`](setup-exe.artifact-config.xml) | `ci-windows.yml` (setup-EXE request) | Authenticode-sign the outer `VibepolloSetup.exe` |

Slugs and project/org/policy are passed as reusable-workflow inputs in
`ci-windows.yml` (`signpath_msi_artifact_configuration_slug`,
`signpath_artifact_configuration_slug`, `signpath_project_slug`, etc.).

## ⚠️ Do NOT re-sign vendor / catalog-bound files

Several files in the MSI are already signed by their upstream vendors and are
bound to a Windows **catalog** (`.cat`). Re-Authenticode-signing a driver DLL
invalidates the catalog hash and **breaks driver installation**. These must be
**excluded** from the `msi-file-apollo` deep-sign:

- `Apollo\drivers\sudovda\SudoVDA.dll`, `Apollo\drivers\sudovda\nefconc.exe` (CN=sudovda / Nefarius)
- `Apollo\drivers\sunshine\SunshineVirtualDisplayDriver.dll` (+ `.cat`),
  `Apollo\drivers\sunshine\virtualdisplay_probe.exe`,
  `Apollo\drivers\sunshine\nefconc.exe`,
  `Apollo\drivers\sunshine\vulkan-layer\VkLayer_sunshine_hdr.dll`
  (libvirtualdisplay release, origin-signed upstream)
- `Apollo\drivers\vhf-gamepad\driver\VibeshineVhfGamepad.dll` is catalog-bound and
  remains byte-for-byte unchanged. The VHF CAT and setup tool are deliberate
  exceptions: both arrive unsigned from the producer and are signed by this
  repository's MSI request.
- `nvngx_truehdr.dll` (NVIDIA RTX Video SDK runtime, downloaded from the pinned TrueHDR runtime release)

The recommended config signs the Sunshine catalog and explicitly excludes the
catalog-bound DLL and third-party binaries above.

## VHF gamepad release boundary

The Windows release flow pins `Nonary/libvirtualgamepad` tag
`v0.1.0-beta.2`, source/tag target
`52cbb8f27cbeb18baec53ca5d7b88081f0787a06`, and archive SHA-256
`354c11239a91fd9fb2f52d45449de8409d96a4e4ed998f2794b5465eaec2434b`.
It also requires DriverVer `08/21/2026,0.1.0.30` and protocol version 2.

The producer sidecars remain outside its ZIP. They travel only in the internal
CI artifact so the Windows build can revalidate the same public bytes without a
second fetch; they are not installed in the MSI. The installed
`release-lock.json` binds the producer identity and hashes plus the exact
`install.ps1` and `cleanup.ps1` bytes.

For local development, `SUNSHINE_ALLOW_LOCAL_VHF_GAMEPAD_TEST_PACKAGE` is an
explicit separate path. It accepts only the driver package's exported local
test certificate and never relaxes production release validation. Its
`self-signed-local-test` manifest must declare and hash that `.cer`; production
manifest file sets remain exact and exclude certificates.

## First-party PEs that MUST be signed

These are produced by this project and ship unsigned into the MSI (they are
stripped in CI and never signed on the runner). The `msi-file-apollo` config is the
**only** thing that signs them:

| File | Install location in MSI |
| --- | --- |
| `sunshine.exe` | `Apollo\` |
| `uninstall.exe` | `Apollo\` |
| `libwebrtc.dll` | `Apollo\` |
| `zlib1.dll` | `Apollo\` |
| `vibeshine_truehdr.dll` | `Apollo\` |
| `sunshinesvc.exe` | `Apollo\tools\` (bound via the `wix_payload` binder) |
| `dxgi-info.exe` | `Apollo\tools\` |
| `audio-info.exe` | `Apollo\tools\` |
| `playnite-launcher.exe` | `Apollo\tools\` |
| `sunshine_wgc_capture.exe` | `Apollo\tools\` |
| `sunshine_display_helper.exe` | `Apollo\tools\` |
| `virtualdisplay_probe.exe` | `Apollo\drivers\sunshine\` |
| `VkLayer_sunshine_hdr.dll` | `Apollo\drivers\sunshine\vulkan-layer\` |

> The paths in the artifact-configuration XML must match the MSI's logical
> directory layout. Confirm the exact in-MSI paths against a built MSI's File
> table if a `<pe-file>` entry reports zero matches.

`vibeshine_truehdr.dll` is the project-built shim and must be signed like other
first-party binaries when the pinned TrueHDR runtime bundle is included.
`nvngx_truehdr.dll` is NVIDIA's runtime and must not be re-signed.

## Two ways to write the `msi-file-apollo` config

- **Strategy 1 — explicit enumeration (recommended).** List each first-party PE
  with an explicit `<pe-file>`. It does not touch vendor or catalog-bound files;
  the Sunshine catalog is the deliberate catalog exception and is listed as a
  `<catalog-file>`. The list must be kept in sync when a new shipping binary is
  added — the CI verification gate (below) is the backstop that catches drift. This is what
  [`msi-file-apollo.artifact-config.xml`](msi-file-apollo.artifact-config.xml) ships.
- **Strategy 2 — scoped wildcards.** Use `<pe-file-set>` with `*`/`**` includes
  scoped to `Apollo\` and `Apollo\tools\` so the `drivers\` subtree is never swept in. Set
  `min-matches="0" max-matches="unbounded"` (the defaults are `1`, so a wildcard
  matching any other count fails the whole request). Shown as a comment in the
  XML. Only use this after confirming no catalog-bound driver DLLs live under the
  matched roots.

## CI verification gate (drift backstop)

`ci-windows.yml` runs a post-sign verification step (when SignPath is enabled)
that fails the build if the virtual-display catalog is missing or unsigned, or
if any first-party PE is unsigned. It:

1. confirms the outer `VibepolloSetup.exe` is signed,
2. confirms the signed MSI is signed,
3. administratively extracts the MSI and confirms the Sunshine virtual-display
   catalog carries a signature from SignPath Foundation,
4. confirms the complete VHF package and release lock are present, its CAT and
   setup tool are signed, its DLL remains unsigned and byte-pinned, and its
   production CAT still validates the INF and DLL,
5. confirms every first-party PE carries a signature, while **skipping** the
   vendor and catalog-bound files above.

This catches a portal misconfiguration (e.g. a container-only `msi-file-apollo` config,
or a newly added binary missing from Strategy-1 enumeration) before release.

## Portal setup checklist

1. Create/confirm the `msi-file-apollo` artifact configuration matches
   `msi-file-apollo.artifact-config.xml` (deep-signs first-party PEs and the Sunshine
   virtual-display catalog and VHF CAT/setup tool, while excluding catalog-bound
   DLLs and vendors).
2. Create/confirm the `setup-exe` artifact configuration matches
   `setup-exe.artifact-config.xml` (PE Authenticode).
3. Confirm the GitHub trusted-build system is linked to project `Vibepollo`.
4. Trigger `tester-windows-installer.yml` (or a release candidate) and confirm
   both unsigned-MSI and post-sign VHF verification gates pass.

## Local builds are unsigned by design

Local / offline packaging never signs. `package_installer` always passes
`-DisableSignPath`, and `build_bootstrapper.ps1` only signs when `-SignWithSignPath`
is passed explicitly. Origin verification is impossible from a developer machine
(the origin metadata must come from GitHub), so signing is a CI-only concern.
`scripts/signpath_sign.ps1` remains available for manual/test signing but is
**non-origin** and not a supported release path.
