#!/usr/bin/env python3
"""Bundle exact gitlink driver sources for offline compile-only host testing."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[3]
DRIVERS = {
    "vibeshine_drm": ("third-party/libvirtualdisplay", "linux/vibeshine-drm"),
    "vibeshine_ds5": ("third-party/libvirtualgamepad", "linux/vibeshine-ds5"),
}


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args], text=True).strip()


def main(output):
    manifest = {"commits": {"Vibepollo": git("rev-parse", "HEAD")}, "files": {}}
    files = {}
    for module, (repo, driver) in DRIVERS.items():
        expected = git("rev-parse", f"HEAD:{repo}")
        if git("-C", str(ROOT / repo), "rev-parse", "HEAD") != expected:
            raise ValueError(f"Submodule is not at pinned commit: {repo}")
        if git("-C", str(ROOT / repo), "status", "--porcelain", "--", driver):
            raise ValueError(f"Driver sources have local changes: {repo}")
        manifest["commits"][repo] = expected
        names = git("-C", str(ROOT / repo), "ls-files", driver).splitlines()
        # Copy only tracked sources, helpers, and licensing; never build outputs.
        for name in names:
            source = ROOT / repo / name
            if source.is_symlink():
                raise ValueError(f"Unexpected source symlink: {source}")
            relative = Path(name).relative_to(driver)
            key = str(Path("sources") / module / relative)
            files[key] = source.read_bytes()
            manifest["files"][key] = hashlib.sha256(files[key]).hexdigest()
    for name in ("check-kernel.py", "KERNEL-CHECK.txt"):
        files[name] = (Path(__file__).parent / name).read_bytes()
    files["sources.json"] = (json.dumps(manifest, indent=2) + "\n").encode()
    # Include the top-level licenses as well as driver-specific licensing.
    for repo, _ in DRIVERS.values():
        for license in (ROOT / repo).glob("LICENSE*"):
            files[f"licenses/{Path(repo).name}-{license.name}"] = license.read_bytes()
    with zipfile.ZipFile(output, "x", compression=zipfile.ZIP_DEFLATED) as bundle:
        for name, content in sorted(files.items()):
            bundle.writestr("Vibepollo-Bazzite-Kernel-Check/" + name, content)
    print(output)


if __name__ == "__main__":
    main(sys.argv[1])
