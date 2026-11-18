# Published nightly notes and metadata

`nightly_notes.py` runs only in the gated publish job, after exact build artifact verification and before creating the draft. It appends raw commit messages and exact GitHub commit/compare links to the existing security/build notes, writes `nightly-metadata.json`, and regenerates `SHA256SUMS` including compatibility metadata and public packages (excluding the checksum file itself).

Ordinary application update checks use the identity embedded in the release API body and fetch no assets. See [the staged compatibility and public asset contract](../docs/api-only-update-contract.md). Historical assets are retained for installed clients.

The notes metadata baseline is the most recently **published**, non-draft nightly prerelease returned by the paginated GitHub releases API. Failed builds and retained drafts do not advance it. Existing pre-metadata published date-style daily tags can provide a migration baseline by resolving their exact Git commit; a plain numeric tag is accepted as a nightly only when its embedded metadata qualifies its immutable channel, tag, version, and source SHA. No baseline is saved in the workflow workspace for future runs. The release allocator separately counts commits not reachable from *any* previously published release source, so a stable and nightly published from independent branches are not double-counted after they are merged.

Schema version 1 fields:

| Field | Contract |
| --- | --- |
| `schema_version` | `1` |
| `channel` | `nightly` |
| `tag`, `version` | Immutable tag and complete embedded application version |
| `source_commit` | Exact lowercase 40-character Git SHA |
| `windows_product_version` | Moonlight only: exact `6.2.B`; omitted for hosts. Prior metadata may omit it for migration, but a present value must match `B`. |
| `nightly_sequence` | Omitted for new plain-numeric Vibepollo nightlies; host legacy date identities retain their encoded sequence; Moonlight uses build `B` |
| `previous_successful_tag`, `previous_source_commit` | Preceding published nightly identity; null for first nightly |
| `changelog_range` | Exact `previous_source_commit..source_commit`; null for first nightly |
| `compare_url` | Exact-SHA GitHub compare URL; null for first nightly |
| `first_nightly` | No previous successfully published nightly exists |
| `no_source_changes` | A previous baseline exists and its source SHA equals this source SHA |
| `commits_total`, `commits_listed` | Number of newly reachable commits in the exact range; all messages are listed |

New Vibepollo nightlies use the allocated plain numeric version as tag, title, and application version. The immutable annotated tag records the release channel and source branch; metadata records `schema_version: 1`, `channel: nightly`, exact `tag`, exact numeric `version`, and the full lowercase source SHA. Numeric tags omit `nightly_sequence` because the shared numeric version is the ordering identity. The shared feature line and repository-local patch allocation are described below. Historical host `daily-YYYY-MM-DD`, `{base}-daily.dYYYYMMDD+{sha12}`, and `{base}-nightly.dYYYYMMDD.{sha12}` identities remain accepted. Moonlight uses `v6.1.0-daily.B` and `6.1.0-daily.B` (Windows product version `6.2.B`). Metadata carries the full source SHA independently of application version display. Clients enable prereleases by default with a persistent stable-only opt-out, and must require a non-draft prerelease with its expected packages and this metadata before treating it as available.

First nightly notes explicitly claim no baseline or delta. Same-source nightlies explicitly say no source changes and rebuild. Divergent/rewound history warns that `git log previous..source` lists newly reachable commits, without claiming forward ancestry; an empty rewind delta is distinct from identical sources.

## Shared feature line rollout

Vibeshine and Vibepollo derive one major/minor from the union of conventional
features reachable from their protected Canary and master branches. Each new
feature identity advances minor once, even if it reaches another branch or
repository in a later release. Use git cherry-pick -x for ports and preserve
the original message's first origin and any Gerrit-style Change-Id trailer,
including when resolving conflicts. A squash or manual port needs the same
origin or existing Change-Id. A feature without either identifier uses its own
commit SHA. Both repositories use their own published source SHAs to qualify
builds and count fixes. A late port can qualify locally and consume a patch
within the shared line. Matching major/minor says nothing about which features
are present in a particular build.

The one-time alignment is 2.18, above both published and reserved numeric
histories at the recorded cutover. The Canary pins are Vibeshine 2.14.8
(5b8f3c0e2b33bc0ddab59a1dbee490c51e2b2cc6) and Vibepollo 2.17.6
(e9bf2a6cb83ca989eca45d5725a325ea8c8c50af). Existing stable and older divergent
tag histories also seed identities. Neither repository builds just to pick up
the alignment. Its next qualifying build adopts the current line.

1. Coordinate both repositories. Keep publication and allocation paused
   while deploying, including manual stable signing and any active jobs.
   Recheck both live tag sets, including reservations, before adopting the
   cutover. Review any tag on or above 2.18 before enabling publication.
2. Ship the same scripts/shared_feature_line.py on both Canary and master
   branches. Bring the numeric stable allocator and signing validation into
   service on both default branches as part of the coordinated rollout.
   The cron launchers can continue to dispatch Canary at their current times.
3. Check both allocators read the same line and skip already released source
   SHAs. Exercise each nightly with publish=false, and validate updater and
   package ordering before restoring the normal scheduling and stable path.

The resolver reads both repositories without credentials. It checks branch
ancestry, protocol agreement, local tag freshness and the highest numeric
reservation across both repositories. It reads both ref sets again before
accepting the snapshot. Nightly and stable verify the candidate before writing
their own protected tag, then check the line again before publication. A
feature merged after the final successful check takes effect on the next
allocation. A reserved SHA can retry the same tag while it is valid on the
current line. If the line overtakes a reservation, leave the tag in place and
use a later qualifying source. Inspect retained upload drafts as described in
[the nightly runbook](../docs/daily-prereleases.md) before manual recovery.
A failed build or upload leaves the shared line and existing tags intact.

If a repository is unavailable, the input changes during validation, or the
branch/protocol checks fail, pause publication until the inputs are coherent.
After adoption, restore the protected history or roll forward with a coordinated
correction above both repositories' numeric high-water marks. Returning to
independent allocators can break the shared line. Preserve existing tags and
the exact source and channel recorded for every published artifact.

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
