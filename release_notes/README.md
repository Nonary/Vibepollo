# Release notes

Create one Markdown file per release tag before pushing the tag.

Examples:

- Tag `1.15.4` uses `release_notes/1.15.4.md` and publishes a stable release.
- Tag `1.15.4-stable` uses `release_notes/1.15.4-stable.md` and publishes a stable release.
- Tag `1.15.5-beta.1` uses `release_notes/1.15.5-beta.1.md` and publishes a pre-release.

Tag suffixes determine release type:

- Stable: unsuffixed semantic versions like `1.15.4`, or `-stable`.
- Pre-release: `-alpha`, `-beta`, or `-rc`, optionally followed by `.` or `-` metadata such as `-rc.1`.

Release tags should not include a `v` prefix. The workflow publishes the GitHub
Release title with the `v` prefix automatically, e.g. tag `1.15.4` is published
as `v1.15.4`.

If no matching notes file exists, the release job fails instead of publishing
generic generated notes.
