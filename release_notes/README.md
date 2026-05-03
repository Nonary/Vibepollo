# Release notes

Create one Markdown file per release tag before pushing the tag.

Examples:

- Tag `1.15.4` uses `release_notes/1.15.4.md`.

Release tags should not include a `v` prefix. The workflow publishes the GitHub
Release title with the `v` prefix automatically, e.g. tag `1.15.4` is published
as `v1.15.4`.

If no matching notes file exists, the release job fails instead of publishing
generic generated notes.
