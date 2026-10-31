#!/usr/bin/python3 -I
"""Check the packaged driver; never rebuild or unload a running driver."""
import os
import subprocess
import sys
from pathlib import Path


def field(path, name):
    return subprocess.check_output(['/usr/sbin/modinfo', '-F', name, str(path)], text=True).strip()


def main():
    if sys.argv[1:] not in ([], ['--load']):
        raise RuntimeError('Expected no arguments or --load')
    kernel = os.uname().release
    module = Path('/usr/lib/modules') / kernel / 'extra/vibepollo/vibeshine_drm.ko'
    if not module.is_file():
        raise RuntimeError(f'Install a Vibepollo driver package for kernel {kernel}')
    if field(module, 'vermagic').split()[0] != kernel or field(module, 'version') != '2.0.2':
        raise RuntimeError('Packaged display driver does not match this kernel/application')
    expected = field(module, 'srcversion')
    if not expected or not field(module, 'signer'):
        raise RuntimeError('Packaged display driver has no source identity or signature')
    if sys.argv[1:] == ['--load']:
        subprocess.run(['/usr/sbin/modprobe', 'vibeshine_drm'], check=True)
    loaded = Path('/sys/module/vibeshine_drm/srcversion')
    if not loaded.is_file() or loaded.read_text().strip() != expected:
        raise RuntimeError('Expected display driver is not loaded; reboot after installation/enrollment')
    print(f'Vibepollo display driver is ready for {kernel}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError, IndexError) as error:
        print(f'Vibepollo driver check: {error}', file=sys.stderr)
        sys.exit(1)
