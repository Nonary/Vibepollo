#!/usr/bin/env python3
"""Compile-only qualification of Vibepollo drivers on the booted Bazzite kernel.

Runs without sudo. Never signs, installs, loads, or unloads a module. All compiler
outputs and logs stay in a new directory underneath the current working directory.
Accepts either a repository checkout or the self-contained qualification bundle.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def run(args, **kwargs):
    return subprocess.run(args, check=True, text=True, **kwargs)


def checked_kernel(value):
    if not re.fullmatch(r"[A-Za-z0-9._+-]+", value):
        raise ValueError("Invalid kernel release")
    match = re.match(r"^(\d+)\.(\d+)(?:\.|[-+]|$)", value)
    if not match or tuple(map(int, match.groups())) < (6, 16):
        raise ValueError("The display driver requires Linux 6.16 or newer")
    return value


def verify_sources(root, manifest):
    # Hashes describe the bundled sources, not a publisher signature.
    for name, digest in manifest["files"].items():
        path = Path(name)
        if path.is_absolute() or ".." in path.parts:
            raise ValueError("Unsafe source path in manifest")
        target = root / path
        if target.is_symlink() or not target.is_file():
            raise ValueError(f"Missing or unsafe source: {name}")
        if hashlib.sha256(target.read_bytes()).hexdigest() != digest:
            raise ValueError(f"Source checksum mismatch: {name}")


def qualify(source, module, kernel, build, destination, jobs):
    target = destination / module
    shutil.copytree(source, target)
    if module == "vibeshine_drm":
        (target / "vibeshine_drm_version.h").write_text(
            '#define VIBESHINE_DRM_VERSION "0.0.0"\n'
        )
    args = ["make", "-C", str(build), f"M={target}", f"-j{jobs}"]
    config = build / ".config"
    if config.exists() and "CONFIG_CC_IS_CLANG=y" in config.read_text():
        args.append("LLVM=1")
    args.append("modules")
    logfile = destination / f"{module}.log"
    print(f"Building {module}; log: {logfile}", flush=True)
    with logfile.open("w") as log:
        proc = subprocess.run(args, stdout=log, stderr=subprocess.STDOUT)
    if proc.returncode:
        return {"status": "failed", "exit_code": proc.returncode, "log": logfile.name}
    artifact = target / f"{module}.ko"
    if not artifact.is_file() or artifact.is_symlink():
        return {"status": "failed", "error": "Build produced no kernel module"}
    vermagic = run(["modinfo", "-F", "vermagic", str(artifact)], capture_output=True).stdout.strip()
    if not vermagic or vermagic.split()[0] != kernel:
        return {"status": "failed", "error": f"Wrong module vermagic: {vermagic}"}
    return {"status": "compiled", "vermagic": vermagic,
            "sha256": hashlib.sha256(artifact.read_bytes()).hexdigest(), "log": logfile.name}


def main():
    if len(sys.argv) != 1:
        raise ValueError("Usage: python3 check-kernel.py (no sudo or arguments)")
    root = Path(__file__).resolve().parent
    manifest = json.loads((root / "sources.json").read_text())
    verify_sources(root, manifest)
    kernel = checked_kernel(os.uname().release)
    build = Path("/usr/lib/modules") / kernel / "build"
    if not (build / "Makefile").is_file():
        raise ValueError(f"Matching kernel-devel is missing: {build}")
    missing = [tool for tool in ("make", "modinfo") if not shutil.which(tool)]
    config = (build / ".config").read_text() if (build / ".config").exists() else ""
    compiler_tools = ("clang", "ld.lld", "llvm-nm", "llvm-objcopy") if "CONFIG_CC_IS_CLANG=y" in config else ("gcc",)
    missing.extend(tool for tool in compiler_tools if not shutil.which(tool))
    if missing:
        raise ValueError("Missing build tools: " + ", ".join(missing) +
                         ". Send this message back; do not change your OS installation.")
    destination = Path(tempfile.mkdtemp(prefix="vibepollo-kernel-check-", dir=Path.cwd()))
    report = {"kernel": kernel, "sources": manifest["commits"],
              "scope": "Unsigned compile only; loading, Secure Boot, SELinux, and streaming are untested",
              "modules": {}}
    jobs = min(os.cpu_count() or 1, 4)
    for module in ("vibeshine_drm", "vibeshine_ds5"):
        try:
            report["modules"][module] = qualify(root / "sources" / module, module, kernel, build, destination, jobs)
        except (OSError, subprocess.CalledProcessError) as exc:
            report["modules"][module] = {"status": "failed", "error": str(exc)}
        (destination / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    print(f"Send report.json and the two .log files from: {destination}")
    print("Nothing was installed or loaded. Secure Boot and SELinux were unchanged.")
    return int(any(item["status"] != "compiled" for item in report["modules"].values()))


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as exc:
        print(f"Kernel check stopped: {exc}", file=sys.stderr)
        sys.exit(1)
