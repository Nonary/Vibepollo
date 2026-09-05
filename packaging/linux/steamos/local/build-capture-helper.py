#!/usr/bin/env python3
"""Build the small capture helper without the host's application linker flags."""
import argparse
import pathlib
import re
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='cc', help='Native-compatible C compiler executable')
    parser.add_argument('--output', type=pathlib.Path, required=True)
    parser.add_argument('--drm-include', type=pathlib.Path, default=pathlib.Path('/usr/include/libdrm'))
    parser.add_argument('--readelf', default='readelf')
    args = parser.parse_args()
    repo = pathlib.Path(__file__).resolve().parents[4]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([
        args.cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
        '-fstack-protector-strong', '-D_FORTIFY_SOURCE=2', '-fPIE', '-pie',
        '-Wl,-z,relro,-z,now', '-I' + str(repo),
        '-I' + str(repo / 'third-party/libvirtualdisplay/linux/vibeshine-drm'),
        '-I' + str(args.drm_include),
        str(pathlib.Path(__file__).with_name('vibeshine-kms-capture.c')),
        '-ldrm', '-lcap', '-o', str(args.output),
    ], check=True)
    dynamic = subprocess.check_output([args.readelf, '-d', str(args.output)], text=True)
    needed = set(re.findall(r'\(NEEDED\).*\[([^]]+)\]', dynamic))
    if needed != {'libdrm.so.2', 'libcap.so.2', 'libc.so.6'} or re.search(r'\((RPATH|RUNPATH)\)', dynamic):
        raise RuntimeError('Capture helper has unexpected runtime dependencies or a library search path')
    print(f'Built {args.output}; only system libc, libdrm, and libcap are required.')


if __name__ == '__main__':
    main()
