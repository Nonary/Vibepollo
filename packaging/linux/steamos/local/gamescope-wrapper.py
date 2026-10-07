#!/usr/bin/python3
"""Select this local compositor only while its pinned SteamOS inputs match."""
import hashlib
import json
import os
import pathlib
import platform
import shlex
import subprocess
import sys


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def os_identity():
    values = {}
    for line in pathlib.Path('/etc/os-release').read_text().splitlines():
        if '=' in line:
            key, value = line.split('=', 1)
            values[key] = shlex.split(value)[0] if value else ''
    return {key: values.get(key) for key in ('ID', 'VERSION_ID', 'BUILD_ID')}


def verify(root, privileged=True):
    manifest = json.loads((root / 'local-manifest.json').read_text())
    if not isinstance(manifest, dict) or not isinstance(manifest.get('files'), dict):
        raise RuntimeError('Invalid local Gamescope manifest schema')
    if (not isinstance(manifest.get('os'), dict)
            or not all(isinstance(manifest.get(key), str) for key in (
                'architecture', 'stock_gamescope_package', 'stock_gamescope_sha256', 'stock_session_sha256'))
            or not all(isinstance(name, str) and isinstance(checksum, str) and len(checksum) == 64
                       and all(char in '0123456789abcdef' for char in checksum)
                       for name, checksum in manifest['files'].items())):
        raise RuntimeError('Invalid local Gamescope manifest schema')
    required = {'bin/gamescope', 'libexec/gamescope', 'libexec/gamescope-session',
                'lib/libstdc++.so.6', 'lib/libgcc_s.so.1',
                'share/gamescope/scripts/00-gamescope/displays/valve.steamdeck.oled.lua'}
    if not required <= manifest['files'].keys():
        raise RuntimeError('Incomplete local Gamescope manifest')
    paths = list(root.rglob('*'))
    if any(path.is_symlink() for path in paths):
        raise RuntimeError('Symlinks are not accepted in the local Gamescope tree')
    inventory = {str(path.relative_to(root)) for path in paths
                 if path.is_file() and path != root / 'local-manifest.json'}
    if inventory != manifest['files'].keys():
        raise RuntimeError('Local Gamescope manifest does not match the file inventory')
    if manifest['os'] != os_identity() or manifest['architecture'] != platform.machine():
        raise RuntimeError('SteamOS version/build or architecture changed')
    package = subprocess.check_output(['/usr/bin/pacman', '-Q', 'gamescope'], text=True).strip()
    if package != manifest['stock_gamescope_package']:
        raise RuntimeError('Valve Gamescope package changed')
    if digest(pathlib.Path('/usr/bin/gamescope')) != manifest['stock_gamescope_sha256']:
        raise RuntimeError('Valve Gamescope executable changed')
    if digest(pathlib.Path('/usr/lib/steamos/gamescope-session')) != manifest['stock_session_sha256']:
        raise RuntimeError('Valve Gaming Mode session launcher changed')
    for relative, expected in manifest['files'].items():
        path = root / relative
        if pathlib.PurePosixPath(relative).is_absolute() or '..' in pathlib.PurePosixPath(relative).parts:
            raise RuntimeError('Invalid manifest path')
        if path.is_symlink() or digest(path) != expected:
            raise RuntimeError(f'Local Gamescope file changed: {relative}')
    if privileged:
        for path in [root, *root.parents, *root.rglob('*')]:
            info = path.stat()
            if info.st_uid != 0 or info.st_mode & 0o022 or path.is_symlink():
                raise RuntimeError(f'Gamescope path must be root-owned and unwritable by other users: {path}')
        verify_capabilities(root / 'libexec/gamescope')
    return manifest


def verify_capabilities(binary):
    capabilities = subprocess.check_output(['/usr/bin/getcap', str(binary)], text=True)
    if capabilities.strip() != str(binary) + ' cap_sys_nice=eip':
        raise RuntimeError('Gamescope CAP_SYS_NICE is missing or changed')


def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    session = sys.argv[1:] == ['--vibepollo-session']
    try:
        verify(root)
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
        print(f'Vibepollo local HDR compositor unavailable: {error}; using Valve Gamescope.', file=sys.stderr)
        if session:
            os.execv('/usr/lib/steamos/gamescope-session', ['gamescope-session'])
        os.execv('/usr/bin/gamescope', ['gamescope', *sys.argv[1:]])
    if session:
        os.execv('/bin/bash', ['bash', str(root / 'libexec/gamescope-session')])
    # Apply these only after the session script published its environment to
    # Steam. The private C++ runtime is selected by the ELF's absolute RUNPATH.
    os.environ['GAMESCOPE_SCRIPT_PATH'] = str(root / 'share/gamescope/scripts')
    os.execv(str(root / 'libexec/gamescope'), ['gamescope', *sys.argv[1:]])


if __name__ == '__main__':
    main()
