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
        is_stable_release = key[3] == 40
        if (
            key[:3] == target_line
            and key <= target_key
            and (is_stable_release if target_key[3] == 40 else version == release_version)
        ):
            notes.append((key, version, path))

    return sorted(notes, key=lambda note: note[0])


def body_without_title(content: str) -> str:
    lines = content.replace("\r\n", "\n").strip().split("\n")
    if lines and lines[0].startswith("# "):
        lines = lines[1:]
    return "\n".join(lines).strip()


def split_sections(body: str) -> tuple[str, list[tuple[str, str]]]:
    preamble: list[str] = []
    sections: list[tuple[str, str]] = []
    heading: str | None = None
    lines: list[str] = []

    for line in body.splitlines():
        if line.startswith("## "):
            if heading is not None:
                sections.append((heading, "\n".join(lines).strip()))
            heading = line[3:].strip()
            lines = []
        elif heading is None:
            preamble.append(line)
        else:
            lines.append(line)

    if heading is not None:
        sections.append((heading, "\n".join(lines).strip()))

    return "\n".join(preamble).strip(), sections


def content_blocks(content: str) -> list[str]:
    blocks: list[str] = []
    prose: list[str] = []
    bullet: list[str] = []

    def flush_prose() -> None:
        if (block := "\n".join(prose).strip()):
            blocks.append(block)
        prose.clear()

    def flush_bullet() -> None:
        if (block := "\n".join(bullet).strip()):
            blocks.append(block)
        bullet.clear()

    for line in content.splitlines():
        if line.startswith("- "):
            flush_prose()
            flush_bullet()
            bullet.append(line)
        elif bullet and (not line.strip() or line.startswith(("  ", "\t"))):
            bullet.append(line)
        elif bullet:
            flush_bullet()
            prose.append(line)
        else:
            prose.append(line)

    flush_bullet()
    flush_prose()
    return blocks


def normalized(block: str) -> str:
    return " ".join(block.split())


def unique_content(content: str, seen: set[str]) -> str:
    unique_blocks: list[str] = []
    for block in content_blocks(content):
        key = normalized(block)
        if key and key not in seen:
            seen.add(key)
            unique_blocks.append(block)
    return "\n\n".join(unique_blocks)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--release-version", required=True)
    parser.add_argument("--notes-dir", type=Path, default=Path("release_notes"))
    parser.add_argument("--product-name", default="Vibeshine")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    release_version = args.release_version.removeprefix("v")
    notes = release_notes(args.notes_dir, release_version)
    current = next((path for _, version, path in notes if version == release_version), None)
    if current is None:
        raise SystemExit(f"No exact release note found for {release_version} in {args.notes_dir}.")

    target_line = ".".join(str(part) for part in version_key(release_version)[:3])
    seen: set[str] = set()
    rendered_notes: list[str] = []
    for index, (_, version, path) in enumerate(notes):
        preamble, sections = split_sections(body_without_title(path.read_text(encoding="utf-8")))
        rendered_sections: list[str] = []
        for heading, content in sections:
            if index == 0:
                unique_content(content, seen)
                rendered_content = content
            else:
                rendered_content = unique_content(content, seen)
            if rendered_content:
                rendered_sections.append(f"### {heading}\n\n{rendered_content}")

        if index == 0:
            unique_content(preamble, seen)
            parts = [f"## {version}"]
            if preamble:
                parts.append(preamble)
            parts.extend(rendered_sections)
            rendered_notes.append("\n\n".join(parts))
        elif rendered_sections:
            rendered_notes.append(f"## {version}\n\n" + "\n\n".join(rendered_sections))

    public_body = "\n\n".join(
        (
            f"# {args.product_name} {release_version}",
            f"These release notes cover the stable {target_line} releases, from the original stable release through this update.",
            "\n\n".join(rendered_notes),
        )
    ).strip() + "\n"
    args.output.write_text(public_body, encoding="utf-8")


if __name__ == "__main__":
    main()
