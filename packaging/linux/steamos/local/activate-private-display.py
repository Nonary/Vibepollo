#!/usr/bin/python3
"""Stage and install a reversible, exact-kernel SteamOS private-display pool."""
import argparse
import grp
import hashlib
import importlib.util
import json
import os
import pathlib
import platform
import pwd
import re
import shutil
import stat
import subprocess
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('private_display_loader', HERE / 'private-display-loader.py')
loader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(loader)
UNITS = pathlib.Path('/etc/systemd/system')
VENDOR_UNITS = pathlib.Path('/usr/lib/systemd/system')


def generate_units(install_root, group):
    program = str(install_root / 'libexec/private-display-loader.py')
    helpers = str(install_root / 'libexec')
    return {
        'vibeshine-vkms.service': f'''[Unit]
Description=Vibeshine SteamOS managed display pool
Wants=vibeshine-vkms-control.socket
After=systemd-modules-load.service
Before=systemd-user-sessions.service display-manager.service

[Service]
Type=oneshot
RemainAfterExit=yes
RuntimeDirectory=vibeshine/vkms-leases
RuntimeDirectoryMode=0700
ExecStart=/usr/bin/python3 -I {program} start-pool
ExecStop=/usr/bin/python3 -I {program} stop-pool
ExecStopPost=/usr/bin/python3 -I {program} quiesce
TimeoutStartSec=30s
TimeoutStopSec=30s
UMask=0022

[Install]
WantedBy=multi-user.target
''',
        'vibeshine-vkms-control.socket': f'''[Unit]
Description=Vibeshine private-display control socket
PartOf=vibeshine-vkms.service

[Socket]
ListenStream=/run/vibeshine/vkms-control.sock
SocketMode=0660
SocketUser=root
SocketGroup={group}
DirectoryMode=0755
Accept=yes
RemoveOnStop=yes
MaxConnections=16
MaxConnectionsPerSource=4
''',
        'vibeshine-vkms-control@.service': f'''[Unit]
Description=Vibeshine private-display control connection
Requires=vibeshine-vkms.service
After=vibeshine-vkms.service

[Service]
Type=exec
User=root
ExecStart={helpers}/vibeshine-vkms-peercred {helpers}/private-display-mode-broker control
StandardInput=socket
StandardOutput=socket
StandardError=journal
UMask=0077
RuntimeMaxSec=5s
TimeoutStopSec=1s
KillMode=control-group
SendSIGKILL=yes
MemoryMax=32M
TasksMax=16
NoNewPrivileges=yes
PrivateTmp=yes
ProtectHome=yes
ProtectSystem=strict
ReadWritePaths=-/sys/kernel/config/vibeshine-drm /run/vibeshine/vkms-leases
ProtectKernelTunables=yes
ProtectKernelModules=yes
ProtectControlGroups=yes
RestrictSUIDSGID=yes
LockPersonality=yes
RestrictAddressFamilies=AF_UNIX
''',
    }


def stage(args):
    if not re.fullmatch(r'[a-z_][a-z0-9_-]{0,31}', args.user):
        raise RuntimeError('Invalid desktop user name')
    account = pwd.getpwnam(args.user)
    if account.pw_uid == 0:
        raise RuntimeError('Select the unprivileged desktop user')
    if args.output.exists() or args.output.is_symlink():
        raise RuntimeError('Staging requires a new output directory')
    module_hash = loader.digest(args.module)
    kernel = platform.release()
    inputs = {'module': module_hash, 'peercred': loader.digest(args.peercred),
              'loader': loader.digest(HERE / 'private-display-loader.py'),
              'installer': loader.digest(pathlib.Path(__file__)),
              'user': args.user, 'uid': account.pw_uid, 'gid': account.pw_gid}
    inputs['requested_mode_broker'] = loader.digest(HERE / 'private-display-mode-broker')
    for name in ('vibeshine-vkms', 'vibeshine-vkms-quiesce'):
        inputs[name] = loader.digest(args.driver_source / 'linux/packaging' / name)
    if args.capture_helper:
        inputs['capture_helper'] = loader.digest(args.capture_helper)
    bundle_id = hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()
    install_root = loader.INSTALL_PARENT / (kernel + '-' + bundle_id[:16])
    output = args.output.absolute()
    output.mkdir(parents=True, mode=0o755)
    try:
        for name in ('lib', 'libexec', 'units'):
            (output / name).mkdir(mode=0o755)
        shutil.copyfile(args.module, output / 'lib/vibeshine_drm.ko')
        shutil.copyfile(HERE / 'private-display-loader.py', output / 'libexec/private-display-loader.py')
        shutil.copyfile(args.peercred, output / 'libexec/vibeshine-vkms-peercred')
        shutil.copyfile(HERE / 'private-display-mode-broker', output / loader.MODE_BROKER)
        required = loader.REQUIRED_FILES | {loader.MODE_BROKER}
        if args.capture_helper:
            (output / 'bin').mkdir(mode=0o755)
            shutil.copyfile(args.capture_helper, output / loader.CAPTURE_HELPER)
            required.add(loader.CAPTURE_HELPER)
        for name in ('vibeshine-vkms', 'vibeshine-vkms-quiesce'):
            shutil.copyfile(args.driver_source / 'linux/packaging' / name, output / 'libexec' / name)
        for name, content in generate_units(install_root, grp.getgrgid(account.pw_gid).gr_name).items():
            (output / 'units' / name).write_text(content)
        for path in output.rglob('*'):
            path.chmod(0o755 if path.is_dir() or str(path.relative_to(output)) in loader.PROGRAM_FILES | {loader.CAPTURE_HELPER} else 0o644)
        module = output / 'lib/vibeshine_drm.ko'
        manifest = {
            'type': 'steamos-local-private-display-v1', 'kernel': kernel,
            'architecture': platform.machine(), 'install_root': str(install_root),
            'user': args.user, 'uid': account.pw_uid, 'gid': account.pw_gid,
            'bundle_id': bundle_id, 'capture_helper': args.capture_helper is not None,
            'requested_mode_broker': True,
            'module_sha256': module_hash,
            'module_version': loader.modinfo(module, 'version'),
            'module_srcversion': loader.modinfo(module, 'srcversion'),
            'files': {name: loader.digest(output / name) for name in sorted(required)},
        }
        (output / loader.MANIFEST).write_text(json.dumps(manifest, indent=2) + '\n')
        (output / loader.MANIFEST).chmod(0o644)
        loader.verify(output, privileged=False)
    except BaseException:
        shutil.rmtree(output)
        raise
    print(f'Staged {output}\nInstall destination: {install_root}\nNo module or service was changed.')


def require_root():
    if os.geteuid() != 0:
        raise RuntimeError('Root is required to install or operate the private-display pool')


def check_user(manifest):
    account = pwd.getpwnam(manifest['user'])
    if account.pw_uid == 0 or account.pw_uid != manifest['uid'] or account.pw_gid != manifest['gid']:
        raise RuntimeError('The staged desktop user identity changed')


def check_units(root, allow_missing=False):
    for name in loader.UNIT_NAMES:
        installed = UNITS / name
        if not installed.exists() and not installed.is_symlink():
            if allow_missing:
                continue
            raise RuntimeError(f'Private-display unit is missing: {installed}')
        info = installed.lstat()
        if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
            raise RuntimeError(f'Unsafe or conflicting private-display unit: {installed}')
        if installed.read_bytes() != (root / 'units' / name).read_bytes():
            raise RuntimeError(f'Another installation owns {installed}; retain it for rollback')


def selected_release():
    current = loader.INSTALL_PARENT / 'current'
    if not current.exists() and not current.is_symlink():
        return None
    if not current.is_symlink() or current.lstat().st_uid != 0:
        raise RuntimeError('Unsafe private-display release selection')
    target = pathlib.Path(os.readlink(current))
    if target.is_absolute() or len(target.parts) != 1 or target.name in ('.', '..'):
        raise RuntimeError('Private-display selection must name a retained immutable release')
    previous = loader.INSTALL_PARENT / target
    loader.verify(previous, require_kernel=False)
    return previous


def write_atomic(path, content, mode=0o644):
    descriptor, temporary = tempfile.mkstemp(prefix='.' + path.name + '-', dir=path.parent)
    temporary = pathlib.Path(temporary)
    try:
        with os.fdopen(descriptor, 'wb') as stream:
            stream.write(content)
            os.fchmod(stream.fileno(), mode)
            stream.flush()
            os.fsync(stream.fileno())
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def select_link(destination):
    current = loader.INSTALL_PARENT / 'current'
    descriptor, temporary = tempfile.mkstemp(prefix='.current-', dir=loader.INSTALL_PARENT)
    os.close(descriptor)
    temporary = pathlib.Path(temporary)
    temporary.unlink()
    try:
        temporary.symlink_to(destination.name)
        temporary.replace(current)
    finally:
        temporary.unlink(missing_ok=True)


def select_release(destination, previous):
    """Publish the next boot selection, preserving every previous release."""
    manifest = loader.verify(destination)
    check_user(manifest)
    if previous:
        old_manifest = loader.verify(previous, require_kernel=False)
        if any(old_manifest[key] != manifest[key] for key in ('user', 'uid', 'gid')):
            raise RuntimeError('A different desktop user owns the selected display release')
    check_units(previous or destination, allow_missing=previous is None)
    # Running accepted socket connections must retain the old broker command
    # until reboot. Reloading units here could mix a new broker and old module.
    defer_reload = loader.MODULE_SYSFS.exists() or loader.POOL.exists()
    originals = {}
    current = loader.INSTALL_PARENT / 'current'
    try:
        for name in loader.UNIT_NAMES:
            target = UNITS / name
            data = (destination / 'units' / name).read_bytes()
            if target.exists() and target.read_bytes() == data:
                continue
            originals[target] = ((target.read_bytes(), stat.S_IMODE(target.stat().st_mode))
                                 if target.exists() else None)
            write_atomic(target, data)
        select_link(destination)
        if not defer_reload:
            loader.command(['/usr/bin/systemctl', 'daemon-reload'])
    except BaseException:
        for target, original in originals.items():
            if original is None:
                target.unlink(missing_ok=True)
            else:
                write_atomic(target, *original)
        if previous:
            select_link(previous)
        else:
            current.unlink(missing_ok=True)
        if not defer_reload:
            subprocess.run(['/usr/bin/systemctl', 'daemon-reload'], check=False)
        raise
    if previous and previous != destination:
        print(f'Previous release retained: {previous}')
        print(f'Rollback boot selection: sudo python3 -I {HERE / "activate-private-display.py"} select-root --installed {previous}')
    if defer_reload:
        print('Next-boot selection updated. The loaded module, running pool, and cached service commands were retained. Reboot to activate this release.')


def install_root(args):
    require_root()
    source = args.stage.absolute()
    manifest = loader.verify(source, privileged=False)
    check_user(manifest)
    destination = pathlib.Path(manifest['install_root'])
    for parent in reversed(list(loader.INSTALL_PARENT.parents)):
        loader.safe_directory(parent)
    loader.INSTALL_PARENT.mkdir(mode=0o755, exist_ok=True)
    loader.safe_directory(loader.INSTALL_PARENT)
    loader.INSTALL_PARENT.chmod(0o755)
    loader.safe_directory(UNITS)
    if any((VENDOR_UNITS / name).exists() for name in loader.UNIT_NAMES):
        raise RuntimeError('A system package already owns the managed-display units')
    previous = selected_release()
    check_units(previous or source, allow_missing=previous is None)
    if destination.exists() or destination.is_symlink():
        if loader.verify(destination) != manifest:
            raise RuntimeError('Another candidate occupies the installation destination')
    else:
        temporary = pathlib.Path(tempfile.mkdtemp(prefix='.install-', dir=loader.INSTALL_PARENT))
        try:
            for name in set(manifest['files']) | {loader.MANIFEST}:
                target = temporary / name
                target.parent.mkdir(mode=0o755, parents=True, exist_ok=True)
                shutil.copyfile(source / name, target, follow_symlinks=False)
            if loader.verify(temporary, privileged=False) != manifest:
                raise RuntimeError('Staged bundle changed during installation')
            for path in [temporary, *temporary.rglob('*')]:
                os.chown(path, 0, 0, follow_symlinks=False)
                path.chmod(0o755 if path.is_dir() or str(path.relative_to(temporary)) in loader.PROGRAM_FILES else 0o644)
            if manifest['capture_helper']:
                helper = temporary / loader.CAPTURE_HELPER
                os.chown(helper, 0, manifest['gid'])
                helper.chmod(0o750)
                loader.command(['/usr/bin/setcap', 'cap_sys_admin=p', str(helper)])
            temporary.rename(destination)
            loader.verify(destination)
        finally:
            if temporary.exists():
                shutil.rmtree(temporary)
    select_release(destination, previous)
    print(f'Installed {destination}; no module was loaded and no service was enabled or started.')


def operate_root(args):
    require_root()
    root = args.installed.absolute()
    manifest = loader.verify(root, require_kernel=args.action not in ('disable-root', 'uninstall-root'))
    check_user(manifest)
    if args.action == 'select-root':
        loader.safe_directory(UNITS)
        if any((VENDOR_UNITS / name).exists() for name in loader.UNIT_NAMES):
            raise RuntimeError('A system package already owns the managed-display units')
        select_release(root, selected_release())
        print(f'Selected {root} for the next boot. No live service was changed.')
        return
    check_units(root)
    if args.action == 'enable-root':
        command = ['/usr/bin/systemctl']
        if loader.MODULE_SYSFS.exists() or loader.POOL.exists():
            command.append('--no-reload')
        loader.command(command + ['enable', 'vibeshine-vkms.service'])
        print('Managed outputs selected for next boot. The live session was not changed.')
    elif args.action == 'start-root':
        loader.verify_loaded(manifest)
        loader.verify_pool_features(manifest)
        loader.command(['/usr/bin/systemctl', 'start', 'vibeshine-vkms.service'])
        print('Managed output pool started. Outputs remain dormant until requested by a client.')
    elif args.action == 'disable-root':
        loader.command(['/usr/bin/systemctl', 'disable', 'vibeshine-vkms.service'])
        print('Next-boot activation disabled. The live session was not changed.')
    elif args.action == 'uninstall-root':
        if loader.MODULE_SYSFS.exists() or loader.POOL.exists():
            raise RuntimeError('Disable activation and reboot before uninstalling; the loaded display module will not be removed from a compositor')
        for name in ('vibeshine-vkms.service', 'vibeshine-vkms-control.socket'):
            active = subprocess.run(['/usr/bin/systemctl', 'is-active', '--quiet', name], check=False)
            if active.returncode == 0:
                raise RuntimeError('Private-display services are still active; disable activation and reboot first')
        loader.command(['/usr/bin/systemctl', 'disable', 'vibeshine-vkms.service'])
        for name in loader.UNIT_NAMES:
            (UNITS / name).unlink()
        loader.command(['/usr/bin/systemctl', 'daemon-reload'])
        current = loader.INSTALL_PARENT / 'current'
        if current.is_symlink() and current.lstat().st_uid == 0 and current.resolve() == root:
            current.unlink()
        shutil.rmtree(root)
        print('Removed the inactive private-display bundle and its owned units.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest='action', required=True)
    stage_parser = subparsers.add_parser('stage')
    for option in ('module', 'peercred', 'driver-source', 'output'):
        stage_parser.add_argument('--' + option, required=True, type=pathlib.Path)
    stage_parser.add_argument('--user', required=True)
    stage_parser.add_argument('--capture-helper', type=pathlib.Path)
    stage_parser.set_defaults(run=stage)
    install_parser = subparsers.add_parser('install-root')
    install_parser.add_argument('--stage', required=True, type=pathlib.Path)
    install_parser.set_defaults(run=install_root)
    for action in ('enable-root', 'start-root', 'disable-root', 'uninstall-root', 'select-root'):
        action_parser = subparsers.add_parser(action)
        action_parser.add_argument('--installed', required=True, type=pathlib.Path)
        action_parser.set_defaults(run=operate_root)
    args = parser.parse_args()
    try:
        args.run(args)
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'activate-private-display: {error}\n')


if __name__ == '__main__':
    main()
