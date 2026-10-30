#!/usr/bin/env python3
"""Build kernel-specific inspection RPMs locally. Does not install or enroll."""
import base64
import fcntl
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

KERNEL = '7.2.11-ogc4.1.fc44.x86_64'
IMAGE = 'localhost/vibepollo-bazzite-builder:f44-gcc14-v1'
NAMES = ('vibeshine_drm.ko', 'vibeshine_ds5.ko')
PINNED = {
    'third-party/libvirtualdisplay': 'b2946d02a09bc453d9e6d287623390ca1dedf964',
    'third-party/libvirtualgamepad': '7b000a2b060b4daeb0b5c6aa7b77ae1d81857e7a',
}


def run(*args, **kwargs):
    print('+ ' + ' '.join(map(str, args)), flush=True)
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def capture(*args):
    return subprocess.check_output(list(map(str, args)), text=True).strip()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def copy_tracked(repo, relative, destination):
    names = subprocess.check_output(['git', '-C', str(repo), 'ls-files', '-z', '--', relative])
    count = 0
    for raw in names.split(b'\0'):
        if not raw:
            continue
        name = os.fsdecode(raw)
        source = repo / name
        tail = Path(name).relative_to(relative)
        target = destination / tail
        if source.is_symlink() or not source.is_file():
            raise RuntimeError(f'Unexpected source file: {source}')
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        count += 1
    if not count:
        raise RuntimeError('No tracked driver sources found')


def kernel_dependency(kernel):
    """Use RPM ownership of actual kernel files, without assuming a virtual provide."""
    root = Path('/usr/lib/modules') / kernel
    candidates = [root / name for name in ('vmlinuz', 'config', 'modules.builtin')]
    # Some image builds remove vmlinuz from the RPM-owned modules tree.
    # A kernel module still establishes an exact dependency on this kernel's
    # installed package. Never substitute the development headers package.
    module_tree = root / 'kernel'
    if module_tree.is_dir():
        for path in module_tree.rglob('*.ko*'):
            if path.is_file():
                candidates.append(path)
                if len(candidates) >= 8:
                    break
    for path in candidates:
        if not path.is_file():
            continue
        query = subprocess.run(['rpm', '-qf', '--qf',
                                '%{NAME} = %{EPOCHNUM}:%{VERSION}-%{RELEASE}\n',
                                str(path)], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if query.returncode:
            continue
        dependencies = sorted(set(query.stdout.splitlines()))
        if len(dependencies) != 1:
            continue
        dependency = dependencies[0].replace(' = 0:', ' = ', 1)
        if not re.fullmatch(r'[A-Za-z0-9_+.-]+ = (?:[0-9]+:)?[A-Za-z0-9_+.~^-]+', dependency):
            raise RuntimeError(f'Unexpected kernel package metadata: {dependency!r}')
        if dependency.startswith(('kernel-devel', 'kernel-headers')):
            continue
        # rpm -qf above already establishes the installed owner and version.
        print(f'Kernel package dependency: {dependency}\nOwned kernel file: {path}', flush=True)
        return dependency
    raise RuntimeError(f'Could not identify the RPM owning kernel {kernel}; '
                       'send this log so we can inspect the image package layout')


def make_spec(kernel, files, dependency):
    # Prevent RPM stripping or compressing signed modules after signing.
    header = '''%global debug_package %{nil}
%global __os_install_post %{nil}
%global _build_id_links none
Name: vibepollo-kmod
Version: 2.0.2
Release: 2.bazzite
Summary: Locally signed Vibepollo drivers for one Bazzite kernel
License: GPL-2.0-only
BuildArch: x86_64
AutoReqProv: no
'''
    return header + f'''Requires: {dependency}
Requires: kmod
Requires: python3
%description
Experimental, locally built drivers for Bazzite kernel {kernel}.
%prep
%build
%install
mkdir -p %{{buildroot}}
cp -a %{{_sourcedir}}/payload/. %{{buildroot}}/
%files
''' + ''.join(f'%attr({mode},root,root) {name}\n' for name, mode in files) + f'''
%posttrans
/usr/sbin/depmod -a {kernel}

%postun
if [ -d /usr/lib/modules/{kernel} ]; then
    /usr/sbin/depmod -a {kernel}
fi
'''


def main():
    if os.geteuid() == 0:
        raise RuntimeError('Run this script without sudo; only signing asks for sudo')
    if os.uname().release != KERNEL:
        raise RuntimeError(f'This first package targets {KERNEL}; your kernel has changed')
    for command in ('git', 'make', 'gcc', 'podman', 'rpm', 'sudo', 'openssl', 'modinfo'):
        if shutil.which(command) is None:
            raise RuntimeError(f'Missing host tool: {command}')
    kit = Path(__file__).resolve().parent
    state = Path(os.environ.get('XDG_DATA_HOME', str(Path.home() / '.local/share'))) / 'vibepollo-bazzite'
    source = kit.parents[3]
    state.mkdir(parents=True, exist_ok=True)
    lock = (state / 'build.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    source_revision = capture('git', '-C', source, 'rev-parse', 'HEAD')
    run('git', '-C', source, 'diff', '--exit-code', 'HEAD', '--', 'packaging/linux/bazzite/drivers')
    for relative, revision in PINNED.items():
        repo = source / relative
        if capture('git', '-C', repo, 'rev-parse', 'HEAD') != revision:
            raise RuntimeError(f'Unexpected submodule: {relative}')
        run('git', '-C', repo, 'diff', '--exit-code', 'HEAD', '--', 'linux')
    dependency = kernel_dependency(KERNEL)
    headers = Path('/usr/src/kernels') / KERNEL
    if not (headers / 'Makefile').is_file():
        raise RuntimeError(f'Matching kernel development files missing: {headers}')
    run('podman', 'image', 'exists', IMAGE)
    output = state / ('drivers-' + time.strftime('%Y%m%d-%H%M%S'))
    output.mkdir(mode=0o700)
    print(f'Output: {output}', flush=True)
    display = output / 'work/display'
    gamepad = output / 'work/gamepad'
    copy_tracked(source / 'third-party/libvirtualdisplay', 'linux/vibeshine-drm', display)
    copy_tracked(source / 'third-party/libvirtualgamepad', 'linux/vibeshine-ds5', gamepad)
    (display / 'vibeshine_drm_version.h').write_text('#define VIBESHINE_DRM_VERSION "2.0.2"\n')
    jobs = str(min(8, os.cpu_count() or 1))
    # The shared build helper selects GCC or Clang to match the host kernel.
    for directory in (display, gamepad):
        run('bash', directory / 'build-module', KERNEL, directory, '-j' + jobs)
    for name, directory in zip(NAMES, (display, gamepad)):
        module = directory / name
        if capture('modinfo', '-F', 'vermagic', module).split()[0] != KERNEL:
            raise RuntimeError(f'Wrong module vermagic: {name}')
        if name == 'vibeshine_drm.ko' and not capture('modinfo', '-F', 'srcversion', module):
            raise RuntimeError(f'Missing module source identity: {name}')
    request = {name: base64.b64encode((directory / name).read_bytes()).decode()
               for name, directory in zip(NAMES, (display, gamepad))}
    print('Signing needs sudo. The private key stays in /var/lib/vibepollo-module-signing.', flush=True)
    result = subprocess.run(['sudo', '/usr/bin/python3', '-I', '-c',
                             (kit / 'sign-modules.py').read_text()],
                            input=json.dumps(request), text=True, stdout=subprocess.PIPE, check=True)
    signed = json.loads(result.stdout)
    payload = output / 'rpm/SOURCES/payload'
    files = []

    def put(relative, data, mode=0o644):
        path = payload / relative.lstrip('/')
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(mode)
        files.append((relative, f'{mode:o}'))
        return path

    hashes = {}
    for name in NAMES:
        data = base64.b64decode(signed['modules'][name], validate=True)
        path = put(f'/usr/lib/modules/{KERNEL}/extra/vibepollo/{name}', data)
        if capture('modinfo', '-F', 'signer', path) != 'Vibepollo local module signing':
            raise RuntimeError(f'Signature absent: {name}')
        hashes[name] = digest(path)
    certificate = base64.b64decode(signed['certificate'], validate=True)
    if hashlib.sha256(certificate).hexdigest() != signed['certificate_sha256']:
        raise RuntimeError('Certificate checksum mismatch')
    (output / 'MOK.der').write_bytes(certificate)
    put('/usr/share/vibepollo/module-signing/MOK.der', certificate)
    put('/usr/libexec/vibeshine/vibepollo-bazzite-driver-check', (kit / 'driver-check.py').read_bytes(), 0o755)
    put('/usr/lib/systemd/system/vibeshine-drm-setup.service.d/50-bazzite.conf', b'''[Service]
ExecStart=
ExecStart=/usr/libexec/vibeshine/vibepollo-bazzite-driver-check --load
SuccessExitStatus=
SuccessExitStatus=0
''')
    put('/usr/lib/systemd/system/vibeshine-vkms.service.d/50-bazzite.conf', b'''[Service]
ExecStartPre=
ExecStartPre=/usr/libexec/vibeshine/vibepollo-bazzite-driver-check
''')
    rpmroot = output / 'rpm'
    for name in ('BUILD', 'BUILDROOT', 'RPMS', 'SPECS', 'SRPMS'):
        (rpmroot / name).mkdir()
    (rpmroot / 'SPECS/vibepollo-kmod.spec').write_text(make_spec(KERNEL, files, dependency))
    run('podman', 'run', '--rm', '--userns=keep-id', '--user', f'{os.getuid()}:{os.getgid()}',
        '--env', 'HOME=/tmp', '--volume', f'{rpmroot}:/rpmbuild:Z', IMAGE,
        'bash', '-euo', 'pipefail', '-c', '''
rpmbuild -bb --define '_topdir /rpmbuild' /rpmbuild/SPECS/vibepollo-kmod.spec
packages=(/rpmbuild/RPMS/x86_64/*.rpm)
test "${#packages[@]}" = 1
rpm -qp --scripts "${packages[0]}" > /rpmbuild/scriptlets.txt
grep -F "/usr/sbin/depmod -a 7.2.11-ogc4.1.fc44.x86_64" /rpmbuild/scriptlets.txt
rpm -qp --requires "${packages[0]}" > /rpmbuild/requires.txt
mkdir /rpmbuild/extracted
bsdtar -xf "${packages[0]}" -C /rpmbuild/extracted
''')
    packages = list((rpmroot / 'RPMS/x86_64').glob('*.rpm'))
    if len(packages) != 1:
        raise RuntimeError('RPM metadata verification failed')
    requires = (rpmroot / 'requires.txt').read_text()
    if dependency not in requires.splitlines():
        raise RuntimeError('RPM lost exact kernel dependency')
    for name in NAMES:
        extracted = rpmroot / 'extracted/usr/lib/modules' / KERNEL / 'extra/vibepollo' / name
        if digest(extracted) != hashes[name]:
            raise RuntimeError(f'Packaging changed signed module: {name}')
    package = output / packages[0].name
    shutil.copyfile(packages[0], package)
    report = {'kernel': KERNEL, 'kernel_package_dependency': dependency,
              'revision': source_revision, 'modules_sha256': hashes,
              'certificate_sha256': signed['certificate_sha256'], 'rpm_sha256': digest(package),
              'package': package.name, 'status': 'Built and inspected; not installed or enrolled'}
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2), flush=True)
    print(f'FINISHED. Report: {output / "report.json"}')
    print(f'RPM: {package}\nPublic certificate: {output / "MOK.der"}')
    print('Nothing was installed, enrolled, loaded, or rebooted. Keep your existing app RPM.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError, ValueError, KeyError) as error:
        print(f'Driver build stopped: {error}', file=sys.stderr)
        sys.exit(1)
