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
**custom C# bootstrapper** (`packaging/windows/bootstrapper/VibepolloInstaller.cs`)
that embeds the MSI as a **.NET managed manifest resource** named `Payload.msi`
(`build_bootstrapper.ps1` compiles with `csc /resource:<msi>,Payload.msi`; the
installer reads it back via `Assembly.GetManifestResourceStream("Payload.msi")`).

SignPath can only recurse into recognized container formats (MSI, CAB, ZIP,
NuGet, VSIX, APPX/MSIX, OPC, JAR, APK, directories). It **cannot** descend into a
.NET assembly's managed resources. Therefore a single recursive request over the
setup EXE can Authenticode-sign the outer EXE but can never reach the embedded
MSI or the binaries inside it.

So the whole package is signed with **two origin-verified requests**, in order:

1. **Deep-sign the MSI** (slug `msi-install-apollo`): signs every first-party PE *inside*
   the MSI (`Apollo\sunshine.exe`, `Apollo\uninstall.exe`, the
   `Apollo\tools\` executables, bundled DLLs), then signs the MSI container
   itself.
2. **Sign the setup EXE** (slug `setup-exe`): the bootstrapper is rebuilt
   embedding the already-signed MSI, then the outer EXE is Authenticode-signed.

Both requests use SignPath's GitHub trusted-build connector (origin verification):
the unsigned artifact is uploaded to GitHub Actions first and submitted by
`github-artifact-id`, so SignPath verifies GitHub produced the build. See
`.github/workflows/ci-windows.yml`.

A literal single request would require migrating off the custom bootstrapper to a
**WiX Burn** bundle (which SignPath can deep-sign), losing the custom installer
UX. That is intentionally out of scope.

## Slugs

| Slug | File | Used by | Purpose |
| --- | --- | --- | --- |
| `msi-install-apollo` | [`msi-install-apollo.artifact-config.xml`](msi-install-apollo.artifact-config.xml) | `ci-windows.yml` (MSI request), `scripts/signpath_sign.ps1` | Deep-sign nested first-party PEs, then the MSI |
| `setup-exe` | [`setup-exe.artifact-config.xml`](setup-exe.artifact-config.xml) | `ci-windows.yml` (setup-EXE request) | Authenticode-sign the outer `VibepolloSetup.exe` |

Slugs and project/org/policy are passed as reusable-workflow inputs in
`ci-windows.yml` (`signpath_msi_artifact_configuration_slug`,
`signpath_artifact_configuration_slug`, `signpath_project_slug`, etc.).

## ⚠️ Do NOT re-sign vendor / catalog-bound files

Several files in the MSI are already signed by their upstream vendors and are
bound to a Windows **catalog** (`.cat`). Re-Authenticode-signing a driver DLL
invalidates the catalog hash and **breaks driver installation**. These must be
**excluded** from the `msi-install-apollo` deep-sign:

- `Apollo\drivers\sudovda\SudoVDA.dll`, `Apollo\drivers\sudovda\nefconc.exe` (CN=sudovda / Nefarius)
- `Apollo\drivers\sunshine\SunshineVirtualDisplayDriver.dll` (+ `.cat`),
  `Apollo\drivers\sunshine\virtualdisplay_probe.exe`,
  `Apollo\drivers\sunshine\nefconc.exe`,
  `Apollo\drivers\sunshine\vulkan-layer\VkLayer_sunshine_hdr.dll`
  (libvirtualdisplay release, origin-signed upstream)
- `nvngx_truehdr.dll`, `vibeshine_truehdr.dll` (optional NVIDIA/TrueHDR runtimes; not shipped by CI)

The recommended config (Strategy 1 below) excludes these by enumerating only
first-party files explicitly.

## First-party PEs that MUST be signed

These are produced by this project and ship unsigned into the MSI (they are
stripped in CI and never signed on the runner). The `msi-install-apollo` config is the
**only** thing that signs them:

| File | Install location in MSI |
| --- | --- |
| `sunshine.exe` | `Apollo\` |
| `uninstall.exe` | `Apollo\` |
| `libwebrtc.dll` | `Apollo\` |
| `zlib1.dll` | `Apollo\` |
| `sunshinesvc.exe` | `Apollo\tools\` (bound via the `wix_payload` binder) |
| `dxgi-info.exe` | `Apollo\tools\` |
| `audio-info.exe` | `Apollo\tools\` |
| `playnite-launcher.exe` | `Apollo\tools\` |
| `sunshine_wgc_capture.exe` | `Apollo\tools\` |
| `sunshine_display_helper.exe` | `Apollo\tools\` |

> The paths in the artifact-configuration XML must match the MSI's logical
> directory layout. Confirm the exact in-MSI paths against a built MSI's File
> table if a `<pe-file>` entry reports zero matches.

## Two ways to write the `msi-install-apollo` config

- **Strategy 1 — explicit enumeration (recommended).** List each first-party PE
  with an explicit `<pe-file>`. Never touches vendor/catalog files. Must be kept
  in sync when a new shipping binary is added — the CI verification gate (below)
  is the backstop that catches drift. This is what
  [`msi-install-apollo.artifact-config.xml`](msi-install-apollo.artifact-config.xml) ships.
- **Strategy 2 — scoped wildcards.** Use `<pe-file-set>` with `*`/`**` includes
  scoped to `Apollo\` and `Apollo\tools\` so the `drivers\` subtree is never swept in. Set
  `min-matches="0" max-matches="unbounded"` (the defaults are `1`, so a wildcard
  matching any other count fails the whole request). Shown as a comment in the
  XML. Only use this after confirming no catalog-bound driver DLLs live under the
  matched roots.

## CI verification gate (drift backstop)

`ci-windows.yml` runs a post-sign verification step (when SignPath is enabled)
that fails the build if any first-party PE is unsigned. It:

1. confirms the outer `VibepolloSetup.exe` is signed,
2. confirms the signed MSI is signed,
3. administratively extracts the MSI and confirms every first-party PE carries a
   signature, while **skipping** the vendor/catalog files above.

This catches a portal misconfiguration (e.g. a container-only `msi-install-apollo` config,
or a newly added binary missing from Strategy-1 enumeration) before release.

## Portal setup checklist

1. Create/confirm the `msi-install-apollo` artifact configuration matches
   `msi-install-apollo.artifact-config.xml` (deep-signs first-party PEs, excludes vendors).
2. Create/confirm the `setup-exe` artifact configuration matches
   `setup-exe.artifact-config.xml` (PE Authenticode).
3. Confirm the GitHub trusted-build system is linked to project `Vibepollo`.
4. Trigger `tester-windows-installer.yml` (or a release candidate) and confirm the
   verification gate passes.

## Local builds are unsigned by design

Local / offline packaging never signs. `package_installer` always passes
`-DisableSignPath`, and `build_bootstrapper.ps1` only signs when `-SignWithSignPath`
is passed explicitly. Origin verification is impossible from a developer machine
(the origin metadata must come from GitHub), so signing is a CI-only concern.
`scripts/signpath_sign.ps1` remains available for manual/test signing but is
**non-origin** and not a supported release path.
