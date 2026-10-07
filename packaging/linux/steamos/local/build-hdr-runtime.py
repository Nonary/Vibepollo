#!/usr/bin/env python3
"""Build a private Mesa VAAPI encoder for the local SteamOS 3.8.24 host ABI."""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import tarfile
import urllib.request

MESA_VERSION = '26.1.7'
MESA_SHA256 = '25e0a669e6638c3563e7be32a0a09f1888317e6eed0d047dc41d49dc8de26c7d'
LIBVA_SHA256 = '467c418c2640a178c6baad5be2e00d569842123763b80507721ab87eb7af8735'


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def archive(work, name, url, expected):
    path = work / name
    if not path.exists():
        urllib.request.urlretrieve(url, path)
    if sha(path) != expected:
        raise RuntimeError(f'Source checksum mismatch: {path}')
    with tarfile.open(path) as source:
        source.extractall(work, filter='data')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    parser.add_argument('--container', default='moonlight-dev')
    parser.add_argument('--mesa-source', type=pathlib.Path, help='Use an already extracted verified source archive')
    parser.add_argument('--package-only', action='store_true', help='Package an already completed build in output/mesa-build')
    parser.add_argument('--jobs', type=int, default=2)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    work = args.output.resolve()
    work.mkdir(parents=True, exist_ok=True)
    venv = work / 'build-venv'
    build = work / 'mesa-build'
    runtime = work / 'runtime'

    def container(command, log=None):
        prefix = ['podman', 'exec', '--user', str(os.getuid()), '--env',
                  'PATH=' + str(venv / 'bin') + ':/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin',
                  args.container, 'nice', '-n', '15']
        if log:
            with (work / log).open('w') as stream:
                subprocess.run(prefix + list(map(str, command)), stdout=stream, stderr=subprocess.STDOUT, check=True)
        else:
            subprocess.run(prefix + list(map(str, command)), check=True)

    if not args.package_only:
        if subprocess.check_output(['pacman', '-Q', 'libva'], text=True).strip() != 'libva 2.22.0-1':
            raise RuntimeError('This local build recipe requires the audited SteamOS libva 2.22.0-1 ABI')
        if args.mesa_source:
            source = args.mesa_source.resolve()
            packed = source.parent / 'mesa-26.1.7.tar.xz'
            if sha(packed) != MESA_SHA256:
                raise RuntimeError('The supplied Mesa source archive checksum does not match')
            # Re-extract the verified archive; do not trust mutable extracted source.
            with tarfile.open(packed) as tree:
                tree.extractall(source.parent, filter='data')
        else:
            archive(work, 'mesa-26.1.7.tar.xz', 'https://archive.mesa3d.org/mesa-26.1.7.tar.xz', MESA_SHA256)
            source = work / 'mesa-26.1.7'
        archive(work, 'libva-2.22.0.tar.gz',
                'https://github.com/intel/libva/archive/refs/tags/2.22.0.tar.gz', LIBVA_SHA256)
        headers = work / 'libva-headers/include/va'
        headers.mkdir(parents=True, exist_ok=True)
        libva_source = work / 'libva-2.22.0/va'
        for header in libva_source.glob('*.h'):
            shutil.copy2(header, headers / header.name)
        version = (libva_source / 'va_version.h.in').read_text()
        for key, value in [('MAJOR_VERSION', '1'), ('MINOR_VERSION', '22'),
                           ('MICRO_VERSION', '0'), ('VERSION', '1.22.0')]:
            version = version.replace('@VA_API_' + key + '@', value)
        (headers / 'va_version.h').write_text(version)
        pkg = work / 'libva-headers/lib/pkgconfig'
        pkg.mkdir(parents=True, exist_ok=True)
        (pkg / 'libva.pc').write_text('Name: libva\nDescription: SteamOS 2.22 headers for private VA driver\n'
                                    'Version: 1.22.0\nCflags: -I' + str(headers.parent) +
                                    '\nLibs: -L/run/host/usr/lib -lva\n')
        if not (venv / 'bin/python3').exists():
            container(['python3', '-m', 'venv', venv])
        container([venv / 'bin/pip', 'install', 'mako==1.4.1', 'PyYAML==6.0.3',
                   'packaging==26.3', 'patchelf==0.19.1.0'], 'python-dependencies.log')
        link = ('-Wl,-rpath-link,/run/host/usr/lib -Wl,--push-state,--no-as-needed '
                '/run/host/usr/lib/libm.so.6 /run/host/usr/lib/libmvec.so.1 -Wl,--pop-state')
        options = ['--prefix=/', '--libdir=lib', '--buildtype=release', '-Dgallium-drivers=radeonsi',
                   '-Dvulkan-drivers=', '-Dplatforms=', '-Dglx=disabled', '-Degl=disabled',
                   '-Dgbm=disabled', '-Dopengl=false', '-Dgles1=disabled', '-Dgles2=disabled',
                   '-Dllvm=disabled', '-Damd-use-llvm=false', '-Dgallium-rusticl=false',
                   '-Dgallium-va=enabled', '-Dvideo-codecs=h264enc,h265enc', '-Dbuild-tests=false',
                   '-Dtools=', '-Dspirv-tools=disabled', '-Dlibunwind=disabled',
                   '-Dlmsensors=disabled', '-Ddisplay-info=disabled', '-Dpkg_config_path=' + str(pkg),
                   '-Dc_args=-mtls-dialect=gnu', '-Dcpp_args=-mtls-dialect=gnu',
                   '-Dc_link_args=' + link, '-Dcpp_link_args=' + link]
        reconfigure = ['--clearcache', '--reconfigure'] if (build / 'build.ninja').exists() else []
        container(['meson', 'setup', *reconfigure, build, source, *options], 'mesa-configure.log')
        container(['ninja', '-C', build, '-j', args.jobs], 'mesa-build.log')

    if runtime.exists():
        raise RuntimeError('Refusing to replace an existing private runtime; use a fresh output for each release')
    driver_dir = runtime / 'lib/dri'
    driver_dir.mkdir(parents=True)
    driver = driver_dir / 'radeonsi_drv_video.so'
    shutil.copy2(build / 'src/gallium/targets/va/libgallium_drv_video.so', driver)
    # This reduced VA frontend needs only compiler and system-library symbols
    # already present on SteamOS. Do not copy the container graphics stack or
    # replace libraries already loaded by the application.
    result = subprocess.run(['ldd', '-r', str(driver)], capture_output=True, text=True, check=True)
    (work / 'native-runtime.log').write_text(result.stdout + result.stderr)
    if 'not found' in result.stdout + result.stderr or 'undefined symbol' in result.stdout + result.stderr:
        raise RuntimeError('Private encoder has unresolved native symbols; see native-runtime.log')
    symbols = subprocess.check_output(['nm', '-D', str(driver)], text=True)
    if '__vaDriverInit_1_22' not in symbols:
        raise RuntimeError('Private driver was not built for host VAAPI 1.22')
    (runtime / 'env.conf').write_text('LIBVA_DRIVERS_PATH=' + str(driver_dir) + '\n'
                                      'LIBVA_DRIVER_NAME=radeonsi\nVIBEPOLLO_PRIVATE_VAAPI=1\n')
    license_dir = runtime / 'licenses'
    license_dir.mkdir()
    mesa_source = args.mesa_source.resolve() if args.mesa_source else work / 'mesa-26.1.7'
    shutil.copy2(mesa_source / 'docs/license.rst', license_dir / 'Mesa.txt')
    manifest = {'type': 'local-private-vaapi-runtime', 'mesa': MESA_VERSION,
                'mesa_source_sha256': MESA_SHA256, 'libva_headers_sha256': LIBVA_SHA256,
                'libva_api': '1.22', 'driver_scope': 'VAAPI h264/hevc encode only',
                'runtime_dependencies': 'all supplied by SteamOS or the application bundle',
                'host_packages': subprocess.check_output(['pacman', '-Q', 'glibc', 'libva', 'mesa'], text=True).splitlines(),
                'files': {str(path.relative_to(runtime)): sha(path)
                          for path in sorted(runtime.rglob('*')) if path.is_file()}}
    (runtime / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Private native runtime staged: {runtime}\nNo host drivers or services changed.')


if __name__ == '__main__':
    main()
