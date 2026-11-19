#!/usr/bin/env python3
"""Verify stable Arch release assets and assemble them into the Pages snapshot."""

import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile


PRODUCT = "vibepollo"
ARCH = "x86_64"
STABLE = r"[0-9]+\.[0-9]+\.[0-9]+"
PACKAGE = rf"{PRODUCT}-{STABLE}-[0-9]+(?:\.[0-9]+)?-{ARCH}\.pkg\.tar\.zst"
MANIFEST = "release-assets.tsv"


def select_asset(tag, release):
    if (
        not re.fullmatch(STABLE, tag)
        or release["tag_name"] != tag
        or release["draft"] is not False
        or release["prerelease"] is not False
        or not release["published_at"]
    ):
        raise ValueError("Expected the exact published numeric stable release")
    [asset] = [
        asset for asset in release["assets"]
        if asset["name"].startswith(f"{PRODUCT}-")
        and "-debug-" not in asset["name"]
        and asset["name"].endswith(".pkg.tar.zst")
    ]
    name = asset["name"]
    digest = asset["digest"] or ""
    if (
        not re.fullmatch(PACKAGE, name)
        or not name.startswith(f"{PRODUCT}-{tag}-")
        or asset["state"] != "uploaded"
        or not re.fullmatch(r"sha256:[0-9a-f]{64}", digest)
    ):
        raise ValueError("Expected the stable x86_64 package and published SHA-256 digest")
    return str(asset["id"]), name, digest.removeprefix("sha256:")


def verify_digest(path, digest):
    with path.open("rb") as package:
        if hashlib.file_digest(package, "sha256").hexdigest() != digest:
            raise ValueError(f"Release asset SHA-256 mismatch for {path.name}")


def verify_package(path, tag, digest):
    verify_digest(path, digest)
    info = subprocess.check_output(["bsdtar", "-xOf", path, ".PKGINFO"], text=True)
    fields = {}
    for line in info.splitlines():
        name, separator, value = line.partition(" = ")
        if separator:
            fields.setdefault(name, []).append(value)
    [version] = fields["pkgver"]
    if (
        fields["pkgname"] != [PRODUCT]
        or fields["arch"] != [ARCH]
        or not re.fullmatch(rf"(?:[1-9][0-9]*:)?{re.escape(tag)}-[0-9]+(?:\.[0-9]+)?", version)
        or path.name != f"{PRODUCT}-{version.split(':')[-1]}-{ARCH}.pkg.tar.zst"
    ):
        raise ValueError("Package metadata does not match the stable release asset")
    return version


def database_record(path):
    records = []
    with tarfile.open(path, "r:*") as archive:
        for member in archive:
            if not member.isfile() or not member.name.endswith("/desc"):
                continue
            sections = archive.extractfile(member).read().decode().strip().split("\n\n")
            fields = dict(section.split("\n", 1) for section in sections)
            if fields.get("%NAME%") != PRODUCT or fields.get("%ARCH%") != ARCH:
                raise ValueError("Unexpected package or architecture in signed index")
            records.append(tuple(fields[field] for field in ("%VERSION%", "%FILENAME%", "%SHA256SUM%")))
    [record] = records
    return record


def verify_signature(path, key):
    subprocess.run(
        ["gpgv", "--keyring", key.resolve(), f"{path}.sig", path], check=True,
    )


def restore_packages(directory):
    manifest = directory / MANIFEST
    key = directory / f"nonary-{PRODUCT}.gpg"
    db = directory / f"{PRODUCT}.db"
    files = directory / f"{PRODUCT}.files"
    for index in (db, files):
        verify_signature(index, key)
        for suffix in ("", ".sig"):
            archive = index.with_name(f"{index.name}.tar.gz{suffix}")
            alias = index.with_name(f"{index.name}{suffix}")
            if archive.read_bytes() != alias.read_bytes():
                raise ValueError(f"Index alias does not match {archive.name}")
    record = database_record(db)
    if record != database_record(files):
        raise ValueError("The sync and file indexes select different packages")
    version, current, expected_digest = record

    if not manifest.exists():
        # Legacy branches hold the package itself in the Git snapshot.
        package = directory / current
        if package.parent != directory:
            raise ValueError("Unexpected legacy package filename")
        verify_digest(package, expected_digest)
        verify_signature(package, key)
        return

    entries = {}
    for line in manifest.read_text().splitlines():
        name, digest = line.split("\t")
        if (
            not re.fullmatch(PACKAGE, name)
            or not re.fullmatch(r"[0-9a-f]{64}", digest)
            or name in entries
        ):
            raise ValueError("Invalid stable release asset entry")
        entries[name] = digest
    if len(entries) > 2 or current not in entries or entries[current] != expected_digest:
        raise ValueError("The release manifest must contain the indexed package and at most one previous package")

    legacy = list(directory.glob(f"{PRODUCT}-*.pkg.tar.zst"))
    if len(entries) + len(legacy) > 2:
        raise ValueError("Too many package payloads in the publication snapshot")
    for package in legacy:
        if not re.fullmatch(PACKAGE, package.name) or package.name in entries:
            raise ValueError("Unexpected legacy package in the release manifest snapshot")
        verify_signature(package, key)

    with tempfile.TemporaryDirectory(dir=directory) as temporary:
        for name, digest in entries.items():
            package = Path(temporary) / name
            tag = name.removeprefix(f"{PRODUCT}-").split("-", 1)[0]
            repository = os.environ["GITHUB_REPOSITORY"]
            url = f"https://github.com/{repository}/releases/download/{tag}/{name}"
            subprocess.run(
                ["curl", "--fail", "--silent", "--show-error", "--location",
                 "--retry", "3", "--max-time", "240", "--output", package, url],
                check=True,
            )
            package_version = verify_package(package, tag, digest)
            if name == current and package_version != version:
                raise ValueError("The indexed version does not match the release package")
            path = directory / name
            os.link(f"{path}.sig", f"{package}.sig")
            verify_signature(package, key)
            package.replace(path)


if __name__ == "__main__":
    operation, *arguments = sys.argv[1:]
    if operation == "select":
        print(*select_asset(arguments[0], json.load(sys.stdin)), sep="\t")
    elif operation == "verify":
        print(verify_package(Path(arguments[0]), *arguments[1:]))
    elif operation == "record":
        print(*database_record(Path(arguments[0])), sep="\t")
    elif operation == "restore":
        restore_packages(Path(arguments[0]))
    else:
        raise ValueError(f"Unknown operation: {operation}")
