#!/usr/bin/python3
"""Verify and operate one immutable SteamOS managed-display bundle."""
import argparse
import hashlib
import json
import os
import pathlib
import platform
import re
import stat
import subprocess

INSTALL_PARENT = pathlib.Path('/opt/vibeshine-private-display')
MODULE_SYSFS = pathlib.Path('/sys/module/vibeshine_drm')
POOL = pathlib.Path('/sys/kernel/config/vibeshine-drm/vibeshine')
UNIT_NAMES = ('vibeshine-vkms.service', 'vibeshine-vkms-control.socket',
              'vibeshine-vkms-control@.service')
PROGRAM_FILES = {'libexec/private-display-loader.py', 'libexec/vibeshine-vkms',
                 'libexec/vibeshine-vkms-peercred', 'libexec/vibeshine-vkms-quiesce'}
REQUIRED_FILES = PROGRAM_FILES | {'lib/vibeshine_drm.ko'} | {
    'units/' + name for name in UNIT_NAMES}
MANIFEST = 'private-display-manifest.json'
CAPTURE_HELPER = 'bin/vibeshine-kms-capture'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def command(arguments, **kwargs):
    return subprocess.run(arguments, check=True, text=True,
                          env={'PATH': '/usr/bin:/usr/sbin', 'LANG': 'C'}, **kwargs)


def modinfo(module, field):
    return command(['/usr/bin/modinfo', '-F', field, str(module)],
                   capture_output=True).stdout.strip()


def safe_directory(path):
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
        raise RuntimeError(f'Unsafe privileged directory: {path}')


def verify(root, privileged=True, require_kernel=True):
    root = pathlib.Path(root)
    if root.is_symlink() or not root.is_dir():
        raise RuntimeError('Bundle must be a real directory')
    if privileged:
        for parent in reversed([root, *root.parents]):
            safe_directory(parent)
    manifest_path = root / MANIFEST
    if manifest_path.is_symlink() or not manifest_path.is_file():
        raise RuntimeError('Bundle manifest is missing or unsafe')
    if privileged:
        info = manifest_path.stat()
        if info.st_uid != 0 or stat.S_IMODE(info.st_mode) != 0o644:
            raise RuntimeError('Bundle manifest is not immutable root-owned data')
    manifest = json.loads(manifest_path.read_text())
    if manifest.get('type') != 'steamos-local-private-display-v1':
        raise RuntimeError('Unknown private-display manifest')
    kernel = manifest.get('kernel', '')
    if not re.fullmatch(r'[A-Za-z0-9._+-]{1,200}', kernel):
        raise RuntimeError('Invalid kernel identity')
    if require_kernel and kernel != platform.release():
        raise RuntimeError('Kernel changed; rebuild the private-display module before activation')
    if manifest.get('architecture') != platform.machine():
        raise RuntimeError('Module architecture differs from this machine')
    module_hash = manifest.get('module_sha256', '')
    if not re.fullmatch(r'[0-9a-f]{64}', module_hash):
        raise RuntimeError('Invalid module checksum')
    bundle_id = manifest.get('bundle_id', '')
    if not re.fullmatch(r'[0-9a-f]{64}', bundle_id):
        raise RuntimeError('Invalid bundle identity')
    expected_root = INSTALL_PARENT / (kernel + '-' + bundle_id[:16])
    if manifest.get('install_root') != str(expected_root):
        raise RuntimeError('Invalid installation destination')
    if privileged and root != expected_root:
        raise RuntimeError('Bundle is outside its immutable installation destination')
    expected = manifest.get('files', {})
    required = REQUIRED_FILES | ({CAPTURE_HELPER} if manifest.get('capture_helper') is True else set())
    if set(expected) != required:
        raise RuntimeError('Unexpected bundle file inventory')
    actual = set()
    for path in root.rglob('*'):
        info = path.lstat()
        if stat.S_ISLNK(info.st_mode) or not (stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode)):
            raise RuntimeError(f'Unsupported bundle entry: {path}')
        if privileged and (info.st_uid != 0 or info.st_mode & 0o022):
            raise RuntimeError(f'Writable or non-root bundle entry: {path}')
        relative = str(path.relative_to(root))
        if privileged:
            mode = 0o755 if path.is_dir() or relative in PROGRAM_FILES else 0o644
            if relative == CAPTURE_HELPER:
                mode = 0o750
            if stat.S_IMODE(info.st_mode) != mode:
                raise RuntimeError(f'Unexpected bundle permissions: {path}')
        if path.is_file() and path != manifest_path:
            actual.add(relative)
            if relative not in expected or digest(path) != expected[relative]:
                raise RuntimeError(f'Bundle checksum mismatch: {relative}')
    if actual != required:
        raise RuntimeError('Incomplete bundle file inventory')
    module = root / 'lib/vibeshine_drm.ko'
    if digest(module) != module_hash or modinfo(module, 'name') != 'vibeshine_drm':
        raise RuntimeError('Unexpected kernel module')
    if modinfo(module, 'vermagic').split()[0] != kernel:
        raise RuntimeError('Module vermagic does not match the pinned kernel')
    if modinfo(module, 'depends'):
        raise RuntimeError('This additive loader requires a module with no external module dependencies')
    for field in ('version', 'srcversion'):
        if not manifest.get('module_' + field) or modinfo(module, field) != manifest['module_' + field]:
            raise RuntimeError(f'Module {field} mismatch')
    if manifest.get('capture_helper'):
        helper = root / CAPTURE_HELPER
        dynamic = command(['/usr/bin/readelf', '-d', str(helper)], capture_output=True).stdout
        if re.search(r'\((?:RPATH|RUNPATH)\)', dynamic):
            raise RuntimeError('Privileged capture helper must not have a runtime library search path')
        dependencies = set(re.findall(r'\(NEEDED\).*?\[([^]]+)\]', dynamic))
        if not dependencies or not dependencies <= {'libdrm.so.2', 'libcap.so.2', 'libc.so.6'}:
            raise RuntimeError('Privileged capture helper has unexpected dynamic dependencies')
        if privileged:
            info = helper.stat()
            if stat.S_IMODE(info.st_mode) != 0o750 or info.st_gid != manifest.get('gid'):
                raise RuntimeError('Privileged capture helper has unexpected permissions')
            capabilities = command(['/usr/bin/getcap', '-n', str(helper)], capture_output=True).stdout.strip()
            if capabilities != str(helper) + ' cap_sys_admin=p':
                raise RuntimeError('Privileged capture helper must have exactly cap_sys_admin=p')
    return manifest


def verify_loaded(manifest):
    if not MODULE_SYSFS.exists():
        return False
    for field in ('version', 'srcversion'):
        path = MODULE_SYSFS / field
        if not path.is_file() or path.read_text().strip() != manifest['module_' + field]:
            raise RuntimeError('A different private-display module is loaded; restart the machine before changing it')
    return True


def require_idle():
    if MODULE_SYSFS.exists():
        refcount = (MODULE_SYSFS / 'refcnt').read_text().strip()
        if not refcount.isdigit() or int(refcount) != 0:
            raise RuntimeError('The compositor still holds the private-display module; leave the graphical session first')
    if POOL.exists():
        for path in (POOL / 'connectors').glob('*/status'):
            if path.read_text().strip() != '2':
                raise RuntimeError('A private output is connected; end its stream before removing the pool')


def system_stopping():
    return subprocess.run(['/usr/bin/systemctl', 'is-system-running'], check=False,
                          capture_output=True, text=True,
                          env={'PATH': '/usr/bin:/usr/sbin', 'LANG': 'C'}).stdout.strip() == 'stopping'


def operate(root, action):
    if os.geteuid() != 0:
        raise RuntimeError('Private-display operations require root')
    manifest = verify(root)
    loaded = verify_loaded(manifest)
    if action == 'verify':
        return
    if action in ('load', 'start-pool'):
        if not loaded:
            command(['/usr/bin/insmod', str(root / 'lib/vibeshine_drm.ko'),
                     'create_default_dev=0'])
            if not verify_loaded(manifest):
                raise RuntimeError('The installed module did not become available')
        if action == 'start-pool':
            command(['/usr/bin/bash', str(root / 'libexec/vibeshine-vkms'), 'start'])
            command(['/usr/bin/udevadm', 'settle', '--timeout=10'])
    elif action == 'stop-pool':
        if not system_stopping():
            require_idle()
        command(['/usr/bin/bash', str(root / 'libexec/vibeshine-vkms'), 'stop'])
    elif action == 'quiesce':
        # ExecStopPost also runs after a refused manual stop. Only global
        # shutdown may quiesce buffers still held by a compositor.
        if loaded and system_stopping():
            command(['/usr/bin/bash', str(root / 'libexec/vibeshine-vkms-quiesce')])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('verify', 'load', 'start-pool', 'stop-pool', 'quiesce'))
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parent.parent
    try:
        operate(root, args.action)
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'private-display-loader: {error}\n')


if __name__ == '__main__':
    main()
