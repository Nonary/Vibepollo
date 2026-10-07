# Release notes

Create one Markdown file per release tag before pushing the tag. Each file is
the version-scoped changelog entry: include only changes introduced by that
tag, not a copy of earlier entries.

Examples:

- Tag `1.15.4` uses `release_notes/1.15.4.md` and publishes a stable release.
- Tag `1.15.4-stable` uses `release_notes/1.15.4-stable.md` and publishes a stable release.
- Tag `1.15.5-beta.1` uses `release_notes/1.15.5-beta.1.md` and publishes a pre-release.

Tag suffixes determine release type:

- Stable: unsuffixed semantic versions like `1.15.4`, or `-stable`.
- Pre-release: `-alpha`, `-beta`, or `-rc`, optionally followed by `.` or `-` metadata such as `-rc.1`.

Release tags should not include a `v` prefix. The workflow publishes the GitHub
Release title with the `v` prefix automatically, e.g. tag `1.15.4` is published
with the title `v1.15.4`. The GitHub Release remains attached to the original
v-less tag; the workflow must not synthesize a second `v1.15.4` tag.

If no exact matching notes file exists for the release tag, the release flow
skips instead of publishing generic generated notes.

The release workflow composes the public GitHub Release body automatically.
It includes every version-scoped entry in the same `major.minor.patch` release
line through the tagged version, so a release page is self-contained without
manually repeating prior line items. The Web UI changelog continues to show
each tag's individual file only.
