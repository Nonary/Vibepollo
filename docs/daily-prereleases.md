# Daily Windows prereleases

The daily workflow is inactive by default. No activation variables are created by this change. Its UTC schedule is 06:17 for Vibeshine and 06:32 for Vibepollo. GitHub may delay scheduled runs.

## Validate before activation

Merge the workflow, helper scripts, driver safeguards and mandatory tests into the default branch and the intended source branch. The source branch must contain `packaging/windows/test_driver_install_safety.ps1` and `scripts/tests/test_daily_version_contract.cmake`; a legacy branch without the safeguards fails the daily build.

Set repository variables `DAILY_RELEASE_SOURCE_BRANCH` to the intended branch and `DAILY_RELEASE_SOURCE_CONFIRMED` to the exact `owner/repository:branch` string. For example, after independently confirming the source, `Nonary/vibeshine:vibe` or `Nonary/Vibepollo:master`. These are examples, not a selection of the release source. The repository's default branch is only the workflow control branch and does not prove the intended release source.

Dispatch **Daily Windows prerelease** from the default branch with `publish` left **false**. This permits a build, test and package validation while activation remains disabled. Inspect the Windows tests, driver safety matrix, extracted MSI safety-script hashes, finalized provenance and the MSI/setup run artifacts. Do not activate until the full remote build succeeds and fresh-install/upgrade/downgrade behavior is tested on suitable disposable Windows machines.

## Activate or stop

After validation and an explicit decision to publish daily builds, set `DAILY_RELEASE_ENABLED` to the exact string `true`. The schedule can then publish. A manual dispatch also requires `publish=true` to publish. Unset the variable or set it to `false` to stop future publication. Fork repositories and dispatches outside the default control branch are blocked.

The source and default control branch must have identical `.github/workflows` trees; the helper fails on a difference instead of requiring a new workflow-write credential during release creation.

Only the private runtime read credential `TRUEHDR_RUNTIME_TOKEN` is passed to the Windows workflow. No SignPath credential is inherited or passed, no symbols release is published, and daily builds use fresh hosted runners and fresh pinned upstream driver artifacts. Driver archive hashes and producer source revisions remain the existing release pins; no local candidate binaries or mutable driver rebuilds are selected.

## Identity and publication

One immutable `daily-YYYY-MM-DD` tag identifies each UTC day's exact source SHA. The application version retains the highest merged v-less source version tag's numeric base, for example `2.0.0-daily.d20261011+aaaaaaaaaaaa`. The date appears as a semantic prerelease identifier and never becomes the numeric major version. Invalid or unencodable source bases fail closed. The application retains the full version while WiX gives daily builds ordinal 90, below stable ordinal 99 at the same numeric base. UUIDv7 ProductCodes order separate packages. Installing a daily over a same-base stable release may require the installer's explicit downgrade flow.

The build must execute CTest, mandatory mocked driver policy tests, existing packaging contracts, and verify that the MSI contains the exact tested driver installer scripts. Publication checks require the finalized setup, tested MSI, source/run provenance and coherent tag/version identity. The release stays a draft until every asset uploads successfully, then becomes an explicit prerelease with `latest=false`. `SHA256SUMS`, `daily-manifest.json` and release notes expose provenance and the unsigned host status.

The host MSI/setup are unsigned. After verifying the immutable upstream VHF archive, the daily lane signs only its catalog and setup tool with an ephemeral artifact signing key and a self-issued, non-CA code-signing certificate. The INF and catalog-bound DLL remain byte-identical. The key and PFX stay outside the package and are deleted in `finally`; only the public CER ships. Updated manifest hashes, both signer thumbprints and a local-test release lock identify the signed payload. The original producer archive lock remains embedded in the manifest for audit. The certificate is valid for ten years; no timestamp service or persistent signing credential is used.

Daily MSIs carry `DAILY_UNSIGNED_DRIVER_POLICY=1`; stable packages retain zero. Runtime safeguards retain equal/newer installed drivers. A missing or genuinely newer gamepad can install only when Windows already reports its catalog as Valid. An untrusted self-signed package warns and leaves the driver unchanged until an explicit manual OS/security review; the installer does not import the certificate, enable test signing, change Secure Boot, or reboot. Fresh driver functionality may therefore remain unavailable. Artifact signing does not establish OS trust. Stable production signing and publisher-trust flow remain separate.

An existing daily tag or release, including a draft from a failed upload, blocks reruns. No daily workflow moves tags, overwrites assets, or deletes stable releases. Review a failed run and any partial draft manually before deciding recovery. Concurrent daily runs queue instead of cancelling an in-progress publication. The workflow summary records disabled, skipped, failed and published job outcomes.

Local contract tests do not prove a Windows application build, MSI extraction, real driver installation or publication. Those require the artifact-only remote validation and machine matrix before activation.

Real signing validation remains blocked: automatic approval review rejected creating the temporary RSA/PFX artifact signing identity under the task's no-new-credentials constraint. No key generation or signing test ran. Direct explicit approval for that ephemeral identity is required before its validation or activation; certificate-store imports and OS security changes remain excluded.
