#!/usr/bin/env python3
"""Stage/install a local exact-host Gamescope candidate; never restart a session."""
import argparse
import importlib.util
import json
import os
import pathlib
import platform
import shutil
import stat
import subprocess
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('gamescope_wrapper', HERE / 'gamescope-wrapper.py')
wrapper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wrapper)
DROPIN_NAME = '90-vibepollo-local-hdr.conf'
INSTALL_PARENT = pathlib.Path('/opt/vibepollo-gamescope/local')


def files(root):
    return {str(path.relative_to(root)): wrapper.digest(path)
            for path in sorted(root.rglob('*')) if path.is_file() and path.name != 'local-manifest.json'}


def prepare_install_parent(parent):
    # sudo can inherit the activation wrapper's private umask. These two
    # directories contain public runtime assets and must be traversable by the
    # desktop user, including when repairing a previous root-only installation.
    managed = (parent.parent, parent)
    for path in reversed([parent, *parent.parents]):
        if path in managed:
            path.mkdir(mode=0o755, exist_ok=True)
        info = path.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
            raise RuntimeError(f'Unsafe privileged installation parent: {path}')
        if path not in managed and not info.st_mode & 0o001:
            raise RuntimeError(f'Installation ancestor is not traversable by desktop users: {path}')
    # Only normalize the two application-owned directories, never /opt or /.
    for path in managed:
        path.chmod(0o755)


def stage(args):
    source = args.candidate.resolve()
    original = json.loads((source / 'manifest.json').read_text())
    patch_hash = original['source']['patch_sha256']
    if (not isinstance(patch_hash, str) or len(patch_hash) != 64
            or any(char not in '0123456789abcdef' for char in patch_hash)):
        raise RuntimeError('Invalid candidate patch checksum')
    if original['architecture'] != platform.machine():
        raise RuntimeError('Candidate architecture differs from the host')
    for relative, expected in original['files'].items():
        path = pathlib.PurePosixPath(relative)
        if path.is_absolute() or '..' in path.parts:
            raise RuntimeError('Invalid candidate manifest path')
        if wrapper.digest(source / relative) != expected:
            raise RuntimeError(f'Candidate checksum mismatch: {relative}')
    identity = wrapper.os_identity()
    if identity != {key: original['os'][key] for key in identity}:
        raise RuntimeError('Candidate was built for another SteamOS version')
    if args.output.exists():
        raise RuntimeError('Staging output must be a new directory')
    install_root = INSTALL_PARENT / (
        identity['BUILD_ID'] + '-' + original['source']['patch_sha256'][:12])
    out = args.output.resolve()
    out.mkdir(parents=True)
    for name in ('lib', 'share'):
        shutil.copytree(source / name, out / name)
    (out / 'bin').mkdir()
    (out / 'libexec').mkdir()
    shutil.copy2(source / 'bin/gamescope', out / 'libexec/gamescope')
    shutil.copy2(HERE / 'gamescope-wrapper.py', out / 'bin/gamescope')
    (out / 'bin/gamescope').chmod(0o755)
    session_text = pathlib.Path('/usr/lib/steamos/gamescope-session').read_text()
    stock_exec = 'exec gamescope \\\n'
    if session_text.count(stock_exec) != 1:
        raise RuntimeError('Unrecognized Valve session compositor invocation')
    (out / 'libexec/gamescope-session').write_text(session_text.replace(
        stock_exec, 'exec ' + str(install_root / 'bin/gamescope') + ' \\\n'))
    subprocess.run([str(args.patchelf.resolve()), '--set-rpath', str(install_root / 'lib'),
                    str(out / 'libexec/gamescope')], check=True)
    manifest = {
        'type': 'local-development-install', 'install_root': str(install_root),
        'os': identity, 'architecture': platform.machine(),
        'stock_gamescope_package': original['stock_gamescope_package'],
        'stock_gamescope_sha256': original['stock_gamescope_sha256'],
        'stock_session_sha256': wrapper.digest(pathlib.Path('/usr/lib/steamos/gamescope-session')),
        'source': original['source'], 'files': files(out),
    }
    (out / 'local-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    wrapper.verify(out, privileged=False)
    print(f'Staged {out}\nInstall destination: {install_root}\nNo live session or service changed.')


def install_root(args):
    if os.geteuid() != 0:
        raise RuntimeError('Root is required only to copy the reviewed candidate under /opt and retain CAP_SYS_NICE')
    source = args.stage.resolve()
    manifest = wrapper.verify(source, privileged=False)
    destination = pathlib.Path(manifest['install_root'])
    if destination.parent != INSTALL_PARENT:
        raise RuntimeError('Unexpected install destination')
    if any(path.is_symlink() for path in source.rglob('*')):
        raise RuntimeError('Symlinks are not accepted in the privileged candidate')
    prepare_install_parent(destination.parent)
    if destination.exists():
        if wrapper.verify(destination) != manifest:
            raise RuntimeError('Another candidate already occupies this destination; retain it and stage a new local revision')
        print(f'Already installed: {destination}')
        return
    temporary = pathlib.Path(tempfile.mkdtemp(prefix='.install-', dir=destination.parent))
    try:
        # Preserve the private 0700 temporary root until the copied contents
        # match the manifest read before copying user-owned staging files.
        for path in source.iterdir():
            if path.is_dir() and not path.is_symlink():
                shutil.copytree(path, temporary / path.name, symlinks=True)
            else:
                shutil.copy2(path, temporary / path.name, follow_symlinks=False)
        copied_manifest = wrapper.verify(temporary, privileged=False)
        if copied_manifest != manifest:
            raise RuntimeError('Staged manifest changed while copying')
        for path in [temporary, *temporary.rglob('*')]:
            os.chown(path, 0, 0)
            path.chmod(0o755 if path.is_dir() or path.stat().st_mode & 0o111 else 0o644)
        wrapper.verify(temporary, privileged=False)
        subprocess.run(['/usr/bin/setcap', 'cap_sys_nice=eip', str(temporary / 'libexec/gamescope')], check=True)
        temporary.rename(destination)
        wrapper.verify(destination)
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
    print(f'Installed {destination}; no live session or service changed.')


def activate_user(args):
    if os.geteuid() == 0:
        raise RuntimeError('Run user activation as the logged-in SteamOS user')
    root = args.installed.resolve()
    wrapper.verify(root)
    if str(root) != json.loads((root / 'local-manifest.json').read_text())['install_root']:
        raise RuntimeError('Installed path differs from its compiled library search path')
    config = pathlib.Path(os.environ.get('XDG_CONFIG_HOME', pathlib.Path.home() / '.config'))
    dropin = config / 'systemd/user/gamescope-session.service.d' / DROPIN_NAME
    contents = ('# Local Vibepollo HDR compositor; effective on the next Gaming Mode entry.\n'
                '[Service]\nExecStart=\nExecStart=' + str(root / 'bin/gamescope') + ' --vibepollo-session\n')
    if dropin.exists() and dropin.read_text() != contents:
        raise RuntimeError(f'Refusing to overwrite a different existing drop-in: {dropin}')
    dropin.parent.mkdir(parents=True, exist_ok=True)
    previous = dropin.read_bytes() if dropin.exists() else None
    fd, name = tempfile.mkstemp(prefix='.vibepollo-', dir=dropin.parent)
    temporary = pathlib.Path(name)
    try:
        with os.fdopen(fd, 'w') as stream:
            stream.write(contents)
        temporary.chmod(0o644)
        temporary.replace(dropin)
        try:
            subprocess.run(['systemctl', '--user', 'daemon-reload'], check=True)
        except subprocess.SubprocessError:
            if previous is None:
                dropin.unlink()
            else:
                dropin.write_bytes(previous)
            subprocess.run(['systemctl', '--user', 'daemon-reload'], check=False)
            raise
    finally:
        temporary.unlink(missing_ok=True)
    print(f'Enabled for next Gaming Mode entry: {dropin}\nNo session was restarted.')


def rollback_user(args):
    if os.geteuid() == 0:
        raise RuntimeError('Run user rollback as the logged-in SteamOS user')
    config = pathlib.Path(os.environ.get('XDG_CONFIG_HOME', pathlib.Path.home() / '.config'))
    dropin = config / 'systemd/user/gamescope-session.service.d' / DROPIN_NAME
    if dropin.exists():
        if not dropin.read_text().startswith('# Local Vibepollo HDR compositor;'):
            raise RuntimeError(f'Refusing to remove an unrecognized drop-in: {dropin}')
        dropin.unlink()
    subprocess.run(['systemctl', '--user', 'daemon-reload'], check=True)
    print('Valve Gamescope selected for the next Gaming Mode entry. Installed files retained for rollback.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(required=True)
    command = commands.add_parser('stage')
    command.add_argument('--candidate', type=pathlib.Path, required=True)
    command.add_argument('--output', type=pathlib.Path, required=True)
    command.add_argument('--patchelf', type=pathlib.Path, required=True)
    command.set_defaults(run=stage)
    command = commands.add_parser('install-root')
    command.add_argument('--stage', type=pathlib.Path, required=True)
    command.set_defaults(run=install_root)
    command = commands.add_parser('activate-user')
    command.add_argument('--installed', type=pathlib.Path, required=True)
    command.set_defaults(run=activate_user)
    command = commands.add_parser('rollback-user')
    command.set_defaults(run=rollback_user)
    args = parser.parse_args()
    args.run(args)


if __name__ == '__main__':
    main()
