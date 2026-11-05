# Published nightly notes and metadata

`nightly_notes.py` runs only in the gated publish job, after exact build artifact verification and before creating the draft. For new Vibepollo numeric releases it writes explicit `channel: nightly`, exact `tag`, `version`, and full `source_commit` identity in `nightly-metadata.json`; no date-derived sequence is emitted. It also records the exact source delta and regenerates `SHA256SUMS` including compatibility metadata and public packages (excluding the checksum file itself). Vibeshine and Moonlight keep their existing identity formats.

Ordinary application update checks use the identity embedded in the release API body and fetch no assets. See [the staged compatibility and public asset contract](../docs/api-only-update-contract.md). Historical assets are retained for installed clients.

For Vibepollo, one allocator serves stable and nightly releases. The version base is the highest published product version across channels; the source delta excludes commits already reachable from any published stable or qualified prerelease source. The exact latest published source, highest-version source, latest stable source, and latest qualified prerelease source must all pass ancestry checks. Unchanged sources and deltas with no conventional `feat`/`fix` commits are skipped. A failed reservation can be retried only with the same annotated tag, channel, and full source SHA. Draft releases never advance the published baseline. Legacy dated and suffixed nightly releases remain accepted as historical inputs; they do not create new suffix identities.

Schema version 1 fields:

| Field | Contract |
| --- | --- |
| `schema_version` | `1` |
| `channel` | `nightly`; for numeric Vibepollo tags this and GitHub `prerelease` identify the channel, not a version suffix |
| `tag`, `version` | Immutable tag and complete embedded application version |
| `source_commit` | Exact lowercase 40-character Git SHA |
| `windows_product_version` | Moonlight only: exact `6.2.B`; omitted for hosts. Prior metadata may omit it for migration, but a present value must match `B`. |
| `nightly_sequence` | Omitted for numeric Vibepollo tags. Historical host tags retain their existing date/revision sequence; Moonlight uses build `B`. |
| `previous_successful_tag`, `previous_source_commit` | Preceding published nightly identity; null for first nightly |
| `changelog_range` | Exact `previous_source_commit..source_commit`; null for first nightly |
| `compare_url` | Exact-SHA GitHub compare URL; null for first nightly |
| `first_nightly` | No previous successfully published nightly exists |
| `no_source_changes` | A previous baseline exists and its source SHA equals this source SHA |
| `commits_total`, `commits_listed` | Number of newly reachable commits in the exact range; all messages are listed |

New Vibepollo host releases use the same v-less numeric tag and application version, such as `2.0.16`, for either channel. Their annotated Git tag records `Vibepollo release channel: nightly` or `stable` and the exact source SHA. GitHub's release `prerelease` flag and metadata `channel` identify the channel for consumers. No numeric version is reused across channels. Previously published hash-only `{base}-nightly.dYYYYMMDD.{sha12}`, revisioned nightly, `daily-YYYY-MM-DD`, `{base}-daily.dYYYYMMDD+{sha12}`, stable-suffix and `v`-prefixed identities remain readable for compatibility. Vibeshine and Moonlight retain their existing nightly identities. Metadata carries the full source SHA independently of application version display. Clients enable prereleases by default with a persistent stable-only opt-out, and must require a non-draft prerelease with its expected packages and this metadata before treating it as available.

First nightly notes explicitly claim no baseline or delta. Same-source nightlies explicitly say no source changes and rebuild. Divergent/rewound history warns that `git log previous..source` lists newly reachable commits, without claiming forward ancestry; an empty rewind delta is distinct from identical sources.

Run focused offline checks with `python -m unittest discover -s scripts/tests -p test_nightly_notes.py -v`. No credential, signing, OS trust, release creation or network access is needed by these tests.

## Same-day transition and Windows versions

`r.N` appears before the random SHA so both revised clients and the shipped
date-only updater can order the bridge above every same-day hexadecimal SHA.
Numeric revisions order numerically, and the real UTC date orders days. No
release is deleted, retagged or given a fabricated date. New clients validate
the API identity, exact source suffix and sequence; malformed nightly shapes
cannot fall back to an ordinary prerelease. Already-installed date-only clients
recognize this bridge through their existing ordinary SemVer route, which
requires a complete platform package and prerelease opt-in but cannot enforce
the new metadata format. That old acceptance limit is tested against a frozen,
byte-identical copy of the shipped updater. Metadata/checksum assets remain
published for older clients; no installed code is changed remotely.

Host MSI ProductVersion retains its existing `patch * 100 + 90` nightly rank
below same-base stable `+99`. MSI does not order same-rank rebuilds or compare
the fourth ProductVersion field. The existing equal-version transactional
replacement, rollback marker and chronological UUIDv7 bootstrapper presentation
remain intact; no new MSI ordinal is invented. Executable FILEVERSION uses the
existing shared stable-tag anchor and UTC two-minute slots, with clock-rollback
and overflow failures retained. Two builds within the same slot may have equal
FILEVERSION. Actual package verification must report these values honestly.
