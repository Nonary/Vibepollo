#!/usr/bin/env python3
"""Compose a public release page from version-scoped changelog entries."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


VERSION_RE = re.compile(r"^v?(\d+\.\d+\.\d+)([-.][0-9A-Za-z.-]+)?$")


def version_key(version: str) -> tuple[int, int, int, int, int, str]:
    match = VERSION_RE.fullmatch(version)
    if not match:
        raise ValueError(f"Unsupported release version: {version}")

    major, minor, patch = (int(part) for part in match.group(1).split("."))
    suffix = (match.group(2) or "").lstrip("-.").lower()
    suffix_parts = re.split(r"[-.]", suffix) if suffix else []
    suffix_num = next((int(part) for part in suffix_parts if part.isdigit()), 0)

    if not suffix or "stable" in suffix_parts:
        suffix_rank = 40
    elif "rc" in suffix_parts:
        suffix_rank = 30
    elif "beta" in suffix_parts:
        suffix_rank = 20
    elif "alpha" in suffix_parts:
        suffix_rank = 10
    else:
        suffix_rank = 5

    return (major, minor, patch, suffix_rank, suffix_num, suffix)


def release_notes(notes_dir: Path, release_version: str) -> list[tuple[tuple[int, int, int, int, int, str], str, Path]]:
    target_key = version_key(release_version)
    target_line = target_key[:3]
    notes: list[tuple[tuple[int, int, int, int, int, str], str, Path]] = []

    for path in notes_dir.glob("*.md"):
        version = path.stem.removeprefix("v")
        try:
            key = version_key(version)
        except ValueError:
            continue
        if key[:3] == target_line and key <= target_key:
            notes.append((key, version, path))

    return sorted(notes, key=lambda note: note[0], reverse=True)


def body_without_title(content: str) -> str:
    lines = content.replace("\r\n", "\n").strip().split("\n")
    if lines and lines[0].startswith("# "):
        lines = lines[1:]
    return "\n".join(lines).strip()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--release-version", required=True)
    parser.add_argument("--notes-dir", type=Path, default=Path("release_notes"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    release_version = args.release_version.removeprefix("v")
    notes = release_notes(args.notes_dir, release_version)
    current = next((path for _, version, path in notes if version == release_version), None)
    if current is None:
        raise SystemExit(f"No exact release note found for {release_version} in {args.notes_dir}.")

    current_body = body_without_title(current.read_text(encoding="utf-8"))
    sections = []
    for _, version, path in notes:
        body = body_without_title(path.read_text(encoding="utf-8"))
        sections.append(f"## {version}\n\n{body}" if body else f"## {version}")

    public_body = "\n\n".join(
        (
            "<!-- vibeshine-changelog:begin -->",
            current_body,
            "<!-- vibeshine-changelog:end -->",
            f"# Vibeshine {release_version}",
            "These release notes include every tagged change in this release line through this version.",
            "\n\n".join(sections),
        )
    ).strip() + "\n"
    args.output.write_text(public_body, encoding="utf-8")


if __name__ == "__main__":
    main()
