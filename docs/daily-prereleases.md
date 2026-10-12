# Daily Windows prereleases

The daily workflow is inactive by default. No activation variables are created by this change. Its UTC schedule is 06:17 for Vibeshine and 06:32 for Vibepollo. GitHub may delay scheduled runs.

## Validate before activation

Merge the workflow, helper scripts, driver safeguards and mandatory tests into the default branch and the intended source branch. The source branch must contain `packaging/windows/test_driver_install_safety.ps1` and `scripts/tests/test_daily_version_contract.cmake`; a legacy branch without the safeguards fails the daily build.

Set repository variables `DAILY_RELEASE_SOURCE_BRANCH` to the intended branch and `DAILY_RELEASE_SOURCE_CONFIRMED` to the exact `owner/repository:branch` string. For example, after independently confirming the source, `Nonary/vibeshine:master` or `Nonary/Vibepollo:master`. The verified single primary is now `master` in both repositories; historical `vibe` and `vibe-test` are retired release lanes. The repository's default branch is only the workflow control branch and does not prove the intended release source.

Dispatch **Daily Windows prerelease** from the default branch with `publish` left **false**. This permits a build, test and package validation while activation remains disabled. Inspect the Windows tests, driver safety matrix, extracted MSI safety-script hashes, finalized provenance and the MSI/setup run artifacts. Do not activate until the full remote build succeeds and fresh-install/upgrade/downgrade behavior is tested on suitable disposable Windows machines.

## Activate or stop

After validation and an explicit decision to publish daily builds, set `DAILY_RELEASE_ENABLED` to the exact string `true`. The schedule can then publish. A manual dispatch also requires `publish=true` to publish. Unset the variable or set it to `false` to stop future publication. Fork repositories and dispatches outside the default control branch are blocked.

The source and default control branch must have identical `.github/workflows` trees; the helper fails on a difference instead of requiring a new workflow-write credential during release creation.

Only the private runtime read credential `TRUEHDR_RUNTIME_TOKEN` is passed to the Windows workflow. No SignPath credential is inherited or passed, no symbols release is published, and daily builds use fresh hosted runners and fresh pinned upstream driver artifacts. Driver archive hashes and producer source revisions remain the existing release pins; no local candidate binaries or mutable driver rebuilds are selected.

## Identity and publication

One immutable `daily-YYYY-MM-DD` tag identifies each UTC day's exact source SHA. The application version retains the highest merged v-less source version tag's numeric base, for example `2.0.0-daily.d20261011+aaaaaaaaaaaa`. The date appears as a semantic prerelease identifier and never becomes the numeric major version. Invalid or unencodable source bases fail closed. The application retains the full version while WiX gives daily builds ordinal 90, below stable ordinal 99 at the same numeric base. UUIDv7 ProductCodes order separate packages. Installing a daily over a same-base stable release may require the installer's explicit downgrade flow.

The build must execute CTest, mandatory mocked driver policy tests, existing packaging contracts, and verify that the MSI contains the exact tested driver installer scripts. Publication checks require the finalized setup, tested MSI, source/run provenance and coherent tag/version identity. The release stays a draft until every asset uploads successfully, then becomes an explicit prerelease with `latest=false`. `SHA256SUMS` and release notes expose integrity evidence and the unsigned host status. Qualification manifests stay in CI and the API identity; see [the public asset and compatibility contract](api-only-update-contract.md).

The host MSI/setup are unsigned. After verifying the immutable upstream VHF archive, the daily lane signs only its catalog and setup tool with an ephemeral artifact signing key and a self-issued, non-CA code-signing certificate. The INF and catalog-bound DLL remain byte-identical. The key and PFX stay outside the package and are deleted in `finally`; only the public CER ships. Updated manifest hashes, both signer thumbprints and a local-test release lock identify the signed payload. The original producer archive lock remains embedded in the manifest for audit. The certificate is valid for ten years; no timestamp service or persistent signing credential is used.

Daily MSIs carry `DAILY_UNSIGNED_DRIVER_POLICY=1`; stable packages retain zero. Runtime safeguards retain equal/newer installed drivers, and unknown inventory fails safely before package or certificate handling. Missing or genuinely older gamepad components can use the bundled self-signed package only after independent cryptographic catalog-member, setup signature, public DER certificate, release-lock and payload checks. If Windows does not already trust it, the installer requires a separate unchecked consent option displaying the exact signer, thumbprint, driver version and persistent machine-wide LocalMachine Root/TrustedPublisher scope. Consent is bound to that exact certificate through elevation and secure MSI properties. Quiet installs do not implicitly approve trust. Failed validation/staging/setup removes only trust entries newly added by this attempt, preserving preexisting and unrelated certificates. No Secure Boot or test-signing setting changes or automatic reboot occurs. Trust persists after uninstall; later genuinely newer components signed with a different daily identity require fresh approval. Equal/newer components skip both driver and certificate changes entirely.

Vibepollo additionally requires `DAILY_RELEASE_MIGRATION_CONFIRMED=Nonary/vibeshine:<upstream-master-sha>:Nonary/Vibepollo:<reviewed-master-sha>`. Both values must be exact current 40-character SHAs. This is an explicit reviewed migration attestation, not automatic conflict resolution; a source or target change blocks the lane until migration is reviewed again. The current upstream migration debt remains unresolved, so do not activate Vibepollo. A future automated migration must be an explicit dependency that produces a tested target SHA, followed by build/test and publication from that SHA; independent scheduled clocks cannot establish this guarantee.

An existing daily tag or release, including a draft from a failed upload, blocks reruns. No daily workflow moves tags, overwrites assets, or deletes stable releases. Review a failed run and any partial draft manually before deciding recovery. Concurrent daily runs queue instead of cancelling an in-progress publication. The workflow summary records disabled, skipped, failed and published job outcomes.

Local and read-only native contract tests do not prove a Windows application build, MSI extraction, real driver installation or publication. Those require the artifact-only remote validation and machine matrix before activation.

Real signing validation remains blocked: automatic approval review rejected creating the temporary RSA/PFX artifact signing identity under the task's no-new-credentials constraint. No key generation or signing test ran. Direct explicit approval for that ephemeral identity is required before its validation or activation; certificate-store imports and OS security changes remain excluded.

Primary migration on 2026-10-11: Vibeshine master fast-forwarded losslessly to 68f38d8b5c37c7c06817698c0c91f872f70c3325 and became the remote default; existing open PR bases moved to identical code on master. Legacy refs remain preserved pending semantic cleanup review. Vibepollo already uses master and must resolve its reviewed migration prerequisite.

## Reusable gamepad signing identity

Daily builds require `NIGHTLY_GAMEPAD_PFX_BASE64` and `NIGHTLY_GAMEPAD_PFX_PASSWORD` repository secrets, plus the public `NIGHTLY_GAMEPAD_SIGNER_THUMBPRINT` variable. Only these scoped signing secrets and the runtime token reach the reusable build; SignPath credentials are excluded. Each build signs a fresh copy of the verified producer with the same identity and exports only its public certificate. It refuses missing, mismatched, not-yet-valid or near-expiry identities; it never generates a replacement automatically or imports trust.

Keep private material outside the repository and downloadable packages. Back up the password-protected PFX securely. Rotation requires explicit approval, replacement of both secrets and the public pin together, and recipient consent for the new certificate; existing recipient trust is never removed by this pipeline. Equal/newer installed components bypass certificate and driver changes. Reusing the same trusted thumbprint avoids adding a new trust entry each night. Self-signing does not establish recipient trust or prove driver installation.
