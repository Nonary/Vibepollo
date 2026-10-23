# Fixed issue follow-up metadata

The `fixed` label means that a maintainer believes a candidate change may address the report. It is not reporter confirmation and does not establish release inclusion. The issue stays open until the reporter confirms the result.

The automated daily test request fails closed unless a maintainer supplies a structured proof comment after the bot's candidate notice and before the daily is published. The proof comment must be authored by a repository owner, member, or collaborator and contain this marker:

```html
<!-- fixed-candidate:v1
{
  "source_commit": "<full 40-character commit SHA>",
  "dependencies": [
    {
      "lock_path": "third-party/libvirtualdisplay-release/windows-x64.json",
      "repository": "Nonary/libvirtualdisplay",
      "source_revision": "<full 40-character dependency source SHA>",
      "archive_sha256": "<64-character SHA-256 from that lock>"
    }
  ]
}
-->
```

`source_commit` must identify the specific candidate change. List every separately packaged dependency the fix requires. Each dependency must have a JSON lock file in the daily's tagged source tree with `schema_version: 1`, `repository`, `source_revision`, and `archive_sha256` matching the proof. The existing packaging step must validate and package the artifact against that lock. If no separate dependency is required, use an explicit empty list (`"dependencies": []`) only after verifying that none is required.

The handler reads the public GitHub Release for the named tag and uses its published status and `published_at`; release-event and `repository_dispatch` fields are only tag identifiers. It asks for a test only when that release is a published daily, the candidate commit is an ancestor of its immutable tag, and all declared dependency locks match the tagged source. A missing, malformed, late, untrusted, or mismatching proof, or an unavailable release lookup, produces no comment and leaves the issue open for a maintainer's verified follow-up. Timestamps order the notice and proof comments and identify retries; timestamps do not establish inclusion.

The follow-up marker is keyed to the candidate notice, source commit, and dependency identities, so release-event retries, the daily publisher's fallback event, and later dailies do not repeat a request for the same candidate cycle. Comment timestamps only establish ordering relative to publication; deterministic markers identify duplicates. No release path closes an issue or treats a keyword as confirmation.
