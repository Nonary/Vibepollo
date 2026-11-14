"""Select an audited VHF release for the exact consumer source; never download it."""
import argparse
import json
from pathlib import Path
import re
import subprocess


RELEASES = {
    "7b000a2b060b4daeb0b5c6aa7b77ae1d81857e7a": (
        "Nonary/libvirtualgamepad", "v0.1.0-beta.9",
        "59e89ab90c6e360e3e23bb4717b43339c4ec0050aedc52fe05e4f3ef306d62ee",
        "10/16/2026,0.1.0.53", "2"),
    "09e635cc3570268e7cd2cbda2a6a39d4efa6e174": (
        "jlobue10/libvirtualgamepad", "v0.1.0-beta.155",
        "56be2b6c18162f88c38db65b062c86d6d7eb596b6eb77dd19da721a24cb19b08",
        "10/24/2026,0.1.0.86", "2"),
    "5a3e4d222dd0b87556d4c564eb58b450ab579fb0": (
        "Nonary/libvirtualgamepad", "v0.1.0-beta.10",
        "650cf8281d95d1fb9388f8117cadeab195727e3dcb4105607df45080b15d312c",
        "10/29/2026,0.1.0.92", "2"),
}
FIELDS = {
    "REPOSITORY": "VHF_REPOSITORY",
    "RELEASE_TAG": "VHF_TAG",
    "RELEASE_ASSET_SHA256": "VHF_ARCHIVE_SHA256",
    "DRIVER_VER": "VHF_DRIVER_VER",
    "PROTOCOL_VERSION": "VHF_PROTOCOL_VERSION",
    "SOURCE_REVISION": "VHF_SOURCE_REVISION",
}


def select_identity(gitlink, contract):
    if gitlink not in RELEASES:
        raise ValueError(f"Unaudited libvirtualgamepad gitlink: {gitlink}")
    expected = dict(zip(FIELDS.values(), (*RELEASES[gitlink], gitlink)))
    for field, name in FIELDS.items():
        # Read only literal defaults. Do not execute CMake or accept interpolated pins.
        values = re.findall(
            rf'^set\(SUNSHINE_VHF_GAMEPAD_{field}\s+(?:"([^"\r\n]*)"|([^\s)]+))',
            contract, re.MULTILINE)
        if len(values) != 1 or (values[0][0] or values[0][1]) != expected[name]:
            raise ValueError(f"Source packaging contract disagrees with audited {name}")
    expected["VHF_ASSET_NAME"] = f"libvirtualgamepad-{expected['VHF_TAG'][1:]}-windows-x64.zip"
    return expected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--github-env", type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9a-f]{40}", args.source_commit):
        raise ValueError("Expected an exact consumer source SHA")

    def git(*arguments):
        return subprocess.check_output(["git", *arguments], text=True).strip()

    if git("rev-parse", "HEAD") != args.source_commit:
        raise ValueError("Checkout does not match the exact consumer source SHA")
    tree = git("ls-tree", args.source_commit, "third-party/libvirtualgamepad").split()
    if len(tree) != 4 or tree[:2] != ["160000", "commit"] or tree[3] != "third-party/libvirtualgamepad":
        raise ValueError("Consumer source has no exact libvirtualgamepad gitlink")
    contract = git("show", f"{args.source_commit}:cmake/packaging/windows_virtual_gamepad_contract.cmake")
    identity = select_identity(tree[2], contract)
    if args.github_env:
        with args.github_env.open("a", encoding="utf-8") as output:
            for name, value in identity.items():
                output.write(f"{name}={value}\n")
    print(json.dumps(identity, sort_keys=True))


if __name__ == "__main__":
    main()
