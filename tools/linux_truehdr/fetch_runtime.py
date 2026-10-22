#!/usr/bin/env python3
"""Fetch the pinned NVIDIA Linux TrueHDR runtime without installing Python packages."""
import argparse
import hashlib
import os
from pathlib import Path
import tempfile
import urllib.request
import zipfile

WHEEL = 'nvidia_vfx-0.2.0.0-cp312-abi3-manylinux_2_27_x86_64.manylinux_2_28_x86_64.whl'
SHA256 = '0252866a7b798a6f92c8543443d88f716e8c5bbc6a7ddd77a2754728dada9fa8'
URL = 'https://pypi.nvidia.com/nvidia-vfx/' + WHEEL


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', type=Path, help='New directory for the extracted NVIDIA wheel')
    parser.add_argument('--wheel', type=Path, help='Use an already downloaded wheel (same pinned digest)')
    args = parser.parse_args()
    destination = args.destination.resolve()
    if destination.exists():
        parser.error('destination already exists; choose a new directory')
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='truehdr-fetch-', dir=destination.parent) as tmp:
        tmp = Path(tmp)
        wheel = args.wheel
        if wheel is None:
            wheel = tmp / WHEEL
            with urllib.request.urlopen(URL, timeout=60) as response, wheel.open('wb') as out:
                while chunk := response.read(1024 * 1024):
                    out.write(chunk)
        with wheel.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        if digest != SHA256:
            raise RuntimeError(f'Wheel digest mismatch: expected {SHA256}, got {digest}')
        staged = tmp / 'runtime'
        with zipfile.ZipFile(wheel) as archive:
            for member in archive.infolist():
                target = (staged / member.filename).resolve()
                if not target.is_relative_to(staged.resolve()):
                    raise RuntimeError('Archive member escapes runtime directory')
            archive.extractall(staged)
        (staged / 'runtime-provenance.txt').write_text(f'url={URL}\nsha256={SHA256}\n')
        os.rename(staged, destination)
    print(destination)


if __name__ == '__main__':
    main()
