#!/usr/bin/env python3
"""Experimental shared native Linux build/install helper.

Includes the native payload and DRM/DKMS upgrades. Tests are opt-in (--enforce).
Never unloads a live display driver, restarts the compositor, or reboots itself.
See docs/linux/local-development.md for supported layouts and rollback caveats.
"""

import argparse
import base64
import contextlib
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import pwd
import re
import selectors
import shlex
import shutil
import signal
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
import uuid


REPO = Path(__file__).resolve().parent.parent
STATE = Path('/var/lib/vibepollo-local-deploy')
TRUSTED_PATH = '/usr/sbin:/usr/bin:/sbin:/bin'
HOST = 'vibepollo.service'
CONTROLLER = 'vibepollo-session-controller.service'
SOCKET = 'vibepollo-session-exec.socket'
HELPERS = (
    'app-supervisor', 'display-power', 'host',
    'kwin-session-environment', 'machine-host', 'profile-import', 'provider-scan',
    'session-broker', 'session-controller', 'session-exec', 'steam-launch',
)
UNITS = (HOST, CONTROLLER, SOCKET, 'vibepollo-session-exec@.service',
         'vibeshine-drm-setup.service', 'vibeshine-vkms.service',
         'vibeshine-vkms-control.socket', 'vibeshine-vkms-control@.service')
FIXED = {
    'usr/bin/vibepollo', 'usr/bin/vibepollo-mangohud',
    'usr/lib/libvibeshine-kwin-gpu.so',
    'usr/lib/modules-load.d/60-sunshine.conf',
    'usr/lib/udev/rules.d/60-sunshine.rules',
    'usr/lib/udev/rules.d/70-vibepollo-uinput.rules',
    'usr/lib/sysusers.d/vibepollo.conf', 'usr/lib/sysusers.d/vibeshine-vkms.conf',
    'usr/lib/firewalld/services/vibepollo.xml', 'etc/ufw/applications.d/vibepollo',
    'usr/share/pipewire/pipewire.conf.d/50-vibepollo-audio.conf',
    'usr/share/metainfo/io.github.Nonary.vibepollo.metainfo.xml',
}
FIXED.update(f'usr/libexec/vibeshine/vibepollo-{name}' for name in HELPERS)
FIXED.update(f'usr/libexec/vibeshine/vibeshine-{name}' for name in
             ('drm-install', 'vkms', 'vkms-peercred', 'vkms-quiesce'))
FIXED.update(f'usr/libexec/vibeshine/{name}' for name in
             ('vibepollo-profile-normalize.py', 'pairing_migration.py'))
FIXED.update(f'usr/lib/systemd/system/{name}' for name in UNITS)
FIXED.update(f'usr/share/applications/io.github.Nonary.vibepollo{suffix}.desktop'
             for suffix in ('', '.kwin', '.terminal'))
FIXED.add('usr/share/icons/hicolor/scalable/apps/apollo.svg')
OPTIONAL = {f'usr/share/icons/hicolor/scalable/status/apollo-{state}.svg'
            for state in ('locked', 'pausing', 'playing', 'tray')}
FIXED.update(f'usr/lib/systemd/user/{unit}.service.d/{dropin}.conf'
             for unit in ('plasma-kwin_wayland', 'plasma-login-kwin_wayland')
             for dropin in ('vibeshine-kwin-gpu', 'vibepollo-kwin-session-environment'))
VERSION = re.compile(r'[1-9][0-9]*\.[0-9]+\.[0-9]+(?:-(?:alpha|beta|rc|stable)\.[0-9]+)?')
TRANSACTION_ID = re.compile(r'[0-9]{8}T[0-9]{6}Z-[0-9a-f]{8}')
CAPABILITIES = {
    'usr/libexec/vibeshine/vibepollo-host': 'cap_sys_admin,cap_sys_nice=p',
    'usr/libexec/vibeshine/vibepollo-session-broker': 'cap_kill,cap_setgid,cap_setuid=p',
}
# These are the production paths of vibeshine-drm-install, not developer
# preferences. Do not accept caller-controlled privileged signing paths.
SIGNING_KEY = Path('/var/lib/dkms/mok.key')
SIGNING_CERTIFICATE = Path('/var/lib/dkms/mok.pub')


class DeployError(RuntimeError):
    pass


class DriverBusy(DeployError):
    def __init__(self, group):
        self.group = group
        super().__init__(f'Driver process group {group} did not drain; refusing file rollback until it exits or the machine reboots')


def run(*args, check=True, timeout=60, **kwargs):
    if (args[0] in ('depmod', 'modprobe', 'systemd-sysusers') or
            (args[0] == 'systemctl' and args[1] in ('stop', 'start', 'mask', 'unmask', 'daemon-reload')) or
            str(args[0]).endswith('vibepollo-session-controller')):
        print('+ ' + ' '.join(str(arg) for arg in args), flush=True)
    result = subprocess.run([str(arg) for arg in args], text=True,
                            stdout=subprocess.PIPE, stderr=kwargs.pop('stderr', subprocess.STDOUT),
                            timeout=timeout, **kwargs)
    if check and result.returncode:
        raise DeployError(f'{args[0]} failed ({result.returncode}):\n{result.stdout}')
    return result


def digest(path):
    with Path(path).open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def safe_name(name):
    return (bool(name) and not name.startswith('/') and
            all(part not in ('', '.', '..') for part in name.split('/')) and
            not any(ord(char) < 32 or ord(char) == 127 for char in name))


def allowed(name):
    return safe_name(name) and (
        name in FIXED or name in OPTIONAL or (name.startswith('usr/bin/vibepollo-') and
                         VERSION.fullmatch(name.removeprefix('usr/bin/vibepollo-')) is not None) or
        name.startswith(('usr/share/vibepollo/', 'usr/lib/vibepollo/')) or
        re.fullmatch(r'usr/src/vibeshine-drm-[1-9][0-9]*\.[0-9]+\.[0-9]+/[^/]+', name) is not None
    )


def inspect_archive(archive, version):
    members = {}
    total = 0
    for entry in archive:
        name = entry.name.rstrip('/') if entry.isdir() else entry.name
        if not safe_name(name) or name in members:
            raise DeployError(f'Unsafe/duplicate archive path: {entry.name!r}')
        if len(members) >= 25000 or entry.size < 0:
            raise DeployError('Archive limits exceeded')
        total += entry.size
        if total > 4 * 1024**3:
            raise DeployError('Archive exceeds 4 GiB unpacked')
        if not entry.isdir():
            if not allowed(name) or not (entry.isfile() or entry.issym()):
                raise DeployError(f'Unexpected artifact/type: {name}')
            if entry.issym() and (name != 'usr/bin/vibepollo' or
                                  entry.linkname != f'vibepollo-{version}'):
                raise DeployError(f'Unexpected symlink: {name}')
        members[name] = entry
    required = FIXED | {f'usr/bin/vibepollo-{version}',
                        'usr/share/vibepollo/web/index.html',
                        'usr/share/vibepollo/web/v2/index.html'}
    required.update(f'usr/src/vibeshine-drm-{version.split("-")[0]}/{name}' for name in
                    ('Makefile', 'build-module', 'dkms.conf', 'vkms_drv.c',
                     'vibeshine_drm_uapi.h', 'vibeshine_drm_version.h', 'vibeshine_drm_vrr.h'))
    if required - members.keys():
        raise DeployError(f'Missing artifacts: {sorted(required - members.keys())}')
    for name in required:
        if members[name].isdir():
            raise DeployError(f'Artifact is a directory: {name}')
    if not members['usr/bin/vibepollo'].issym():
        raise DeployError('Public executable must point to the versioned candidate')
    if {name for name in members if name.startswith('usr/bin/vibepollo-') and
            VERSION.fullmatch(name.removeprefix('usr/bin/vibepollo-'))} != {f'usr/bin/vibepollo-{version}'}:
        raise DeployError('Expected exactly one versioned public executable')
    drivers = {name.split('/')[2] for name in members if name.startswith('usr/src/') and
               not members[name].isdir()}
    if drivers != {f'vibeshine-drm-{version.split("-")[0]}'}:
        raise DeployError('Expected exactly one matching versioned DRM source tree')
    # No file may be used as another member's parent (including the public symlink).
    for name in members:
        for parent in PurePosixPath(name).parents:
            if str(parent) in members and not members[str(parent)].isdir():
                raise DeployError(f'Non-directory archive parent: {parent}')
    return {name: member for name, member in members.items() if not member.isdir()}


def sync_path(path, directory=False):
    fd = os.open(path, os.O_RDONLY | (os.O_DIRECTORY if directory else os.O_NOFOLLOW))
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def write_json(path, value):
    temporary = path.with_suffix('.new')
    with temporary.open('w') as output:
        json.dump(value, output, indent=2, sort_keys=True)
        output.flush()
        os.fsync(output.fileno())
    os.replace(temporary, path)
    sync_path(path.parent, directory=True)


def fingerprint(path):
    try:
        info = path.lstat()
    except FileNotFoundError:
        return None
    if stat.S_ISLNK(info.st_mode):
        return {'link': os.readlink(path)}
    if not stat.S_ISREG(info.st_mode):
        raise DeployError(f'Not a regular file: {path}')
    return {'sha256': digest(path)}


def metadata(path):
    info = path.lstat()
    return {
        'uid': info.st_uid, 'gid': info.st_gid, 'mode': stat.S_IMODE(info.st_mode),
        'xattrs': {} if stat.S_ISLNK(info.st_mode) else {
            key: base64.b64encode(os.getxattr(path, key)).decode('ascii')
            for key in os.listxattr(path)
        },
    }


class Files:
    """Exact-file journal; the alternate root/owner is used only by unit tests."""

    def __init__(self, transaction, root=Path('/'), owner=0, validator=allowed):
        self.transaction = transaction
        self.root = root
        self.owner = owner
        self.created_dirs = []
        self.validator = validator

    def parents(self, path, create=False):
        relative = path.relative_to(self.root)
        public_assets = relative.parts[:3] in (
            ('usr', 'share', 'vibepollo'), ('usr', 'lib', 'vibepollo'))
        cursor = self.root
        for part in relative.parts[:-1]:
            cursor /= part
            try:
                info = cursor.lstat()
            except FileNotFoundError:
                if not create:
                    return
                cursor.mkdir(mode=0o755)
                cursor.chmod(0o755)  # root's private umask must not leak into /usr.
                sync_path(cursor.parent, directory=True)
                self.created_dirs.append(str(cursor.relative_to(self.root)))
                info = cursor.lstat()
            if not stat.S_ISDIR(info.st_mode) or info.st_uid != self.owner or info.st_mode & 0o022:
                raise DeployError(f'Unsafe installation parent: {cursor}')
            if public_assets and not info.st_mode & 0o001:
                raise DeployError(f'Public asset directory is not traversable by the service account: {cursor}')

    def snapshot(self, names):
        saved = {}
        backup = self.transaction / 'before'
        backup.mkdir(mode=0o700)
        for index, name in enumerate(sorted(names)):
            if not self.validator(name):
                raise DeployError(f'Invalid backup target: {name}')
            target = self.root / name
            self.parents(target)
            content = fingerprint(target)
            if content is None:
                saved[name] = None
                continue
            info = target.lstat()
            if info.st_uid != self.owner:
                raise DeployError(f'Installed file is not administrator-owned: {target}')
            if stat.S_ISREG(info.st_mode) and info.st_mode & 0o022:
                raise DeployError(f'Installed file is writable outside its owner: {target}')
            record = dict(content, uid=info.st_uid, gid=info.st_gid,
                          mode=stat.S_IMODE(info.st_mode), mtime_ns=info.st_mtime_ns, xattrs={})
            if 'sha256' in content:
                filename = f'{index:06d}'
                shutil.copyfile(target, backup / filename, follow_symlinks=False)
                sync_path(backup / filename)
                if digest(backup / filename) != content['sha256']:
                    raise DeployError(f'Installed file changed while backing it up: {target}')
                record['backup'] = filename
                record['xattrs'] = {
                    key: base64.b64encode(os.getxattr(target, key)).decode('ascii')
                    for key in os.listxattr(target)
                }
            saved[name] = record
        sync_path(backup, directory=True)
        sync_path(self.transaction, directory=True)
        return saved

    def replace(self, name, source=None, link=None, mode=0o644, uid=0, gid=0, xattrs=None):
        target = self.root / name
        self.parents(target, create=True)
        if target.is_dir() and not target.is_symlink():
            raise DeployError(f'Refusing to replace a directory: {target}')
        fd, temporary_name = tempfile.mkstemp(prefix='.vibepollo-install-', dir=target.parent)
        temporary = Path(temporary_name)
        try:
            with os.fdopen(fd, 'wb') as output:
                if link is None:
                    with Path(source).open('rb') as data:
                        shutil.copyfileobj(data, output)
                    output.flush()
                    os.fsync(output.fileno())
            if link is not None:
                temporary.unlink()
                temporary.symlink_to(link)
            os.chown(temporary, uid, gid, follow_symlinks=False)
            if link is None:
                temporary.chmod(mode)
                for key, value in (xattrs or {}).items():
                    os.setxattr(temporary, key, base64.b64decode(value))
                sync_path(temporary)
            os.replace(temporary, target)
            sync_path(target.parent, directory=True)
        finally:
            temporary.unlink(missing_ok=True)

    def restore(self, saved):
        # Validate every backup before restoring even the first file.
        for name, record in saved.items():
            if not self.validator(name):
                raise DeployError(f'Invalid rollback path: {name}')
            if record and 'backup' in record:
                if not re.fullmatch(r'[0-9]{6}', record['backup']):
                    raise DeployError('Invalid backup filename')
                if digest(self.transaction / 'before' / record['backup']) != record['sha256']:
                    raise DeployError(f'Corrupt rollback backup: {name}')
        for name, record in saved.items():
            if record is None:
                self.remove(name)
            else:
                self.replace(name, source=self.transaction / 'before' / record.get('backup', ''),
                             link=record.get('link'), mode=record['mode'], uid=record['uid'],
                             gid=record['gid'], xattrs=record['xattrs'])
                os.utime(self.root / name, ns=(record['mtime_ns'], record['mtime_ns']), follow_symlinks=False)
                if 'link' not in record:
                    sync_path(self.root / name)
                sync_path((self.root / name).parent, directory=True)

    def remove(self, name):
        target = self.root / name
        self.parents(target)
        target.unlink(missing_ok=True)
        if target.parent.exists():
            sync_path(target.parent, directory=True)


def retain_failed_install(directory, manifest, phase, detail):
    manifest['failure'] = detail
    manifest['status'] = 'UNHEALTHY' if phase == 'readiness' else 'MUTATING'
    write_json(directory / 'transaction.json', manifest)
    if phase != 'readiness':
        # A partial payload stays stopped until explicitly recovered.
        quiesce()
    print('Installation retained for diagnosis; no files were reverted.\n'
          'Retry readiness: vibepollo-install --part2 (complete payloads only).\n'
          'Restore the previous installation: vibepollo-install --recover', file=sys.stderr)


def unit_properties(unit):
    result = run('systemctl', 'show', unit, '-p', 'ActiveState', '-p', 'ControlGroup',
                 '-p', 'InvocationID', '-p', 'LoadState', '-p', 'UnitFileState')
    return dict(line.split('=', 1) for line in result.stdout.splitlines() if '=' in line)


def capture_logs(invocation):
    if not re.fullmatch(r'[0-9a-f]{32}', invocation):
        return ''
    return run('journalctl', f'_SYSTEMD_INVOCATION_ID={invocation}', '--no-pager', '-o', 'cat',
               '-n', '100', '--grep', 'Found H.264 encoder:|Using event-driven KMS capture|'
               'Screencasting with KMS|CLIENT CONNECTED|Error:', check=False).stdout


def managed_pool_state(drm=Path('/sys/class/drm'), control=Path('/run/vibeshine/vkms-control.sock')):
    try:
        attributes = control.lstat()
        # Match native private_display::ready(): dormant connectors require
        # the trusted control endpoint that will connect them during launch.
        if (not stat.S_ISSOCK(attributes.st_mode) or attributes.st_uid != 0 or
                attributes.st_mode & 0o007):
            return None
        state = None
        for path in drm.glob('card*-Virtual-*/enabled'):
            card = path.parent.name.split('-')[0]
            try:
                if (drm / card / 'device').resolve(strict=True).name != 'vibeshine':
                    continue
                enabled = path.read_text().strip()
            except OSError:
                continue
            if enabled == 'enabled':
                return 'active'
            if enabled == 'disabled':
                state = 'idle'
        return state
    except OSError:
        return None


def health():
    try:
        units = {name: unit_properties(name) for name in (HOST, CONTROLLER, SOCKET)}
        if any(value.get('ActiveState') != 'active' for value in units.values()):
            return 'unhealthy', 'One or more native units are inactive'
        group = units[HOST].get('ControlGroup', '')
        if not group.startswith('/') or '..' in group.split('/'):
            return 'unknown', 'No valid host cgroup'
        pids = (Path('/sys/fs/cgroup') / group.lstrip('/') / 'cgroup.procs').read_text().split()
        listeners = run('ss', '-H', '-lntp', 'sport = :47990').stdout
        if not set(pids).intersection(re.findall(r'pid=([0-9]+)', listeners)):
            return 'unhealthy', 'Web UI listener is not owned by the current host cgroup'
        logs = capture_logs(units[HOST].get('InvocationID', ''))
        if 'Found H.264 encoder:' not in logs:
            return 'unknown', 'No H.264 readiness evidence for the current invocation'
        pool = managed_pool_state()
        if pool is None:
            return 'unhealthy', 'Managed virtual display pool or trusted control endpoint is unavailable'
        # Encoder probing can be synthetic when startup precedes DPMS wake.
        # Startup intentionally disconnects the idle pool when physical scanout
        # exists. Only a client launch connects/enables its private output;
        # neither virtual scanout nor capture is an idle startup requirement.
        display = 'active managed scanout' if pool == 'active' else 'dormant managed pool (idle)'
        return 'healthy', f'Startup ready: current host listener, H.264 and {display}; client capture untested'
    except (OSError, DeployError, subprocess.SubprocessError) as error:
        return 'unknown', str(error)


def broker_units():
    result = run('systemctl', 'list-units', '--all', '--plain', '--no-legend', '--no-pager',
                 'vibepollo-session-exec@*.service')
    names = [line.split()[0] for line in result.stdout.splitlines() if line.strip()]
    if any(not re.fullmatch(r'vibepollo-session-exec@[A-Za-z0-9_.\\-]+\.service', name) for name in names):
        raise DeployError('Unexpected broker instance name')
    return names


def quiesce():
    run('systemctl', 'mask', '--runtime', SOCKET)
    names = {SOCKET, CONTROLLER, HOST}
    groups = {unit_properties(name).get('ControlGroup', '') for name in names}
    run('systemctl', 'stop', SOCKET, timeout=60)
    accepted = set(broker_units())
    names.update(accepted)
    groups.update(unit_properties(name).get('ControlGroup', '') for name in accepted)
    for name in (CONTROLLER, HOST):
        run('systemctl', 'stop', name, timeout=60)
    # Admission is now closed. Collect every remaining accepted instance and
    # its cgroup before stopping it, including instances queued before closure.
    for _ in range(5):
        brokers = set(broker_units())
        groups.update(unit_properties(name).get('ControlGroup', '') for name in brokers)
        names.update(brokers)
        for name in brokers:
            run('systemctl', 'stop', name, timeout=60)
        if set(broker_units()) <= names:
            break
    else:
        raise DeployError('Broker instances did not converge after closing admission')
    run('/usr/libexec/vibeshine/vibepollo-session-controller', 'cleanup', timeout=45)
    for name in [*names, *broker_units()]:
        if unit_properties(name).get('ActiveState') not in ('inactive', 'failed'):
            raise DeployError(f'{name} did not stop')
    for group in filter(None, groups):
        if not group.startswith('/') or '..' in group.split('/'):
            raise DeployError('Unsafe service cgroup path')
        events = Path('/sys/fs/cgroup') / group.lstrip('/') / 'cgroup.events'
        if events.exists() and 'populated 0' not in events.read_text().splitlines():
            raise DeployError(f'Service cgroup still populated: {group}')
    if Path('/run/vibepollo/session.env').exists():
        raise DeployError('Old session binding survived cleanup')


def start_controller(active=True):
    run('systemctl', 'daemon-reload')
    run('systemctl', 'unmask', '--runtime', SOCKET)
    if active:
        run('systemctl', 'start', CONTROLLER, timeout=120)


def power_probe():
    record = Path('/run/vibepollo/session.env')
    account = pwd.getpwnam('vibepollo')
    info = record.lstat()
    if not stat.S_ISREG(info.st_mode) or (info.st_uid, info.st_gid, stat.S_IMODE(info.st_mode)) != (0, account.pw_gid, 0o640):
        raise DeployError('Untrusted controller session record')
    lines = [line.split('=', 1) for line in record.read_text().splitlines()]
    generations = [value for key, value in lines if key == 'generation']
    if len(generations) != 1 or not re.fullmatch('[1-9][0-9]*', generations[0]):
        raise DeployError('Invalid session generation')
    process = subprocess.Popen([
        'setpriv', '--reuid', str(account.pw_uid), '--regid', str(account.pw_gid),
        '--init-groups', '--no-new-privs', '--', 'env',
        f'VIBEPOLLO_SESSION_GENERATION={generations[0]}',
        '/usr/libexec/vibeshine/vibepollo-session-exec', 'display-power',
    ], stdout=subprocess.PIPE)
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            if not selector.select(9) or os.read(process.stdout.fileno(), 1) != b'R':
                raise DeployError('Generation-bound display power helper did not become ready')
        deadline = time.monotonic() + 3
        while True:
            result = health()
            if result[0] == 'healthy' or time.monotonic() >= deadline:
                return result
            time.sleep(0.2)
    finally:
        process.kill()
        process.wait(timeout=5)
        process.stdout.close()


def readiness(timeout):
    deadline = time.monotonic() + timeout
    detail = 'Controller/host did not become ready before the deadline'
    while time.monotonic() < deadline:
        if all(unit_properties(name).get('ActiveState') == 'active' for name in (HOST, CONTROLLER, SOCKET)):
            try:
                result, detail = power_probe()
                if result == 'healthy':
                    return result, detail
            except (OSError, DeployError, subprocess.SubprocessError) as error:
                detail = str(error)
        time.sleep(1)
    active_session = run('loginctl', 'show-seat', 'seat0', '-p', 'ActiveSession', '--value', check=False)
    session = active_session.stdout.strip()
    graphical = True
    if session and re.fullmatch(r'[A-Za-z0-9_-]+', session):
        kind = run('loginctl', 'show-session', session, '-p', 'Type', '--value', check=False)
        graphical = kind.returncode != 0 or kind.stdout.strip() == 'wayland'
    if (active_session.returncode == 0 and (not session or not graphical) and
            not Path('/run/vibepollo/session.env').exists() and
            unit_properties(CONTROLLER).get('ActiveState') == 'active'):
        return 'waiting-session', 'Installed controller is waiting for a local graphical session'
    return 'unhealthy', detail


def install_mode(name, _source_mode=0):
    if name.endswith('/vibepollo-host'):
        return 0o750
    if name.endswith('/vibepollo-session-broker'):
        return 0o700
    if name == 'usr/lib/libvibeshine-kwin-gpu.so':
        return 0o4755
    if (name.startswith('usr/bin/') or name.startswith('usr/libexec/vibeshine/') or
            re.fullmatch(r'usr/src/vibeshine-drm-[0-9.]+/build-module', name)):
        return 0o755
    return 0o644


def driver_preflight(candidate, version):
    # Source upgrades and a missing/stale module are expected, not blockers.
    name = f'usr/src/vibeshine-drm-{version.split("-")[0]}'
    for staged in (candidate / name).iterdir():
        if not stat.S_ISREG(staged.lstat().st_mode):
            raise DeployError(f'Unexpected driver source type: {staged}')
    kernel = os.uname().release
    if not (Path('/usr/lib/modules') / kernel / 'build/Makefile').is_file():
        raise DeployError(f'Install matching kernel headers for {kernel} before building its driver')


def driver_allowed(name):
    return safe_name(name) and bool(re.fullmatch(
        r'(?:usr/src/vibeshine-drm-[0-9][A-Za-z0-9._+-]*(?:/.*)?|'
        r'var/lib/(?:dkms/vibeshine-drm|vibeshine-drm)(?:/.*)?|'
        r'usr/lib/modules/[A-Za-z0-9._+-]+/(?:[^/]+/)*vibeshine_drm\.ko(?:\.(?:zst|xz|gz))?)', name))


def driver_inventory(root=Path('/'), owner=0):
    """Only this module's state; never follow DKMS build/source symlinks."""
    files, directories = set(), {}
    roots = list((root / 'usr/src').glob('vibeshine-drm-*'))
    roots += [root / 'var/lib/dkms/vibeshine-drm', root / 'var/lib/vibeshine-drm']
    guard = Files(root, root=root, owner=owner, validator=driver_allowed)
    def visit(path):
        name = str(path.relative_to(root))
        if not driver_allowed(name):
            raise DeployError(f'Unexpected driver state: {path}')
        guard.parents(path)
        info = path.lstat()
        if info.st_uid != owner or (not stat.S_ISLNK(info.st_mode) and info.st_mode & 0o022):
            raise DeployError(f'Untrusted driver state: {path}')
        if stat.S_ISDIR(info.st_mode):
            if path.is_mount():
                raise DeployError(f'Refusing mounted driver state: {path}')
            directories[name] = metadata(path)
            for child in path.iterdir():
                visit(child)
        elif stat.S_ISREG(info.st_mode) or stat.S_ISLNK(info.st_mode):
            if stat.S_ISLNK(info.st_mode):
                # DKMS build/source/kernel aliases must stay inside the exact
                # driver backup domain, not redirect a privileged build elsewhere.
                target = path.resolve()
                if not target.is_relative_to(root) or not driver_allowed(str(target.relative_to(root))):
                    raise DeployError(f'Driver link escapes the backed-up domain: {path}')
            files.add(name)
        else:
            raise DeployError(f'Special driver state: {path}')
        if len(files) + len(directories) > 50000:
            raise DeployError('Driver backup file limit exceeded')
    for path in roots:
        if os.path.lexists(path):
            if path.is_symlink():
                raise DeployError(f'Driver state root is a symlink: {path}')
            visit(path)
    for path in (root / 'usr/lib/modules').glob('*/**/vibeshine_drm.ko*'):
        visit(path)
    return files, directories


def driver_state(root=Path('/'), owner=0):
    files, directories = driver_inventory(root, owner)
    return {'files': {name: dict(fingerprint(root / name), metadata=metadata(root / name))
                      for name in sorted(files)}, 'directories': directories}


def backup_driver(directory):
    backup = directory / 'driver'
    backup.mkdir(mode=0o700)
    names, directories = driver_inventory()
    if sum((Path('/') / name).lstat().st_size for name in names) > 4 * 1024**3:
        raise DeployError('Driver rollback backup exceeds 4 GiB')
    saved = Files(backup, validator=driver_allowed).snapshot(names)
    kernels = {os.uname().release}
    kernels.update(name.split('/')[3] for name in names if name.startswith('usr/lib/modules/'))
    kernels.update(path.name for path in Path('/usr/lib/modules').iterdir()
                   if path.is_dir() and (path / 'build/Makefile').is_file())
    for kernel in kernels:
        if (not re.fullmatch(r'[A-Za-z0-9._+-]+', kernel) or
                not (Path('/usr/lib/modules') / kernel / 'build/Makefile').is_file()):
            raise DeployError(f'Matching headers required to upgrade the previously installed driver for {kernel}')
    return {'before': saved, 'directories': directories, 'started': False, 'kernels': sorted(kernels),
            'boot_id': Path('/proc/sys/kernel/random/boot_id').read_text().strip()}


def driver_transaction_preflight():
    # DKMS/package tools do not share our lock. Reject an already-active
    # transaction and require an exclusive maintenance window during deployment.
    busy = run('pgrep', '-x', 'dkms|pacman|apt|apt-get|dpkg|dnf|rpm|zypper|depmod', check=False)
    if busy.returncode == 0:
        raise DeployError('Another package/driver transaction is active; retry after it finishes')
    if busy.returncode != 1:
        raise DeployError('Could not check for concurrent package/driver transactions')
    if unit_properties('vibeshine-drm-setup.service').get('ActiveState') in ('activating', 'deactivating'):
        raise DeployError('The system driver setup job is still running')
    for path, mode in ((SIGNING_KEY, 0o600), (SIGNING_CERTIFICATE, 0o644)):
        Files(STATE).parents(path)
        try:
            info = path.lstat()
        except FileNotFoundError as error:
            raise DeployError(f'Native signing setup is missing {path}; complete the official native installer first') from error
        if not stat.S_ISREG(info.st_mode) or (info.st_uid, info.st_gid, stat.S_IMODE(info.st_mode)) != (0, 0, mode):
            raise DeployError('Set up the native module signing key before deploying; enrollment is not rollbackable')
    run('/usr/libexec/vibeshine/vibeshine-drm-install', 'signing-status')
    for path in Path('/etc/dkms').rglob('*.conf'):
        Files(STATE).parents(path)
        info = path.lstat()
        if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
            raise DeployError(f'Untrusted DKMS configuration: {path}')
        active = [line.strip() for line in path.read_text().splitlines()
                  if line.strip() and not line.lstrip().startswith('#')]
        if not dkms_config_supported(path, active):
            raise DeployError(f'Custom DKMS configuration needs manual review before transactional deployment: {path}')
    if shutil.which('weak-modules') or shutil.which('weak-modules2'):
        raise DeployError('Weak-module hooks are not covered by this local driver rollback')


def dkms_config_supported(path, active):
    # Explicit statements of the official signing defaults are not custom
    # callbacks. Parse literals only: never evaluate shell configuration here.
    if not active:
        return True
    if path != Path('/etc/dkms/framework.conf') and path.parent != Path('/etc/dkms/framework.conf.d'):
        return False
    defaults = {'mok_signing_key': str(SIGNING_KEY), 'mok_certificate': str(SIGNING_CERTIFICATE),
                'try_sign_modules': 'not_in_chroot'}
    for line in active:
        match = re.fullmatch(r'([a-z_]+)=(?:"([^"$`\\]*)"|\'([^\'$`\\]*)\'|([^\s;#$`\\]+))(?:[ \t]+#.*)?[ \t]*', line)
        if not match or match[1] not in defaults:
            return False
        value = next(value for value in match.groups()[1:] if value is not None)
        if value != defaults[match[1]]:
            return False
    return True


def restore_driver(directory, state, root=Path('/'), owner=0):
    files = Files(directory / 'driver', root=root, owner=owner, validator=driver_allowed)
    current, directories = driver_inventory(root, owner)
    # Validate backup bytes before deleting anything generated by DKMS.
    for record in state['before'].values():
        if 'backup' in record and digest(directory / 'driver/before' / record['backup']) != record['sha256']:
            raise DeployError('Corrupt driver backup')
    for name in sorted(current - state['before'].keys(), key=len, reverse=True):
        files.remove(name)
    # A DKMS symlink may replace an old directory (or the reverse).
    for name in sorted(directories, key=len, reverse=True):
        if name not in state['directories']:
            path = root / name
            if not any(path.iterdir()):
                path.rmdir()
                sync_path(path.parent, directory=True)
    files.restore(state['before'])
    for name, saved in sorted(state['directories'].items(), key=lambda item: len(item[0])):
        path = root / name
        files.parents(path / 'probe', create=True)
        os.chown(path, saved['uid'], saved['gid'])
        path.chmod(saved['mode'])
        for key in set(os.listxattr(path)) - saved['xattrs'].keys():
            os.removexattr(path, key)
        for key, value in saved['xattrs'].items():
            os.setxattr(path, key, base64.b64decode(value))
        sync_path(path, directory=True)
    affected = set(state.get('kernels', []))
    affected.update(name.split('/')[3] for name in current | state['before'].keys()
                    if name.startswith('usr/lib/modules/'))
    for kernel in sorted(affected):
        if not re.fullmatch(r'[A-Za-z0-9._+-]+', kernel):
            raise DeployError('Invalid driver rollback kernel')
        run('depmod', '-a', kernel, timeout=120)
    actual = driver_state(root, owner)
    expected = {name: dict({key: saved[key] for key in ('sha256', 'link') if key in saved},
                           metadata={key: saved[key] for key in ('uid', 'gid', 'mode', 'xattrs')})
                for name, saved in state['before'].items()}
    if actual['files'] != expected or actual['directories'] != state['directories']:
        raise DeployError('Driver rollback did not restore the exact prior state')


def driver_needs_reboot():
    loaded = Path('/sys/module/vibeshine_drm')
    if not loaded.exists():
        return False
    for field in ('version', 'srcversion'):
        result = run('modinfo', '-F', field, 'vibeshine_drm', check=False)
        value = result.stdout.strip()
        if result.returncode or not value or not (loaded / field).is_file() or (loaded / field).read_text().strip() != value:
            return True
    return False


def install_driver(directory, manifest):
    driver_transaction_preflight()
    manifest['driver']['started'] = True
    write_json(directory / 'transaction.json', manifest)
    print('Installing/upgrading DRM driver (DKMS or signed direct build); this may take several minutes.', flush=True)
    # Inherit output for progress, but never implicitly request firmware/MOK
    # changes: those are not reversible by a file transaction.
    # Reuse the official installer for every installed kernel, not just uname.
    # Only its kernel selector is overridden; all production paths stay fixed.
    wrapper = ('source /usr/libexec/vibeshine/vibeshine-drm-install || exit; '
               'readonly deploy_kernel="$1"; '
               'current_kernel_release() { printf "%s\\n" "$deploy_kernel"; }; main install')
    returncodes = []
    for kernel in manifest['driver']['kernels']:
        print(f'Building/installing Vibepollo DRM for {kernel}', flush=True)
        code = driver_command(['/usr/bin/bash', '-c', wrapper, 'vibepollo-driver-upgrade', kernel])
        returncodes.append(code)
        if code not in (0, 4):
            raise DeployError(f'Driver installation failed for {kernel} ({code}); see output above')
    manifest['driver']['after'] = driver_state()
    write_json(directory / 'transaction.json', manifest)
    helper = Path('/usr/libexec/vibeshine/vibeshine-drm-install').read_text()
    source_ids = re.findall(r'^MODULE_SOURCE_ID="([0-9a-f]{64})"$', helper, re.M)
    if len(source_ids) != 1:
        raise DeployError('Installed driver helper lacks an unambiguous source identity')
    identity = source_ids[0] + ':' + digest(SIGNING_CERTIFICATE)
    for kernel in manifest['driver']['kernels']:
        installed = run('modinfo', '-k', kernel, '-F', 'version', 'vibeshine_drm').stdout.strip()
        if installed != manifest['version'].split('-')[0]:
            raise DeployError(f'Wrong installed driver version for {kernel}: {installed}')
        markers = [Path('/var/lib/vibeshine-drm') / f'{kind}-{installed}-{kernel}' for kind in ('dkms', 'direct')]
        if not any(path.is_file() and (path.read_text().splitlines() or [''])[0] == identity for path in markers):
            raise DeployError(f'Driver source/signing identity was not updated for {kernel}')
    if 4 in returncodes or driver_needs_reboot():
        return True
    run('modprobe', 'vibeshine_drm', 'create_default_dev=0')
    return driver_needs_reboot()


def driver_command(command):
    # Do not restore module files while an interrupted DKMS/make descendant
    # is still running. A separate process group permits bounded cancellation.
    process = subprocess.Popen(command, start_new_session=True)
    def drain():
        with contextlib.suppress(ProcessLookupError):
            os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
        finally:
            with contextlib.suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGKILL)
            with contextlib.suppress(subprocess.TimeoutExpired):
                process.wait(timeout=5)
        deadline = time.monotonic() + 5
        while not driver_group_empty(process.pid) and time.monotonic() < deadline:
            time.sleep(0.1)
        if not driver_group_empty(process.pid):
            raise DriverBusy(process.pid)
    try:
        code = process.wait(timeout=900)
        # DKMS kills its progress shell but its current `sleep 3` may still
        # be exiting. Allow bounded natural completion before treating the
        # group as leaked; no later install or rollback may run meanwhile.
        deadline = time.monotonic() + 5
        while not driver_group_empty(process.pid) and time.monotonic() < deadline:
            time.sleep(0.1)
    except BaseException:
        drain()
        raise
    if not driver_group_empty(process.pid):
        drain()
        raise DeployError('Driver helper left descendants running; they were stopped before rollback')
    return code


def driver_group_empty(group):
    try:
        os.killpg(group, 0)
        return False
    except ProcessLookupError:
        return True


def snapshot_archive(source, expected, destination):
    fd = os.open(source, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, 'rb') as incoming:
        info = os.fstat(incoming.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_size > 2 * 1024**3:
            raise DeployError('Invalid deployment archive')
        with destination.open('xb') as output:
            remaining = info.st_size
            while remaining:
                chunk = incoming.read(min(1024 * 1024, remaining))
                if not chunk:
                    raise DeployError('Deployment archive was truncated during snapshot')
                output.write(chunk)
                remaining -= len(chunk)
            if incoming.read(1):
                raise DeployError('Deployment archive grew during snapshot')
            output.flush()
            os.fsync(output.fileno())
    sync_path(destination.parent, directory=True)
    if digest(destination) != expected:
        raise DeployError('Deployment archive changed; refusing installation')


def transaction_path(identifier):
    if not TRANSACTION_ID.fullmatch(identifier):
        raise DeployError('Invalid transaction ID')
    directory = STATE / identifier
    info = directory.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or stat.S_IMODE(info.st_mode) != 0o700:
        raise DeployError('Untrusted rollback directory')
    return directory


def rollback(directory, manifest):
    files = Files(directory)
    if manifest['status'] in ('ROLLED_BACK', 'ABORTED'):
        print(f'Transaction {directory.name} is already restored/aborted.')
        return
    if manifest['status'] == 'ROLLBACK_REBOOT_REQUIRED':
        raise DeployError('Previous files already restored; reboot and run vibepollo-install --part2')
    if not manifest.get('payload_mutated', True):
        quiesce()
        start_controller(manifest['controller_active'])
        manifest['status'] = 'ABORTED'
        write_json(directory / 'transaction.json', manifest)
        print('Restored prior service state; this transaction never replaced payload files.')
        return
    driver = manifest.get('driver')
    if (driver and driver.get('active_group') and
            driver['boot_id'] == Path('/proc/sys/kernel/random/boot_id').read_text().strip() and
            not driver_group_empty(driver['active_group'])):
        raise DriverBusy(driver['active_group'])
    if driver and driver.get('after') and driver_state() != driver['after']:
        raise DeployError('Driver state changed outside this transaction; refusing stale rollback')
    # An older transaction must never overwrite a newer/manual installation.
    for name, expected in manifest['after'].items():
        current = fingerprint(Path('/') / name)
        old = manifest['before'].get(name)
        old_content = ({key: old[key] for key in ('sha256', 'link') if key in old}
                       if old is not None else None)
        if current != expected and not (manifest['status'] in ('PREPARED', 'MUTATING', 'ROLLBACK_FAILED') and current == old_content):
            raise DeployError(f'Installed file changed outside this transaction: {name}')
        if current is not None:
            actual = metadata(Path('/') / name)
            if manifest['status'] in ('COMMITTED', 'UNHEALTHY', 'REBOOT_REQUIRED'):
                matches = actual == manifest['installed_metadata'][name]
            else:
                matches = current == old_content and actual == {
                    key: old[key] for key in ('uid', 'gid', 'mode', 'xattrs')}
                if not matches and current == expected:
                    intended = manifest['intended_metadata'][name]
                    without_caps = dict(actual, xattrs={key: value for key, value in actual['xattrs'].items()
                                                       if key != 'security.capability'})
                    caps = run('getcap', '/' + name).stdout.strip() if 'link' not in current else ''
                    permitted = {'', f'/{name} {CAPABILITIES[name]}'} if name in CAPABILITIES else {''}
                    matches = without_caps == intended and caps in permitted
            if not matches:
                raise DeployError(f'Installed metadata changed outside this transaction: {name}')
    try:
        quiesce()
        files.restore(manifest['before'])
        if driver and driver.get('started'):
            restore_driver(directory, driver)
            driver['after'] = driver_state()
            # A candidate loaded by a reboot cannot be replaced underneath KWin.
            if driver_needs_reboot():
                manifest['status'] = 'ROLLBACK_REBOOT_REQUIRED'
                manifest['rollback_boot_id'] = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
                driver['after'] = driver_state()
                write_json(directory / 'transaction.json', manifest)
                print(f'Previous files/driver restored. Reboot, then run:\n'
                      'vibepollo-install --part2', flush=True)
                return
        for name, record in manifest['before'].items():
            target = Path('/') / name
            expected = ({key: record[key] for key in ('sha256', 'link') if key in record}
                        if record is not None else None)
            if fingerprint(target) != expected:
                raise DeployError(f'Rollback content mismatch: {name}')
            if record is not None:
                info = target.lstat()
                if (info.st_uid, info.st_gid, stat.S_IMODE(info.st_mode)) != (record['uid'], record['gid'], record['mode']):
                    raise DeployError(f'Rollback metadata mismatch: {name}')
                if 'link' not in record:
                    attrs = {key: base64.b64encode(os.getxattr(target, key)).decode('ascii')
                             for key in os.listxattr(target)}
                    if attrs != record['xattrs']:
                        raise DeployError(f'Rollback capabilities/xattrs mismatch: {name}')
        run('udevadm', 'control', '--reload-rules')
        start_controller(manifest['controller_active'])
    except BaseException:
        manifest['status'] = 'ROLLBACK_FAILED'
        write_json(directory / 'transaction.json', manifest)
        # Never restart a possibly mixed set after a failed restore.
        with contextlib.suppress(Exception):
            quiesce()
        raise
    manifest['status'] = 'ROLLED_BACK'
    write_json(directory / 'transaction.json', manifest)
    print(f'Restored transaction {directory.name}; configuration and pairing state were not changed.')


def stop_legacy_user_hosts():
    # Run as the invoking desktop user, only after installation confirmation.
    for unit in ('sunshine.service', 'vibeshine.service', HOST):
        active = run('systemctl', '--user', 'is-active', '--quiet', unit, check=False).returncode == 0
        enabled = run('systemctl', '--user', 'is-enabled', '--quiet', unit, check=False).returncode == 0
        if active or enabled:
            run('systemctl', '--user', 'disable', '--now', unit)


def arch_package_version(version):
    if not VERSION.fullmatch(version):
        raise DeployError('Invalid local package version')
    return version.replace('-', '.') + '-1'


def package_array(recipe, name):
    # Read only literal metadata; never execute a PKGBUILD to discover it.
    match = re.search(r'^' + name + r'=\((.*?)\)', recipe, re.M | re.S)
    if not match:
        raise DeployError(f'Missing Arch package metadata: {name}')
    try:
        values = shlex.split(match[1], comments=True)
    except ValueError as error:
        raise DeployError(f'Invalid Arch package metadata: {name}') from error
    if any(not re.fullmatch(r'[A-Za-z0-9@._+:/<>=-]+', value) for value in values):
        raise DeployError(f'Nonliteral Arch package metadata: {name}')
    return values


def build_native_package(archive_path, destination, version):
    """Package the exact validated local stage with the maintained Arch hooks."""
    recipe = (REPO / 'packaging/linux/Arch/PKGBUILD').read_text()
    fields = [('pkgname', 'vibepollo'), ('pkgbase', 'vibepollo'),
              ('pkgver', arch_package_version(version)), ('pkgdesc', 'Local Vibepollo build'),
              ('arch', 'x86_64'), ('builddate', str(int(time.time())))]
    for array, field in (('depends', 'depend'), ('provides', 'provides'),
                         ('conflicts', 'conflict'), ('license', 'license')):
        fields.extend((field, value) for value in package_array(recipe, array))
    with tarfile.open(archive_path, 'r:gz') as source:
        members = inspect_archive(source, version)
        fields.append(('size', str(sum(member.size for member in members.values()))))
        with tarfile.open(destination, 'w:gz', format=tarfile.PAX_FORMAT) as package:
            metadata_files = {
                '.PKGINFO': ''.join(f'{key} = {value}\n' for key, value in fields).encode(),
                '.INSTALL': (REPO / 'packaging/linux/Arch/vibepollo.install').read_bytes(),
            }
            for name, data in metadata_files.items():
                entry = tarfile.TarInfo(name)
                entry.mode = 0o644
                entry.size = len(data)
                package.addfile(entry, io.BytesIO(data))
            # Include parents with public modes: the build runs under the user's
            # umask, which must not determine accessibility of installed assets.
            parents = {str(parent) for name in members for parent in PurePosixPath(name).parents
                       if str(parent) != '.'}
            for name in sorted(parents):
                entry = tarfile.TarInfo(name)
                entry.type, entry.mode = tarfile.DIRTYPE, 0o755
                package.addfile(entry)
            for name, member in sorted(members.items()):
                entry = tarfile.TarInfo(name)
                entry.mode = 0o777 if member.issym() else install_mode(name)
                if member.issym():
                    entry.type, entry.linkname = tarfile.SYMTYPE, member.linkname
                    package.addfile(entry)
                else:
                    entry.size = member.size
                    with source.extractfile(member) as data:
                        package.addfile(entry, data)
    return destination


def verify_package_payload(directory, manifest):
    package_path = directory / 'candidate.pkg.tar.gz'
    if digest(package_path) != manifest['sha256']:
        raise DeployError('Retained package changed; refusing package finalization')
    with tarfile.open(package_path, 'r:gz') as package:
        members = inspect_archive([member for member in package
                                   if member.name not in ('.PKGINFO', '.INSTALL')], manifest['version'])
        expected = {}
        for name, member in members.items():
            if member.issym():
                expected[name] = {'link': member.linkname}
            else:
                with package.extractfile(member) as data:
                    expected[name] = {'sha256': hashlib.file_digest(data, 'sha256').hexdigest()}
        verify_payload(directory, {'after': expected}, members)


def package_readiness(directory, manifest):
    expected = 'vibepollo ' + arch_package_version(manifest['version'])
    if run('pacman', '-Q', 'vibepollo', stderr=subprocess.PIPE).stdout.strip() != expected:
        raise DeployError('Installed package differs from this transaction; use pacman to recover')
    native_installation_preflight()
    verify_package_payload(directory, manifest)
    if driver_needs_reboot():
        manifest['status'] = 'PACKAGE_REBOOT_REQUIRED'
        write_json(directory / 'transaction.json', manifest)
        print('Package installed. Reboot, then run vibepollo-install --part2.')
        return 0
    result, detail = readiness(manifest['timeout'])
    if result not in ('healthy', 'waiting-session'):
        raise DeployError(f'Package installed but startup is not ready: {detail}; '
                          'review package setup output, then retry --part2')
    manifest['status'] = 'PACKAGE_COMMITTED'
    write_json(directory / 'transaction.json', manifest)
    print(f'Installed local package: {detail}. Original profiles are retained.')
    return 0


def root_package_install(args):
    identifier = time.strftime('%Y%m%dT%H%M%SZ', time.gmtime()) + '-' + uuid.uuid4().hex[:8]
    directory = STATE / identifier
    directory.mkdir(mode=0o700)
    package = directory / 'candidate.pkg.tar.gz'
    snapshot_archive(args.archive, args.sha256, package)
    expected = 'vibepollo ' + arch_package_version(args.version)
    if run('pacman', '-Qp', '--', package, stderr=subprocess.PIPE).stdout.strip() != expected:
        raise DeployError('Local package identity does not match the requested build')
    manifest = {'backend': 'pacman', 'status': 'PACKAGE_INSTALLING',
                'version': args.version, 'timeout': args.timeout, 'sha256': args.sha256}
    write_json(directory / 'transaction.json', manifest)
    write_json(STATE / 'latest.json', {'id': identifier})
    print(f'Root-private candidate retained at {package}.\n'
          'Pacman owns this installation; recovery uses pacman and its cached packages.\n'
          'The native installer installs dependencies and matching kernel headers, not a full system upgrade.', flush=True)
    command = ['/usr/bin/bash', str(REPO / 'scripts/linux_install.sh'), '--package', str(package)]
    if args.yes:
        command.append('--yes')
    try:
        result = subprocess.call(command)
        if result:
            raise DeployError(f'Native package installation failed ({result}); inspect pacman output. '
                              f'The candidate remains at {package}')
        manifest['status'] = 'PACKAGE_INSTALLED'
        write_json(directory / 'transaction.json', manifest)
        return package_readiness(directory, manifest)
    except BaseException:
        if manifest['status'] == 'PACKAGE_INSTALLING':
            manifest['status'] = 'PACKAGE_FAILED'
        write_json(directory / 'transaction.json', manifest)
        raise


def native_installation_preflight():
    """Check updater prerequisites without reading or changing shared state."""
    guidance = ('This local-build helper updates an already-configured native Vibepollo host; '
                'complete the native package installation and machine setup first. '
                'Use --stage-only to build without installing.')
    try:
        account = pwd.getpwnam('vibepollo')
    except KeyError as error:
        raise DeployError('Missing service account vibepollo. ' + guidance) from error
    for path, kind, uid, gid, mode in (
        (Path('/etc/vibepollo/machine.conf'), stat.S_ISREG, 0, 0, 0o600),
        (Path('/var/lib/vibepollo'), stat.S_ISDIR, account.pw_uid, account.pw_gid, 0o700),
    ):
        Files(STATE).parents(path)
        try:
            info = path.lstat()
        except FileNotFoundError as error:
            raise DeployError(f'Missing native installation prerequisite: {path}. ' + guidance) from error
        if not kind(info.st_mode) or (info.st_uid, info.st_gid, stat.S_IMODE(info.st_mode)) != (uid, gid, mode):
            raise DeployError(f'Unexpected existing native installation ownership/mode: {path}')
    return account


def root_install(args):
    account = native_installation_preflight()
    identifier = time.strftime('%Y%m%dT%H%M%SZ', time.gmtime()) + '-' + uuid.uuid4().hex[:8]
    directory = STATE / identifier
    directory.mkdir(mode=0o700)
    sync_path(STATE, directory=True)
    archive_path = directory / 'candidate.tar.gz'
    snapshot_archive(args.archive, args.sha256, archive_path)
    candidate = directory / 'candidate'
    candidate.mkdir(mode=0o700)
    with tarfile.open(archive_path, 'r:gz') as archive:
        members = inspect_archive(archive, args.version)
        for name, member in members.items():
            if member.issym():
                continue
            target = candidate / name
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.extractfile(member) as source, target.open('xb') as output:
                shutil.copyfileobj(source, output)
    if digest(candidate / f'usr/bin/vibepollo-{args.version}') != digest(candidate / 'usr/libexec/vibeshine/vibepollo-host'):
        raise DeployError('Staged public/private host binaries are from different builds')
    driver_preflight(candidate, args.version)
    driver_transaction_preflight()
    previous = {unit: unit_properties(unit) for unit in (HOST, CONTROLLER, SOCKET)}
    if any('masked' in props.get('UnitFileState', '') for props in previous.values()):
        raise DeployError('A native unit is administratively masked; refusing to override it')
    if os.path.lexists('/run/systemd/system/vibepollo-session-exec.socket'):
        raise DeployError('A runtime socket override already exists')
    baseline, reason = health()
    print(f'Pre-install health: {baseline}: {reason}', flush=True)
    files = Files(directory)
    names = set(members)
    # Remove obsolete hashed assets as part of the same exact-file journal.
    for tree in ('usr/share/vibepollo', 'usr/lib/vibepollo'):
        if any(name.startswith(tree + '/') for name in members):
            existing = Path('/') / tree
            if existing.is_symlink():
                raise DeployError(f'Unexpected symlink tree: {existing}')
            for path in existing.rglob('*'):
                files.parents(path)
                if path.is_symlink() or path.is_file():
                    names.add(str(path.relative_to('/')))
                elif not path.is_dir():
                    raise DeployError(f'Unexpected special asset: {path}')
    for path in Path('/usr/bin').glob('vibepollo-*'):
        if VERSION.fullmatch(path.name.removeprefix('vibepollo-')):
            info = path.lstat()
            if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
                raise DeployError(f'Unexpected versioned executable: {path}')
            names.add(str(path.relative_to('/')))
    manifest = {'status': 'PREPARED', 'payload_mutated': False, 'baseline': baseline, 'baseline_reason': reason,
                'version': args.version, 'controller_active': previous[CONTROLLER]['ActiveState'] == 'active',
                'units_before': previous, 'before': files.snapshot(names), 'after': {}, 'intended_metadata': {}}
    print('Backing up existing DRM sources, installed modules and DKMS state.', flush=True)
    manifest['driver'] = backup_driver(directory)
    manifest['timeout'] = args.timeout
    for name in names:
        member = members.get(name)
        manifest['after'][name] = (None if member is None else
                                   {'link': member.linkname} if member.issym() else
                                   {'sha256': digest(candidate / name)})
        if member is not None:
            manifest['intended_metadata'][name] = {
                'uid': 0, 'gid': account.pw_gid if name.endswith('/vibepollo-host') else 0,
                'mode': 0o777 if member.issym() else install_mode(name), 'xattrs': {},
            }
    write_json(directory / 'transaction.json', manifest)
    write_json(STATE / 'latest.json', {'id': identifier})
    print(f'Rollback backup: {directory}\nStopping admission and active streams.', flush=True)
    phase = 'quiescing'
    try:
        quiesce()
        driver_transaction_preflight()
        # Detect a package/manual update that raced the snapshot, before any
        # payload file is replaced. Never restore stale backups over it.
        for name, record in manifest['before'].items():
            old = ({key: record[key] for key in ('sha256', 'link') if key in record}
                   if record is not None else None)
            if fingerprint(Path('/') / name) != old:
                raise DeployError(f'Installed file changed during preflight: {name}')
            if record is not None and metadata(Path('/') / name) != {
                    key: record[key] for key in ('uid', 'gid', 'mode', 'xattrs')}:
                raise DeployError(f'Installed metadata changed during preflight: {name}')
        phase = 'mutating'
        manifest['status'] = 'MUTATING'
        manifest['payload_mutated'] = True
        write_json(directory / 'transaction.json', manifest)
        print(f'Installing {len(names)} payload files.', flush=True)
        for name in sorted(names):
            member = members.get(name)
            if member is None:
                files.remove(name)
            else:
                files.replace(name, candidate / name, link=member.linkname if member.issym() else None,
                              mode=install_mode(name, member.mode),
                              gid=account.pw_gid if name.endswith('/vibepollo-host') else 0)
        for path, caps in (
            ('/usr/libexec/vibeshine/vibepollo-host', 'cap_sys_admin,cap_sys_nice+p'),
            ('/usr/libexec/vibeshine/vibepollo-session-broker', 'cap_setgid,cap_setuid,cap_kill+p'),
        ):
            run('setcap', caps, path)
            sync_path(path)
        verify_payload(directory, manifest, members)
        manifest['installed_metadata'] = {
            name: metadata(Path('/') / name) for name in names if manifest['after'][name] is not None
        }
        manifest['created_dirs'] = files.created_dirs
        write_json(directory / 'transaction.json', manifest)
        run('udevadm', 'control', '--reload-rules')
        if install_driver(directory, manifest):
            manifest['status'] = 'REBOOT_REQUIRED'
            write_json(directory / 'transaction.json', manifest)
            run('systemctl', 'daemon-reload')
            print(f'Installed {args.version}, including the new DRM driver. KWin still holds the old module.\n'
                  'Reboot required; services remain stopped until reboot. No automatic reboot was requested.\n'
                  'After reboot: vibepollo-install --part2', flush=True)
            return 0
        phase = 'readiness'
        start_controller()
        print(f'Checking startup readiness (up to {args.timeout}s).', flush=True)
        result, detail = readiness(args.timeout)
        if result not in ('healthy', 'waiting-session'):
            raise DeployError(detail)
        manifest['status'] = 'COMMITTED'
        write_json(directory / 'transaction.json', manifest)
        print(f'Installed {args.version}: {detail}.\n'
              'These checks do not prove client video delivery or suspend/resume.\n'
              'Recovery: vibepollo-install --recover')
        return 0
    except BaseException as error:
        print(f'Deployment failed during {phase}: {error}', file=sys.stderr, flush=True)
        manifest['failure'] = str(error)
        manifest['created_dirs'] = files.created_dirs
        if isinstance(error, DriverBusy):
            manifest['driver']['active_group'] = error.group
            manifest['status'] = 'ROLLBACK_FAILED'
            write_json(directory / 'transaction.json', manifest)
            print('Admission remains closed; driver writers must stop before rollback.', file=sys.stderr)
            raise
        if manifest['driver']['started']:
            manifest['driver']['after'] = driver_state()
        write_json(directory / 'transaction.json', manifest)
        with contextlib.suppress(Exception):
            diagnostics = run('journalctl', '-u', CONTROLLER, '-u', HOST, '--since', '-3 minutes',
                              '--no-pager', '-n', '150', check=False).stdout
            (directory / 'failure.log').write_text(diagnostics)
            print(f'Diagnostics: {directory / "failure.log"}', file=sys.stderr)
        if phase in ('mutating', 'readiness'):
            retain_failed_install(directory, manifest, phase, str(error))
        else:
            # No files were replaced. In particular, do not restore a stale
            # snapshot when another package/manual update caused preflight drift.
            try:
                quiesce()
                start_controller(manifest['controller_active'])
                manifest['status'] = 'ABORTED'
            except BaseException:
                manifest['status'] = 'ROLLBACK_FAILED'
                print('Service-state recovery failed; admission remains closed.', file=sys.stderr)
                with contextlib.suppress(Exception):
                    quiesce()
                raise
            finally:
                write_json(directory / 'transaction.json', manifest)
        raise


def finalize(directory, manifest):
    status = manifest['status']
    if status in ('COMMITTED', 'ROLLED_BACK', 'ABORTED'):
        print(f'Latest transaction is already {status.lower()}.')
        return
    if status not in ('REBOOT_REQUIRED', 'ROLLBACK_REBOOT_REQUIRED', 'UNHEALTHY'):
        raise DeployError('Installation is incomplete; use vibepollo-install --recover')
    previous_boot = manifest.get('rollback_boot_id') if status == 'ROLLBACK_REBOOT_REQUIRED' else manifest['driver']['boot_id']
    if status != 'UNHEALTHY' and Path('/proc/sys/kernel/random/boot_id').read_text().strip() == previous_boot:
        raise DeployError('Reboot first; restarting the host does not replace the loaded driver')
    if os.uname().release not in manifest['driver']['kernels']:
        raise DeployError('Boot a kernel whose driver was built by this transaction')
    if driver_needs_reboot():
        raise DeployError('Loaded and installed driver version/source still differ; reboot into the updated kernel')
    if driver_state() != manifest['driver']['after']:
        raise DeployError('Driver files/state changed since this transaction; refusing stale finalization')
    if status == 'ROLLBACK_REBOOT_REQUIRED':
        for name, saved in manifest['before'].items():
            expected = ({key: saved[key] for key in ('sha256', 'link') if key in saved} if saved else None)
            if fingerprint(Path('/') / name) != expected or (saved and metadata(Path('/') / name) != {
                    key: saved[key] for key in ('uid', 'gid', 'mode', 'xattrs')}):
                raise DeployError(f'Previous payload changed before rollback finalization: {name}')
        start_controller(manifest['controller_active'])
        manifest['status'] = 'ROLLED_BACK'
    else:
        for name, expected in manifest['after'].items():
            if fingerprint(Path('/') / name) != expected:
                raise DeployError(f'Payload changed since installation: {name}')
            if expected and metadata(Path('/') / name) != manifest['installed_metadata'][name]:
                raise DeployError(f'Payload metadata changed since installation: {name}')
        run('/usr/libexec/vibeshine/vibeshine-drm-install', 'status')
        start_controller()
        result, detail = readiness(manifest['timeout'])
        if result not in ('healthy', 'waiting-session'):
            print(f'Post-reboot readiness failed: {detail}', flush=True)
            retain_failed_install(directory, manifest, 'readiness', detail)
            raise DeployError(detail)
        manifest['status'] = 'COMMITTED'
        print(detail, flush=True)
    write_json(directory / 'transaction.json', manifest)
    print(f'Transaction {directory.name}: {manifest["status"]}', flush=True)


def resume_latest(command, identifier, latest):
    if not latest:
        raise DeployError('No installation transaction is available to resume or recover')
    if identifier and identifier != latest:
        raise DeployError('Only the latest transaction can be resumed or recovered')
    directory = transaction_path(latest)
    manifest = json.loads((directory / 'transaction.json').read_text())
    if manifest.get('backend') == 'pacman':
        if command == 'rollback':
            raise DeployError('This installation is managed by pacman. Reinstall the previous package '
                              'with sudo pacman -U /path/to/previous.pkg.tar.zst; original profiles '
                              'are retained. File rollback cannot reverse package transactions.')
        return package_readiness(directory, manifest)
    if command == 'rollback' and manifest['status'] != 'ROLLBACK_REBOOT_REQUIRED':
        rollback(directory, manifest)
    else:
        finalize(directory, manifest)
    return 0


def verify_payload(directory, manifest, members):
    account = pwd.getpwnam('vibepollo')
    if os.path.samefile('/usr/bin/vibepollo', '/usr/libexec/vibeshine/vibepollo-host'):
        raise DeployError('Public and capability-bearing host must be distinct inodes')
    for name, expected in manifest['after'].items():
        target = Path('/') / name
        if fingerprint(target) != expected:
            raise DeployError(f'Installed payload mismatch: {name}')
        if expected is not None and 'link' not in expected:
            info = target.lstat()
            wanted_gid = account.pw_gid if name.endswith('/vibepollo-host') else 0
            if (info.st_uid, info.st_gid, stat.S_IMODE(info.st_mode)) != (
                    0, wanted_gid, install_mode(name, members[name].mode)):
                raise DeployError(f'Installed ownership/mode mismatch: {name}')
    cap_expectations = {
        '/usr/libexec/vibeshine/vibepollo-host': 'cap_sys_admin,cap_sys_nice=p',
        '/usr/libexec/vibeshine/vibepollo-session-broker': 'cap_kill,cap_setgid,cap_setuid=p',
    }
    for name, expected in manifest['after'].items():
        if expected is None or 'link' in expected:
            continue
        path = '/' + name
        actual = run('getcap', path).stdout.strip()
        required = f'{path} {cap_expectations[path]}' if path in cap_expectations else ''
        if actual != required:
            raise DeployError(f'Unexpected capabilities: {path}: {actual}')


def enforce_tests(build, enforce, jobs=10):
    if not enforce:
        print('Skipping tests (use --enforce to run the full suite and gate installation).', flush=True)
        return
    result = subprocess.run(['ctest', '--test-dir', str(build), '--output-on-failure', '--no-tests=error', f'-j{jobs}'])
    if result.returncode:
        raise DeployError('Enforced tests failed; installation was not started')


def read_cache(build):
    path = build / 'CMakeCache.txt'
    if not path.is_file():
        return {}
    return dict(re.findall(r'^([A-Za-z_][A-Za-z0-9_]*):[^=\r\n]+=([^\r\n]*)$', path.read_text(), re.M))


def resolve_version(explicit, cache):
    version = explicit or os.environ.get('BUILD_VERSION') or cache.get('BUILD_VERSION')
    if not version:
        tagged = run('git', '-C', REPO, 'describe', '--tags', '--exact-match', 'HEAD', check=False)
        version = tagged.stdout.strip().removeprefix('v') if tagged.returncode == 0 else ''
        if version:
            dirty = run('git', '-C', REPO, 'status', '--porcelain', '--untracked-files=normal', '--ignore-submodules=none')
            if dirty.stdout.strip():
                raise DeployError('A modified tagged checkout needs an explicit --version; refusing to label it from the tag alone')
    if not VERSION.fullmatch(version):
        raise DeployError('Supply --version with a nonzero release version (e.g. 1.19.0-beta.5). '
                          'Defaults are BUILD_VERSION, the existing build cache, then an exact HEAD tag.')
    return version


def platform_preflight(native=True):
    if sys.version_info < (3, 11) or sys.platform != 'linux':
        raise DeployError('This experimental helper requires Linux and Python 3.11 or newer')
    if os.uname().machine != 'x86_64':
        raise DeployError('Only native x86_64 Linux builds are supported by this helper; no cross-compilation')
    if not native:
        return
    if not Path('/run/systemd/system').is_dir() or not Path('/sys/fs/cgroup/cgroup.controllers').is_file():
        raise DeployError('Native deployment requires a booted systemd host with cgroup v2 (not a build container)')
    if not Path('/usr/lib/modules').is_dir() or Path('/lib/modules').resolve() != Path('/usr/lib/modules').resolve():
        raise DeployError('Native deployment requires /lib/modules to resolve to /usr/lib/modules')
    kernel = re.match(r'([0-9]+)\.([0-9]+)', os.uname().release)
    if not kernel or tuple(map(int, kernel.groups())) < (6, 16):
        raise DeployError('Managed Vibepollo displays require Linux 6.16 or newer')
    commands = ('systemctl', 'loginctl', 'journalctl', 'ss', 'setpriv', 'udevadm',
                'getcap', 'setcap', 'modinfo', 'modprobe', 'depmod', 'pgrep', 'bash', 'openssl')
    missing = [name for name in commands if not shutil.which(name, path=TRUSTED_PATH)]
    if missing:
        raise DeployError('Missing native installation tools: ' + ', '.join(missing))


def configure_command(args, build, cache):
    cc = args.cc or os.environ.get('CC') or cache.get('CMAKE_C_COMPILER')
    cxx = args.cxx or os.environ.get('CXX') or cache.get('CMAKE_CXX_COMPILER')
    for compiler in (cc, cxx, args.cuda_host_compiler):
        if compiler and not shutil.which(compiler):
            raise DeployError(f'Compiler not found: {compiler}; pass a compiler executable, not a shell command')
    root = args.cuda_root or os.environ.get('CUDAToolkit_ROOT') or os.environ.get('CUDA_PATH')
    cached_root = cache.get('CUDA_TOOLKIT_ROOT_DIR', '')
    if not root and cached_root and not cached_root.endswith('-NOTFOUND'):
        root = cached_root
    if not root:
        nvcc = cache.get('CMAKE_CUDA_COMPILER')
        if not nvcc or nvcc.endswith('-NOTFOUND'):
            nvcc = shutil.which('nvcc')
        if nvcc:
            root = str(Path(nvcc).resolve().parent.parent)
        else:
            root = next((path for path in ('/usr/local/cuda', '/opt/cuda')
                         if (Path(path) / 'bin/nvcc').is_file()), None)
    available = bool(root and (Path(root) / 'bin/nvcc').is_file())
    cached_cuda = cache.get('SUNSHINE_ENABLE_CUDA')
    cuda = (args.cuda == 'on' or (args.cuda == 'auto' and
            (cached_cuda.upper() in ('ON', 'TRUE', '1', 'YES') if cached_cuda else available)))
    if cuda and not available:
        raise DeployError('CUDA is enabled but nvcc was not found; pass --cuda-root or explicitly use --cuda off')
    command = ['cmake', '-S', str(REPO), '-B', str(build), '-G', 'Ninja',
               '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DCMAKE_INSTALL_PREFIX=/usr',
               f'-DBUILD_VERSION={args.version}', '-DBUILD_VIBESHINE_KWIN_GPU_BRIDGE=ON',
               '-DSUNSHINE_ASSETS_DIR=/usr/share/vibepollo', '-DSUNSHINE_EXECUTABLE_PATH=/usr/bin/vibepollo',
               f'-DSUNSHINE_ENABLE_CUDA={"ON" if cuda else "OFF"}', '-DSUNSHINE_ENABLE_PORTAL=ON',
               '-DBUILD_TESTS=ON', '-DBUILD_DOCS=OFF']
    for key, value in (('CMAKE_C_COMPILER', cc), ('CMAKE_CXX_COMPILER', cxx)):
        if value:
            command.append(f'-D{key}={value}')
    if cuda:
        command.append(f'-DCUDA_TOOLKIT_ROOT_DIR={Path(root).resolve()}')
        command.append(f'-DCMAKE_CUDA_COMPILER={Path(root).resolve() / "bin/nvcc"}')
        host = args.cuda_host_compiler or cache.get('CMAKE_CUDA_HOST_COMPILER') or cxx
        if host:
            command.append(f'-DCMAKE_CUDA_HOST_COMPILER={host}')
    print(f'Build settings: version={args.version}, jobs={args.jobs}, CUDA={"on" if cuda else "off"}, '
          f'C={cc or "CMake default"}, C++={cxx or "CMake default"}', flush=True)
    return command


def confirm_install():
    # GNU readline keeps editing within the answer, rather than letting the
    # terminal's erase/kill handling overwrite the printed prompt.
    import readline  # noqa: F401 -- installs Python's interactive input editor
    import termios

    if not sys.stdin.isatty():
        raise DeployError('Installation confirmation needs a terminal; use --yes for noninteractive installation')
    while True:
        # Keystrokes entered while build output was scrolling are not consent.
        termios.tcflush(sys.stdin.fileno(), termios.TCIFLUSH)
        try:
            answer = input('Install this build (including driver upgrades)? [y/N] ').strip().lower()
        except (EOFError, KeyboardInterrupt):
            print()
            return False
        if answer in ('y', 'yes'):
            return True
        if answer in ('', 'n', 'no'):
            return False
        print('Please enter y to install or n to cancel.')


def version_probe_environment(work):
    environment = dict(os.environ, XDG_CONFIG_HOME=str(work / 'version-config'),
                       VIBEPOLLO_MIGRATE_CONFIG='0')
    environment.pop('CONFIGURATION_DIRECTORY', None)
    return environment


def build_install(args):
    if os.geteuid() == 0:
        raise DeployError('Run build/install as your desktop user, without sudo; only installation elevates')
    platform_preflight(native=not args.stage_only)
    package_install = not args.stage_only and bool(shutil.which('pacman', path=TRUSTED_PATH))
    if not args.stage_only and not package_install:
        native_installation_preflight()
    if not 1 <= args.jobs <= 1024 or not 10 <= args.timeout <= 300:
        raise DeployError('Use --jobs 1..1024 and --timeout 10..300')
    if args.skip_build and any((args.cc, args.cxx, args.cuda_root, args.cuda_host_compiler, args.cuda != 'auto')):
        raise DeployError('Build-setting overrides cannot be combined with --skip-build')
    required = ['cmake', 'git'] + ([] if args.skip_build else ['ninja', 'npm'])
    if args.enforce:
        required.append('ctest')
    if not args.stage_only:
        required.append('sudo')
    missing = [name for name in required if not shutil.which(name)]
    if missing:
        raise DeployError('Missing build tools: ' + ', '.join(missing))
    if not args.stage_only and not package_install and run('systemctl', '--user', 'is-active', '--quiet', HOST, check=False).returncode == 0:
        raise DeployError('The obsolete user host is active; migrate to the native controller first')
    build = REPO / 'build'
    build.mkdir(exist_ok=True)
    with (build / '.local-deploy.lock').open('w') as lock, tempfile.TemporaryDirectory(prefix='vibepollo-local-stage-') as temporary:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        print(run('git', '-C', REPO, 'status', '--short').stdout, end='')
        cache = read_cache(build)
        args.version = resolve_version(args.version, cache)
        if not args.skip_build:
            command = configure_command(args, build, cache)
            # CMake also reads BUILD_VERSION from its environment. Do not let
            # a stale exported value silently override the explicit CLI choice.
            subprocess.run(command, env=dict(os.environ, BUILD_VERSION=args.version), check=True)
            subprocess.run(['cmake', '--build', str(build), f'-j{args.jobs}'], check=True)
        cache = read_cache(build)
        if cache.get('BUILD_VERSION') != args.version:
            raise DeployError('Build cache does not match the requested version')
        if args.enforce and cache.get('BUILD_TESTS', '').upper() not in ('ON', 'TRUE', 'YES', '1'):
            raise DeployError('--enforce requires BUILD_TESTS=ON; run without --skip-build to configure the test suite')
        work = Path(temporary)
        enforce_tests(build, args.enforce, args.jobs)
        environment = dict(os.environ, DESTDIR=str(work / 'stage'))
        subprocess.run(['cmake', '--install', str(build)], env=environment, check=True)
        version_environment = version_probe_environment(work)
        # Vibepollo parses defaults before --version. Seed only this isolated
        # probe profile from the staged payload, never an installed host's data.
        probe_profile = work / 'version-config' / 'vibepollo'
        probe_profile.mkdir(parents=True)
        shutil.copyfile(work / 'stage/usr/share/vibepollo/apps.json', probe_profile / 'apps.json')
        reported = run(work / 'stage' / f'usr/bin/vibepollo-{args.version}', '--version',
                       env=version_environment).stdout
        if not re.search(r'Vibepollo version: ' + re.escape(args.version) + r'(?:\s|$)', reported):
            raise DeployError(f'Staged executable did not report the requested version: {reported}')
        archive_path = work / 'candidate.tar.gz'
        with tarfile.open(archive_path, 'w:gz', format=tarfile.PAX_FORMAT) as archive:
            for name in ('usr', 'etc'):
                archive.add(work / 'stage' / name, arcname=name)
        with tarfile.open(archive_path, 'r:gz') as archive:
            inspect_archive(archive, args.version)
        if args.stage_only:
            destination = build / 'local-deploy-candidate.tar.gz'
            shutil.copyfile(archive_path, destination)
            print(f'Validated candidate: {destination}\nSHA256: {digest(destination)}\nNo system files or services changed.')
            return 0
        if not package_install:
            driver_preflight(work / 'stage', args.version)
        if package_install:
            package = build / f'vibepollo-{arch_package_version(args.version)}-x86_64.pkg.tar.gz'
            build_native_package(archive_path, package, args.version)
            print(f'Local package: {package}\n'
                  'Installation replaces conflicting host packages and preserves original profiles.\n'
                  'The native installer installs dependencies and matching kernel headers, not a full system upgrade.\n'
                  'Recovery uses pacman, not the file rollback journal. Streams will disconnect.')
            if not args.yes and not confirm_install():
                raise DeployError('Installation cancelled; the local package was retained')
            stop_legacy_user_hosts()
            command = ['sudo', '/usr/bin/python3', '-I', str(Path(__file__).resolve()),
                       '_package_install', str(package), digest(package), '--version', args.version,
                       '--timeout', str(args.timeout)]
            if args.yes:
                command.append('--yes')
            return subprocess.call(command)
        print('Installation will disconnect streams. Backups are root-private; configuration/pairing state is preserved.')
        if not args.yes and not confirm_install():
            raise DeployError('Installation cancelled')
        return subprocess.call(['sudo', '/usr/bin/python3', '-I', str(Path(__file__).resolve()),
                                '_install', str(archive_path), digest(archive_path), '--version', args.version,
                                '--timeout', str(args.timeout)])


def parse_arguments(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] in ('--part2', '--recover'):
        argv.insert(0, 'install')
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    install = commands.add_parser('install', help='Build, stage, back up and install (run without sudo)')
    action = install.add_mutually_exclusive_group()
    action.add_argument('--part2', action='store_true', help='Resume/validate the latest installation after reboot or a readiness failure')
    action.add_argument('--recover', action='store_true', help='Restore the previous installation from the latest transaction')
    install.add_argument('--version', help='Explicit version; otherwise BUILD_VERSION, cached version, or exact HEAD tag')
    install.add_argument('--jobs', type=int, default=min(10, os.cpu_count() or 1), help='Build/test parallelism (default: up to 10 CPUs)')
    install.add_argument('--cc', help='C compiler executable; otherwise CC, build cache, or CMake default')
    install.add_argument('--cxx', help='C++23 compiler executable; otherwise CXX, build cache, or CMake default')
    install.add_argument('--cuda', choices=('auto', 'on', 'off'), default='auto', help='Preserve cached CUDA choice, or detect toolkit on a fresh build')
    install.add_argument('--cuda-root', help='CUDA toolkit directory containing bin/nvcc')
    install.add_argument('--cuda-host-compiler', help='Host compiler executable supported by the CUDA toolkit')
    install.add_argument('--skip-build', action='store_true', help='Use existing configured artifacts')
    install.add_argument('--stage-only', action='store_true', help='Build/stage without sudo or service changes')
    install.add_argument('--enforce', action='store_true', help='Run all tests and stop installation on any failure')
    install.add_argument('--yes', action='store_true')
    internal = commands.add_parser('_install', help=argparse.SUPPRESS)
    internal.add_argument('archive', type=Path)
    internal.add_argument('sha256')
    internal.add_argument('--version', required=True)
    package_internal = commands.add_parser('_package_install', help=argparse.SUPPRESS)
    package_internal.add_argument('archive', type=Path)
    package_internal.add_argument('sha256')
    package_internal.add_argument('--version', required=True)
    package_internal.add_argument('--yes', action='store_true')
    for command in (install, internal, package_internal):
        command.add_argument('--timeout', type=int, default=90)
    undo = commands.add_parser('rollback', help='Restore the latest transaction (run without sudo)')
    undo.add_argument('transaction', nargs='?', help='Defaults to the latest transaction')
    finish = commands.add_parser('finalize', help='Validate a driver installation/rollback after reboot')
    finish.add_argument('transaction', nargs='?', help='Defaults to the latest transaction')
    args = parser.parse_args(argv)
    if args.command == 'install' and (args.part2 or args.recover):
        if len(argv) != 2:
            parser.error('--part2/--recover cannot be combined with build/install options')
        args.command = 'finalize' if args.part2 else 'rollback'
        args.transaction = None
    return args


def main():
    args = parse_arguments()
    os.environ['LC_ALL'] = 'C.UTF-8'
    platform_preflight(native=False)
    if args.command == 'install':
        return build_install(args)
    if args.command in ('rollback', 'finalize') and os.geteuid() != 0:
        print('This operation may disconnect streams while restoring/validating services.', flush=True)
        return subprocess.call(['sudo', '/usr/bin/python3', '-I', str(Path(__file__).resolve()),
                                args.command] + ([args.transaction] if args.transaction else []))
    if os.geteuid() != 0:
        raise DeployError('Internal installation phase requires root')
    # Do not let caller-provided bus addresses, systemd overrides, or loader
    # environment redirect privileged subprocesses.
    os.environ.clear()
    os.environ.update({'PATH': TRUSTED_PATH, 'LC_ALL': 'C.UTF-8'})
    platform_preflight()
    os.umask(0o077)
    Files(STATE).parents(STATE / 'lock')
    STATE.mkdir(mode=0o700, exist_ok=True)
    sync_path(STATE.parent, directory=True)
    info = STATE.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or stat.S_IMODE(info.st_mode) != 0o700:
        raise DeployError('Untrusted deployment state directory')
    with (STATE / 'lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        latest = json.loads((STATE / 'latest.json').read_text())['id'] if (STATE / 'latest.json').exists() else None
        if args.command in ('rollback', 'finalize'):
            return resume_latest(args.command, args.transaction, latest)
        if not VERSION.fullmatch(args.version) or not re.fullmatch('[0-9a-f]{64}', args.sha256):
            raise DeployError('Invalid candidate version/hash')
        if not 10 <= args.timeout <= 300:
            raise DeployError('Readiness timeout must be 10..300 seconds')
        if latest:
            manifest = json.loads((transaction_path(latest) / 'transaction.json').read_text())
            if manifest['status'] in ('PREPARED', 'MUTATING', 'ROLLBACK_FAILED', 'REBOOT_REQUIRED', 'ROLLBACK_REBOOT_REQUIRED', 'UNHEALTHY'):
                raise DeployError('Latest installation is unfinished; use vibepollo-install --part2 or --recover')
        def interrupted(signum, _frame):
            raise DeployError(f'Interrupted by signal {signum}')
        signal.signal(signal.SIGTERM, interrupted)
        signal.signal(signal.SIGINT, interrupted)
        if args.command == '_package_install':
            return root_package_install(args)
        return root_install(args)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (DeployError, OSError, subprocess.SubprocessError, ValueError) as error:
        print(f'linux-local-deploy: {error}', file=sys.stderr)
        sys.exit(1)
