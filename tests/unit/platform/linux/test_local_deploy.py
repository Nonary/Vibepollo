#!/usr/bin/env python3
"""Native installer tests: no system services or real installation paths touched."""

import importlib.util
import io
import json
import os
from pathlib import Path
import socket
import stat
import tarfile
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[4]
SPEC = importlib.util.spec_from_file_location('local_deploy', ROOT / 'scripts/linux_local_deploy.py')
deploy = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(deploy)
VERSION = '1.19.0-beta.5'


def package(extra=(), omit=()):
    stream = io.BytesIO()
    names = deploy.FIXED | {
        f'usr/bin/vibepollo-{VERSION}', 'usr/share/vibepollo/web/index.html',
        'usr/share/vibepollo/web/v2/index.html',
        'usr/src/vibeshine-drm-1.19.0/vibeshine_drm_version.h',
    }
    names.update('usr/src/vibeshine-drm-1.19.0/' + name for name in
                 ('Makefile', 'build-module', 'dkms.conf', 'vkms_drv.c',
                  'vibeshine_drm_uapi.h', 'vibeshine_drm_vrr.h'))
    with tarfile.open(fileobj=stream, mode='w:gz') as archive:
        for name in sorted(names - set(omit)):
            entry = tarfile.TarInfo(name)
            if name == 'usr/bin/vibepollo':
                entry.type = tarfile.SYMTYPE
                entry.linkname = f'vibepollo-{VERSION}'
                archive.addfile(entry)
            else:
                entry.size = 4
                archive.addfile(entry, io.BytesIO(b'test'))
        for entry in extra:
            archive.addfile(entry, io.BytesIO(b'\0' * entry.size))
    stream.seek(0)
    return tarfile.open(fileobj=stream, mode='r:gz')


class ArchiveTests(unittest.TestCase):
    def test_local_package_must_include_driver_helper_and_build_sources(self):
        for missing in ('usr/libexec/vibeshine/vibeshine-drm-install',
                        'usr/src/vibeshine-drm-1.19.0/Makefile',
                        'usr/src/vibeshine-drm-1.19.0/build-module',
                        'usr/src/vibeshine-drm-1.19.0/vibeshine_drm_vrr.h'):
            with self.subTest(missing=missing), package(omit=[missing]) as archive:
                with self.assertRaisesRegex(deploy.DeployError, 'Missing artifacts'):
                    deploy.inspect_archive(archive, VERSION)

    def test_release_version_allowlist(self):
        for version in ('1.19.0', '1.19.0-stable.1', '1.19.0-alpha.2', VERSION, '1.19.0-rc.1'):
            self.assertIsNotNone(deploy.VERSION.fullmatch(version))
        for version in ('0.0.0', '../1.19.0', '1.19.0;id', '1.19.0-unknown.1'):
            self.assertIsNone(deploy.VERSION.fullmatch(version))

    def test_complete_native_payload(self):
        with package() as archive:
            members = deploy.inspect_archive(archive, VERSION)
        self.assertIn('usr/libexec/vibeshine/vibepollo-display-power', members)

    def test_missing_recovery_helper_fails_preflight(self):
        with package(omit=['usr/libexec/vibeshine/vibepollo-display-power']) as archive:
            with self.assertRaises(deploy.DeployError):
                deploy.inspect_archive(archive, VERSION)

    def test_native_apollo_icons_with_or_without_tray(self):
        with package() as archive:
            self.assertIn('usr/share/icons/hicolor/scalable/apps/apollo.svg',
                          deploy.inspect_archive(archive, VERSION))
        with package([tarfile.TarInfo(name) for name in deploy.OPTIONAL]) as archive:
            self.assertTrue(deploy.OPTIONAL <= deploy.inspect_archive(archive, VERSION).keys())
        self.assertFalse(deploy.allowed('usr/share/icons/hicolor/scalable/apps/unrelated.svg'))

    def test_paths_special_files_links_duplicates_and_symlink_parents(self):
        entries = []
        for name in ('/etc/shadow', '../escape', 'usr/../escape', 'usr//bin/x',
                     'etc/vibepollo/machine.conf', 'usr/bin/bash', 'usr/share/vibepollo/../bad',
                     'usr/bin/vibepollo-1.19.0-beta.4',
                     'usr/bin/vibepollo-mangohud'):
            entries.append(tarfile.TarInfo(name))
        for kind in (tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.FIFOTYPE, tarfile.CHRTYPE):
            entry = tarfile.TarInfo('usr/share/vibepollo/bad')
            entry.type = kind
            entry.linkname = '/etc'
            entries.append(entry)
        entries.append(tarfile.TarInfo('usr/bin/vibepollo/escape'))
        for entry in entries:
            with self.subTest(name=entry.name, kind=entry.type), package([entry]) as archive:
                with self.assertRaises(deploy.DeployError):
                    deploy.inspect_archive(archive, VERSION)

    def test_file_modes_do_not_trust_archive_privilege_bits(self):
        self.assertEqual(deploy.install_mode('usr/share/vibepollo/web/index.html', 0o7777), 0o644)
        self.assertEqual(deploy.install_mode('usr/libexec/vibeshine/vibepollo-display-power', 0o4777), 0o755)
        self.assertEqual(deploy.install_mode('usr/libexec/vibeshine/vibepollo-host', 0o7777), 0o750)
        self.assertEqual(deploy.install_mode('usr/libexec/vibeshine/vibepollo-session-broker', 0o7777), 0o700)
        self.assertEqual(deploy.install_mode('usr/lib/libvibeshine-kwin-gpu.so'), 0o4755)


class SharedBuildTests(unittest.TestCase):
    def test_version_probe_cannot_use_or_migrate_the_installed_profile(self):
        with mock.patch.dict(os.environ, {'CONFIGURATION_DIRECTORY': '/var/lib',
                                          'VIBEPOLLO_MIGRATE_CONFIG': '1'}):
            environment = deploy.version_probe_environment(Path('/tmp/probe'))
        self.assertNotIn('CONFIGURATION_DIRECTORY', environment)
        self.assertEqual(environment['XDG_CONFIG_HOME'], '/tmp/probe/version-config')
        self.assertEqual(environment['VIBEPOLLO_MIGRATE_CONFIG'], '0')

    def test_resume_flags_select_actions_without_transaction_ids(self):
        for prefix in ([], ['install']):
            for flag, command in [('--part2', 'finalize'), ('--recover', 'rollback')]:
                args = deploy.parse_arguments(prefix + [flag])
                self.assertEqual(args.command, command)
                self.assertIsNone(args.transaction)
        for command in ('finalize', 'rollback'):
            self.assertIsNone(deploy.parse_arguments([command]).transaction)

    def test_resume_flags_reject_conflicting_or_ignored_options(self):
        for args in (['install', '--part2', '--recover'],
                     ['install', '--part2', '--version', VERSION],
                     ['install', '--rollback', 'always']):
            with mock.patch.object(deploy.sys, 'stderr', io.StringIO()), self.assertRaises(SystemExit):
                deploy.parse_arguments(args)

    def test_confirmation_discards_typeahead_and_reprompts_for_junk(self):
        with mock.patch.object(deploy.sys.stdin, 'isatty', return_value=True), \
                mock.patch.object(deploy.sys.stdin, 'fileno', return_value=99), \
                mock.patch('termios.tcflush') as flush, \
                mock.patch('builtins.input', side_effect=['top', '\x1b[Z', 'YES']):
            self.assertTrue(deploy.confirm_install())
        self.assertEqual(flush.call_count, 3)

    def test_confirmation_decline_and_eof_cancel(self):
        for value in ('', 'n', 'no', EOFError()):
            with mock.patch.object(deploy.sys.stdin, 'isatty', return_value=True), \
                    mock.patch.object(deploy.sys.stdin, 'fileno', return_value=99), \
                    mock.patch('termios.tcflush'), mock.patch('builtins.input', side_effect=[value]):
                self.assertFalse(deploy.confirm_install())

    def test_confirmation_rejects_noninteractive_stdin(self):
        with mock.patch.object(deploy.sys.stdin, 'isatty', return_value=False):
            with self.assertRaisesRegex(deploy.DeployError, 'needs a terminal'):
                deploy.confirm_install()

    def args(self, **values):
        return SimpleNamespace(**dict(dict(version=VERSION, jobs=4, cc=None, cxx=None,
                                           cuda='auto', cuda_root=None, cuda_host_compiler=None), **values))

    def test_real_cmake_cache_comments_and_blank_lines_do_not_swallow_keys(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / 'CMakeCache.txt').write_text(
                '# CMake cache\n\n//Version\nBUILD_VERSION:UNINITIALIZED=1.19.0-beta.5\n\n'
                '//Compiler\nCMAKE_CXX_COMPILER:FILEPATH=/usr/bin/g++-15\n')
            self.assertEqual(deploy.read_cache(build), {
                'BUILD_VERSION': VERSION, 'CMAKE_CXX_COMPILER': '/usr/bin/g++-15'})

    def test_version_precedence_and_exact_clean_tag(self):
        with mock.patch.dict(os.environ, {'BUILD_VERSION': '1.20.0'}, clear=True):
            self.assertEqual(deploy.resolve_version(VERSION, {'BUILD_VERSION': '1.18.0'}), VERSION)
            self.assertEqual(deploy.resolve_version(None, {'BUILD_VERSION': '1.18.0'}), '1.20.0')
        with mock.patch.dict(os.environ, {}, clear=True):
            self.assertEqual(deploy.resolve_version(None, {'BUILD_VERSION': VERSION}), VERSION)
            with mock.patch.object(deploy, 'run', side_effect=[
                    mock.Mock(returncode=0, stdout='v1.20.0\n'), mock.Mock(stdout='')]) as run:
                self.assertEqual(deploy.resolve_version(None, {}), '1.20.0')
            self.assertIn('--exact-match', run.call_args_list[0].args)
            with mock.patch.object(deploy, 'run', side_effect=[
                    mock.Mock(returncode=0, stdout='v1.20.0\n'), mock.Mock(stdout=' M modified')]):
                with self.assertRaisesRegex(deploy.DeployError, 'modified tagged checkout'):
                    deploy.resolve_version(None, {})
            with mock.patch.object(deploy, 'run', return_value=mock.Mock(returncode=1, stdout='')):
                with self.assertRaises(deploy.DeployError):
                    deploy.resolve_version(None, {})

    def test_cached_toolchain_and_cuda_are_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'bin').mkdir()
            (root / 'bin/nvcc').write_bytes(b'fixture')
            cache = {'CMAKE_C_COMPILER': '/toolchain/gcc', 'CMAKE_CXX_COMPILER': '/toolchain/g++',
                     'CMAKE_CUDA_HOST_COMPILER': '/toolchain/g++', 'SUNSHINE_ENABLE_CUDA': 'ON',
                     'CUDA_TOOLKIT_ROOT_DIR': temporary}
            with mock.patch.dict(os.environ, {}, clear=True), \
                    mock.patch.object(deploy.shutil, 'which', side_effect=lambda name: name):
                command = deploy.configure_command(self.args(), root / 'build', cache)
            self.assertIn('-DCMAKE_C_COMPILER=/toolchain/gcc', command)
            self.assertIn('-DCMAKE_CUDA_HOST_COMPILER=/toolchain/g++', command)
            self.assertIn(f'-DCMAKE_CUDA_COMPILER={root}/bin/nvcc', command)
            self.assertIn('-DSUNSHINE_ENABLE_CUDA=ON', command)

    def test_no_cuda_detection_and_explicit_overrides(self):
        with mock.patch.dict(os.environ, {'CC': 'env-gcc', 'CXX': 'env-g++'}, clear=True), \
                mock.patch.object(deploy.shutil, 'which', side_effect=lambda name: None if name == 'nvcc' else name), \
                mock.patch.object(Path, 'is_file', return_value=False):
            command = deploy.configure_command(self.args(cc='chosen-gcc'), ROOT / 'build', {})
            self.assertIn('-DCMAKE_C_COMPILER=chosen-gcc', command)
            self.assertIn('-DCMAKE_CXX_COMPILER=env-g++', command)
            self.assertIn('-DSUNSHINE_ENABLE_CUDA=OFF', command)
            with self.assertRaisesRegex(deploy.DeployError, 'CUDA is enabled'):
                deploy.configure_command(self.args(cuda='on'), ROOT / 'build', {})
            with self.assertRaisesRegex(deploy.DeployError, 'CUDA is enabled'):
                deploy.configure_command(self.args(), ROOT / 'build', {'SUNSHINE_ENABLE_CUDA': 'ON'})
            command = deploy.configure_command(self.args(cuda='off'), ROOT / 'build', {'SUNSHINE_ENABLE_CUDA': 'ON'})
            self.assertIn('-DSUNSHINE_ENABLE_CUDA=OFF', command)

    def test_stage_only_platform_check_does_not_need_systemd(self):
        with mock.patch.object(deploy.sys, 'platform', 'linux'), \
                mock.patch.object(deploy.sys, 'version_info', (3, 11)), \
                mock.patch.object(os, 'uname', return_value=SimpleNamespace(machine='x86_64')), \
                mock.patch.object(Path, 'is_dir', return_value=False):
            deploy.platform_preflight(native=False)
            with self.assertRaisesRegex(deploy.DeployError, 'systemd'):
                deploy.platform_preflight()

    def test_signing_defaults_are_allowed_but_shell_and_hooks_are_not(self):
        path = Path('/etc/dkms/framework.conf')
        self.assertTrue(deploy.dkms_config_supported(path, [
            'mok_signing_key="/var/lib/dkms/mok.key" # official default',
            "mok_certificate='/var/lib/dkms/mok.pub'", 'try_sign_modules=not_in_chroot']))
        for line in ('post_transaction="hook"', 'mok_signing_key="/custom/mok.key"',
                     'mok_signing_key="$(command)"', 'try_sign_modules=not_in_chroot; command',
                     'try_sign_modules=not_in_chroot#not-a-comment', 'try_sign_modules = not_in_chroot'):
            self.assertFalse(deploy.dkms_config_supported(path, [line]), line)
        self.assertFalse(deploy.dkms_config_supported(Path('/etc/dkms/vibeshine-drm.conf'),
                                                     ['try_sign_modules=not_in_chroot']))


class NativePackageTests(unittest.TestCase):
    def test_package_metadata_payload_modes_and_maintained_hooks(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            candidate = base / 'candidate.tar.gz'
            with package() as fixture, tarfile.open(candidate, 'w:gz') as output:
                for member in fixture:
                    output.addfile(member, fixture.extractfile(member) if member.isfile() else None)
            destination = base / 'candidate.pkg.tar.gz'
            deploy.build_native_package(candidate, destination, VERSION)
            with tarfile.open(destination) as result:
                info = result.extractfile('.PKGINFO').read().decode()
                self.assertIn('pkgname = vibepollo\n', info)
                self.assertIn('pkgver = 1.19.0.beta.5-1\n', info)
                self.assertIn('depend = dkms\n', info)
                self.assertIn('conflict = sunshine\n', info)
                self.assertEqual(result.extractfile('.INSTALL').read(),
                                 (ROOT / 'packaging/linux/Arch/vibepollo.install').read_bytes())
                self.assertEqual(result.getmember('usr/libexec/vibeshine/vibepollo-host').mode, 0o750)
                self.assertEqual(result.getmember('usr/share/vibepollo').mode, 0o755)
                for member in result:
                    self.assertEqual((member.uid, member.gid), (0, 0))
                self.assertEqual(result.getmember('usr/bin/vibepollo').linkname, f'vibepollo-{VERSION}')
            if deploy.shutil.which('pacman'):
                self.assertEqual(deploy.run('pacman', '-Qp', '--', destination, stderr=deploy.subprocess.PIPE).stdout.strip(),
                                 'vibepollo ' + deploy.arch_package_version(VERSION))

    def test_package_metadata_rejects_shell_expansion_and_missing_arrays(self):
        for recipe in ('depends=("$(id)")', 'depends=("${extra}")', ''):
            with self.assertRaises(deploy.DeployError):
                deploy.package_array(recipe, 'depends')

    def test_package_phase_does_not_require_existing_account(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            source = base / 'input.pkg.tar.gz'
            source.write_bytes(b'fixture')
            state = base / 'state'
            state.mkdir()
            args = SimpleNamespace(archive=source, sha256=deploy.digest(source), version=VERSION,
                                   timeout=30, yes=True)
            with mock.patch.object(deploy, 'STATE', state), \
                    mock.patch.object(deploy, 'run', return_value=SimpleNamespace(
                        stdout='vibepollo ' + deploy.arch_package_version(VERSION))), \
                    mock.patch.object(deploy.subprocess, 'call', return_value=0) as install, \
                    mock.patch.object(deploy, 'package_readiness', return_value=0), \
                    mock.patch.object(deploy.pwd, 'getpwnam', side_effect=KeyError):
                self.assertEqual(deploy.root_package_install(args), 0)
            command = install.call_args.args[0]
            self.assertEqual(command[:2], ['/usr/bin/bash', str(ROOT / 'scripts/linux_install.sh')])
            retained = Path(command[command.index('--package') + 1])
            self.assertEqual(retained.read_bytes(), source.read_bytes())
            self.assertIn('--yes', command)
            latest = json.loads((state / 'latest.json').read_text())['id']
            manifest = json.loads((state / latest / 'transaction.json').read_text())
            self.assertEqual(manifest['backend'], 'pacman')
            self.assertEqual(manifest['status'], 'PACKAGE_INSTALLED')

    def test_changed_retained_package_cannot_finalize_an_old_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / 'candidate.pkg.tar.gz').write_bytes(b'changed')
            with mock.patch.object(deploy, 'verify_payload') as verify:
                with self.assertRaisesRegex(deploy.DeployError, 'Retained package changed'):
                    deploy.verify_package_payload(directory, {'sha256': '0' * 64})
                verify.assert_not_called()

    def test_only_active_or_enabled_legacy_user_units_are_disabled(self):
        results = [SimpleNamespace(returncode=0), SimpleNamespace(returncode=1),
                   SimpleNamespace(returncode=0), SimpleNamespace(returncode=1),
                   SimpleNamespace(returncode=1), SimpleNamespace(returncode=1),
                   SimpleNamespace(returncode=1)]
        with mock.patch.object(deploy, 'run', side_effect=results) as run:
            deploy.stop_legacy_user_hosts()
        self.assertIn(mock.call('systemctl', '--user', 'disable', '--now', 'sunshine.service'),
                      run.call_args_list)
        self.assertNotIn(mock.call('systemctl', '--user', 'disable', '--now', deploy.HOST),
                         run.call_args_list)

    def test_package_resume_never_calls_file_rollback(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / 'transaction.json').write_text(json.dumps({'backend': 'pacman'}))
            with mock.patch.object(deploy, 'transaction_path', return_value=directory), \
                    mock.patch.object(deploy, 'rollback') as rollback, \
                    mock.patch.object(deploy, 'package_readiness', return_value=0) as readiness:
                with self.assertRaisesRegex(deploy.DeployError, 'managed by pacman'):
                    deploy.resume_latest('rollback', None, 'latest')
                self.assertEqual(deploy.resume_latest('finalize', None, 'latest'), 0)
                rollback.assert_not_called()
                readiness.assert_called_once()


class NativeInstallationPreflightTests(unittest.TestCase):
    def test_missing_account_is_actionable_before_build_or_root_staging(self):
        with mock.patch.object(deploy.pwd, 'getpwnam', side_effect=KeyError('vibepollo')), \
                mock.patch.object(deploy, 'platform_preflight'), \
                mock.patch.object(deploy.shutil, 'which', return_value=None), \
                mock.patch.object(deploy.os, 'geteuid', return_value=1000), \
                mock.patch.object(deploy, 'snapshot_archive') as snapshot, \
                mock.patch.object(deploy, 'run') as run:
            for action in (deploy.build_install, deploy.root_install):
                with self.subTest(action=action.__name__), \
                        self.assertRaisesRegex(deploy.DeployError, 'Missing service account.*--stage-only'):
                    action(SimpleNamespace(stage_only=False))
            snapshot.assert_not_called()
            run.assert_not_called()

    def test_missing_configuration_and_unsafe_metadata_are_rejected(self):
        account = SimpleNamespace(pw_uid=123, pw_gid=456)
        with mock.patch.object(deploy.pwd, 'getpwnam', return_value=account), \
                mock.patch.object(deploy.Files, 'parents'), \
                mock.patch.object(deploy.Path, 'lstat') as lstat:
            lstat.side_effect = FileNotFoundError()
            with self.assertRaisesRegex(deploy.DeployError, 'Missing native installation prerequisite'):
                deploy.native_installation_preflight()
            lstat.side_effect = None
            lstat.return_value = SimpleNamespace(st_mode=stat.S_IFLNK | 0o777, st_uid=0, st_gid=0)
            with self.assertRaisesRegex(deploy.DeployError, 'ownership/mode'):
                deploy.native_installation_preflight()
            lstat.side_effect = [
                SimpleNamespace(st_mode=stat.S_IFREG | 0o600, st_uid=0, st_gid=0),
                SimpleNamespace(st_mode=stat.S_IFDIR | 0o700, st_uid=123, st_gid=456),
            ]
            self.assertIs(deploy.native_installation_preflight(), account)


class FilesTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='vibepollo-deploy-test-')
        self.base = Path(self.temporary.name)
        self.root = self.base / 'root'
        self.root.mkdir()
        self.transaction = self.base / 'transaction'
        self.transaction.mkdir()
        self.files = deploy.Files(self.transaction, self.root, os.getuid())

    def tearDown(self):
        self.temporary.cleanup()

    def place(self, name, contents=b'old'):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        return path

    def test_partial_install_restore_content_links_modes_xattrs_and_absence(self):
        public = f'usr/bin/vibepollo-{VERSION}'
        alias = 'usr/bin/vibepollo'
        new = 'usr/libexec/vibeshine/vibepollo-display-power'
        binary = self.place(public)
        binary.chmod(0o751)
        os.setxattr(binary, 'user.vibepollo-test', b'original')
        (self.root / alias).symlink_to(f'vibepollo-{VERSION}')
        saved = self.files.snapshot([public, alias, new])
        candidate = self.base / 'new'
        candidate.write_bytes(b'new')
        self.files.replace(public, candidate, mode=0o755, uid=os.getuid(), gid=os.getgid())
        self.files.replace(new, candidate, mode=0o755, uid=os.getuid(), gid=os.getgid())
        self.files.restore(saved)
        self.assertEqual(binary.read_bytes(), b'old')
        self.assertEqual(stat.S_IMODE(binary.stat().st_mode), 0o751)
        self.assertEqual(os.getxattr(binary, 'user.vibepollo-test'), b'original')
        self.assertEqual(os.readlink(self.root / alias), f'vibepollo-{VERSION}')
        self.assertFalse((self.root / new).exists())

    def test_corrupt_backup_is_rejected_before_any_restore(self):
        name = 'usr/share/vibepollo/web/index.html'
        target = self.place(name)
        saved = self.files.snapshot([name])
        target.write_bytes(b'current')
        (self.transaction / 'before' / saved[name]['backup']).write_bytes(b'tampered')
        with self.assertRaises(deploy.DeployError):
            self.files.restore(saved)
        self.assertEqual(target.read_bytes(), b'current')

    def test_symlink_parent_is_rejected(self):
        (self.root / 'usr').symlink_to(self.base, target_is_directory=True)
        with self.assertRaises(deploy.DeployError):
            self.files.snapshot(['usr/bin/vibepollo'])

    def test_private_public_asset_parent_is_rejected_before_install(self):
        name = 'usr/share/vibepollo/prelogin/apps.json'
        target = self.place(name)
        target.parent.chmod(0o700)
        with self.assertRaisesRegex(deploy.DeployError, 'not traversable'):
            self.files.snapshot([name])
        self.assertEqual(stat.S_IMODE(target.parent.stat().st_mode), 0o700)
        self.assertEqual(target.read_bytes(), b'old')

    def test_private_driver_state_parent_remains_allowed(self):
        name = 'var/lib/vibeshine-drm/source-marker'
        target = self.place(name)
        target.parent.chmod(0o700)
        files = deploy.Files(self.transaction, self.root, os.getuid(), validator=lambda _: True)
        self.assertIsNotNone(files.snapshot([name])[name])

    def test_new_dirs_remain_traversable_under_private_umask(self):
        source = self.base / 'source'
        source.write_bytes(b'code')
        previous = os.umask(0o077)
        try:
            self.files.replace('usr/libexec/vibeshine/vibepollo-display-power', source,
                               mode=0o755, uid=os.getuid(), gid=os.getgid())
        finally:
            os.umask(previous)
        for name in self.files.created_dirs:
            self.assertEqual(stat.S_IMODE((self.root / name).stat().st_mode), 0o755)

    def test_archive_copy_is_hash_checked_and_rejects_symlinks(self):
        source = self.base / 'source'
        source.write_bytes(b'archive')
        with self.assertRaises(deploy.DeployError):
            deploy.snapshot_archive(source, '0' * 64, self.base / 'bad-copy')
        link = self.base / 'link'
        link.symlink_to(source)
        with self.assertRaises(OSError):
            deploy.snapshot_archive(link, deploy.digest(source), self.base / 'link-copy')

    def test_driver_restore_recovers_deleted_modules_sources_and_dkms_without_following_links(self):
        old_module = 'usr/lib/modules/6.18/updates/dkms/vibeshine_drm.ko.zst'
        old_source = 'usr/src/vibeshine-drm-1.18.0/Makefile'
        marker = 'var/lib/vibeshine-drm/dkms-1.18.0-6.18'
        link = 'var/lib/dkms/vibeshine-drm/1.18.0/source'
        for name in (old_module, old_source, marker):
            self.place(name)
        (self.root / link).parent.mkdir(parents=True)
        (self.root / link).symlink_to(self.root / 'usr/src/vibeshine-drm-1.18.0')
        driver = self.transaction / 'driver'
        driver.mkdir()
        files, dirs = deploy.driver_inventory(self.root, os.getuid())
        before = deploy.driver_state(self.root, os.getuid())
        saved = deploy.Files(driver, self.root, os.getuid(), deploy.driver_allowed).snapshot(files)
        (self.root / old_module).unlink()
        (self.root / old_source).unlink()
        (self.root / marker).write_bytes(b'new-marker')
        self.place('usr/lib/modules/6.18/updates/vibepollo/vibeshine_drm.ko', b'candidate')
        self.place('usr/src/vibeshine-drm-1.19.0/Makefile', b'new-source')
        with mock.patch.object(deploy, 'run') as run:
            deploy.restore_driver(self.transaction, {'before': saved, 'directories': dirs}, self.root, os.getuid())
        self.assertEqual(deploy.driver_state(self.root, os.getuid()), before)
        run.assert_called_once_with('depmod', '-a', '6.18', timeout=120)

    def test_driver_inventory_rejects_special_files_and_other_modules(self):
        self.assertFalse(deploy.driver_allowed('usr/lib/modules/6.18/updates/dkms/nvidia.ko'))
        self.assertFalse(deploy.driver_allowed('var/lib/dkms/mok.key'))
        self.assertFalse(deploy.driver_allowed('var/lib/vibeshine-drm/../../shadow'))
        fifo = self.root / 'var/lib/vibeshine-drm/fifo'
        fifo.parent.mkdir(parents=True)
        os.mkfifo(fifo)
        with self.assertRaisesRegex(deploy.DeployError, 'Special driver state'):
            deploy.driver_inventory(self.root, os.getuid())


class PolicyTests(unittest.TestCase):
    def test_latest_transaction_is_resolved_without_an_id(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for command, status, expected in [('finalize', 'UNHEALTHY', 'finalize'),
                                               ('rollback', 'UNHEALTHY', 'rollback'),
                                               ('rollback', 'ROLLBACK_REBOOT_REQUIRED', 'finalize')]:
                (directory / 'transaction.json').write_text(json.dumps({'status': status}))
                with mock.patch.object(deploy, 'transaction_path', return_value=directory) as resolve, \
                        mock.patch.object(deploy, 'rollback') as undo, \
                        mock.patch.object(deploy, 'finalize') as finish:
                    deploy.resume_latest(command, None, 'latest-id')
                resolve.assert_called_once_with('latest-id')
                self.assertEqual(undo.call_count, int(expected == 'rollback'))
                self.assertEqual(finish.call_count, int(expected == 'finalize'))

    def test_missing_or_stale_transaction_does_not_run_recovery(self):
        for identifier, latest in [(None, None), ('older', 'latest')]:
            with mock.patch.object(deploy, 'transaction_path') as resolve:
                with self.assertRaises(deploy.DeployError):
                    deploy.resume_latest('rollback', identifier, latest)
                resolve.assert_not_called()

    def test_unhealthy_candidate_can_retry_same_boot_without_auto_rollback(self):
        manifest = {'status': 'UNHEALTHY', 'rollback_policy': 'always', 'baseline': 'healthy',
                    'timeout': 90, 'after': {}, 'driver': {'boot_id': 'same-boot',
                    'kernels': [os.uname().release], 'after': {}}}
        with mock.patch.object(Path, 'read_text', return_value='same-boot'), \
                mock.patch.object(deploy, 'driver_needs_reboot', return_value=False), \
                mock.patch.object(deploy, 'driver_state', return_value={}), \
                mock.patch.object(deploy, 'run'), mock.patch.object(deploy, 'start_controller'), \
                mock.patch.object(deploy, 'write_json'), mock.patch.object(deploy, 'rollback') as undo, \
                mock.patch.object(deploy, 'readiness', side_effect=[('unhealthy', 'not ready'),
                                                                  ('healthy', 'ready')]):
            with self.assertRaisesRegex(deploy.DeployError, 'not ready'):
                deploy.finalize(Path('/unused'), manifest)
            self.assertEqual(manifest['status'], 'UNHEALTHY')
            undo.assert_not_called()
            deploy.finalize(Path('/unused'), manifest)
            self.assertEqual(manifest['status'], 'COMMITTED')

    def test_driver_waits_for_naturally_exiting_descendant(self):
        import ctypes

        # Container PID 1 need not reap the intentionally orphaned fixture.
        # Adopt it locally and supply the reaping a normal host init performs;
        # keep the real process-group probe and the production rollback guard.
        libc = ctypes.CDLL(None, use_errno=True)
        previous = ctypes.c_int()
        self.assertEqual(libc.prctl(37, ctypes.byref(previous), 0, 0, 0), 0)  # PR_GET_CHILD_SUBREAPER
        self.assertEqual(libc.prctl(36, 1, 0, 0, 0), 0)  # PR_SET_CHILD_SUBREAPER
        group_empty = deploy.driver_group_empty

        def reap_and_probe(group):
            # driver_command already waited for its direct child before probing.
            while True:
                try:
                    child, _ = os.waitpid(-group, os.WNOHANG)
                except ChildProcessError:
                    break
                if child == 0:
                    break
            return group_empty(group)

        try:
            with tempfile.TemporaryDirectory() as temporary, \
                    mock.patch.object(deploy, 'driver_group_empty', side_effect=reap_and_probe):
                marker = Path(temporary) / 'finished'
                command = ['/usr/bin/bash', '-c',
                           '(sleep 0.2; printf done > "$1") & exit 4',
                           'driver-fixture', str(marker)]
                self.assertEqual(deploy.driver_command(command), 4)
                self.assertEqual(marker.read_text(), 'done')
        finally:
            self.assertEqual(libc.prctl(36, previous.value, 0, 0, 0), 0)

    def test_driver_persistent_descendant_still_forces_cleanup_and_failure(self):
        process = mock.Mock(pid=123)
        process.wait.return_value = 0
        with mock.patch.object(deploy.subprocess, 'Popen', return_value=process), \
                mock.patch.object(os, 'killpg') as kill, \
                mock.patch.object(deploy, 'driver_group_empty', side_effect=[False, False, True, True]), \
                mock.patch.object(deploy.time, 'monotonic', side_effect=[0, 6, 6]):
            with self.assertRaisesRegex(deploy.DeployError, 'left descendants running'):
                deploy.driver_command(['unused'])
        kill.assert_any_call(123, deploy.signal.SIGTERM)
        kill.assert_any_call(123, deploy.signal.SIGKILL)

    def test_synthetic_encoder_probe_does_not_require_future_client_capture_logs(self):
        with mock.patch.object(deploy, 'unit_properties', return_value={
                'ActiveState': 'active', 'ControlGroup': '/system.slice/vibepollo.service', 'InvocationID': 'a' * 32}), \
                mock.patch.object(Path, 'read_text', return_value='123\n'), \
                mock.patch.object(deploy, 'run', return_value=mock.Mock(stdout='users:(("host",pid=123,fd=1))')), \
                mock.patch.object(deploy, 'capture_logs', return_value='Found H.264 encoder: h264_nvenc [nvenc]'), \
                mock.patch.object(deploy, 'managed_pool_state', return_value='active'):
            status, detail = deploy.health()
        self.assertEqual(status, 'healthy')
        self.assertIn('capture untested', detail)

    def test_same_version_different_driver_source_requires_reboot(self):
        with mock.patch.object(Path, 'exists', return_value=True), \
                mock.patch.object(Path, 'is_file', return_value=True), \
                mock.patch.object(Path, 'read_text', side_effect=['1.19.0', 'old-srcversion']), \
                mock.patch.object(deploy, 'run', side_effect=[
                    mock.Mock(stdout='1.19.0', returncode=0), mock.Mock(stdout='new-srcversion', returncode=0)]):
            self.assertTrue(deploy.driver_needs_reboot())

    def test_loaded_driver_without_disk_module_requires_reboot_on_rollback(self):
        with mock.patch.object(Path, 'exists', return_value=True), \
                mock.patch.object(deploy, 'run', return_value=mock.Mock(stdout='not found', returncode=1)):
            self.assertTrue(deploy.driver_needs_reboot())

    def test_driver_cancellation_does_not_allow_rollback_with_surviving_workers(self):
        process = mock.Mock(pid=123)
        process.wait.side_effect = [KeyboardInterrupt(), 0, 0]
        with mock.patch.object(deploy.subprocess, 'Popen', return_value=process), \
                mock.patch.object(os, 'killpg'), \
                mock.patch.object(deploy, 'driver_group_empty', return_value=False), \
                mock.patch.object(deploy.time, 'monotonic', side_effect=[0, 6]):
            with self.assertRaises(deploy.DriverBusy):
                deploy.driver_command(['unused'])

    def test_finalize_requires_a_real_reboot(self):
        manifest = {'status': 'REBOOT_REQUIRED', 'driver': {'boot_id': 'same-boot'}}
        with mock.patch.object(Path, 'read_text', return_value='same-boot'), \
                mock.patch.object(deploy, 'start_controller') as start:
            with self.assertRaisesRegex(deploy.DeployError, 'Reboot first'):
                deploy.finalize(Path('/unused'), manifest)
        start.assert_not_called()
    def test_failed_install_is_retained_until_explicit_recovery(self):
        for phase, status in [('mutating', 'MUTATING'), ('readiness', 'UNHEALTHY')]:
            manifest = {'rollback_policy': 'always'}
            with mock.patch.object(deploy, 'write_json'), \
                    mock.patch.object(deploy, 'quiesce') as stop, \
                    mock.patch.object(deploy, 'rollback') as undo:
                deploy.retain_failed_install(Path('/unused'), manifest, phase, 'failure')
            self.assertEqual(manifest['status'], status)
            self.assertEqual(manifest['failure'], 'failure')
            self.assertEqual(stop.call_count, int(phase == 'mutating'))
            undo.assert_not_called()


    def test_tests_are_skipped_unless_enforced(self):
        with mock.patch.object(deploy.subprocess, 'run') as run:
            deploy.enforce_tests(ROOT / 'build', False)
        run.assert_not_called()

    def test_enforce_gates_every_test_failure(self):
        for code in (1, 8):
            with mock.patch.object(deploy.subprocess, 'run', return_value=mock.Mock(returncode=code)):
                with self.assertRaisesRegex(deploy.DeployError, 'Enforced tests failed'):
                    deploy.enforce_tests(ROOT / 'build', True)
        with mock.patch.object(deploy.subprocess, 'run', return_value=mock.Mock(returncode=0)) as run:
            deploy.enforce_tests(ROOT / 'build', True, jobs=3)
        self.assertEqual(run.call_args.args[0][0], 'ctest')
        self.assertIn('--no-tests=error', run.call_args.args[0])
        self.assertIn('-j3', run.call_args.args[0])

    def test_quiesce_masks_before_stopping_and_never_starts_host(self):
        calls = []
        def fake_run(*args, **kwargs):
            calls.append(args)
            return mock.Mock(stdout='', returncode=0)
        with mock.patch.object(deploy, 'run', fake_run), \
                mock.patch.object(deploy, 'broker_units', return_value=[]), \
                mock.patch.object(deploy, 'unit_properties', return_value={'ActiveState': 'inactive'}), \
                mock.patch.object(Path, 'exists', return_value=False):
            deploy.quiesce()
            deploy.start_controller()
        self.assertEqual(calls[0], ('systemctl', 'mask', '--runtime', deploy.SOCKET))
        self.assertIn(('systemctl', 'start', deploy.CONTROLLER), calls)
        self.assertNotIn(('systemctl', 'start', deploy.HOST), calls)

    def test_quiesce_tracks_brokers_accepted_during_socket_close(self):
        first = 'vibepollo-session-exec@1.service'
        late = 'vibepollo-session-exec@2.service'
        calls = []
        def fake_run(*args, **kwargs):
            calls.append(args)
            return mock.Mock(stdout='', returncode=0)
        with mock.patch.object(deploy, 'run', fake_run), \
                mock.patch.object(deploy, 'broker_units', side_effect=[[first], [first, late], [first, late], [first, late]]), \
                mock.patch.object(deploy, 'unit_properties', return_value={'ActiveState': 'inactive'}) as properties, \
                mock.patch.object(Path, 'exists', return_value=False):
            deploy.quiesce()
        self.assertIn(('systemctl', 'stop', late), calls)
        self.assertIn(mock.call(late), properties.call_args_list)

    def test_quiesce_rejects_populated_stopped_cgroup(self):
        with mock.patch.object(deploy, 'run'), \
                mock.patch.object(deploy, 'broker_units', return_value=[]), \
                mock.patch.object(deploy, 'unit_properties', return_value={
                    'ActiveState': 'inactive', 'ControlGroup': '/system.slice/vibepollo.service'}), \
                mock.patch.object(Path, 'exists', return_value=True), \
                mock.patch.object(Path, 'read_text', return_value='populated 1\n'):
            with self.assertRaisesRegex(deploy.DeployError, 'still populated'):
                deploy.quiesce()

    def test_readiness_retries_startup_probe_failures(self):
        with mock.patch.object(deploy, 'unit_properties', return_value={'ActiveState': 'active'}), \
                mock.patch.object(deploy.time, 'sleep'), \
                mock.patch.object(deploy, 'power_probe', side_effect=[
                    deploy.DeployError('starting'), ('unknown', 'probing encoders'), ('healthy', 'ready')]):
            self.assertEqual(deploy.readiness(10), ('healthy', 'ready'))

    def test_nongraphical_seat_waits_without_claiming_capture_validation(self):
        with mock.patch.object(deploy.time, 'monotonic', side_effect=[0, 11]), \
                mock.patch.object(deploy, 'run', side_effect=[
                    mock.Mock(returncode=0, stdout='3\n'), mock.Mock(returncode=0, stdout='tty\n')]), \
                mock.patch.object(Path, 'exists', return_value=False), \
                mock.patch.object(deploy, 'unit_properties', return_value={'ActiveState': 'active'}):
            self.assertEqual(deploy.readiness(10)[0], 'waiting-session')


class ManagedPoolReadinessTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.drm = self.root / 'drm'
        self.output = self.drm / 'card2-Virtual-1/enabled'
        self.output.parent.mkdir(parents=True)
        self.output.write_text('disabled\n')
        (self.output.parent / 'status').write_text('disconnected\n')
        self.device = self.root / 'devices/faux/vibeshine'
        self.device.mkdir(parents=True)
        (self.drm / 'card2').mkdir()
        (self.drm / 'card2/device').symlink_to(self.device)
        (self.drm / 'card0-HDMI-A-1').mkdir()
        (self.drm / 'card0-HDMI-A-1/enabled').write_text('enabled\n')
        self.control = self.root / 'control.sock'
        self.socket = socket.socket(socket.AF_UNIX)
        self.addCleanup(self.socket.close)
        self.socket.bind(str(self.control))
        self.control.chmod(0o660)
        # Exercise real inode types/modes while simulating root ownership.
        original_lstat = Path.lstat
        self.socket_uid = 0

        def owned_lstat(path):
            info = original_lstat(path)
            if path == self.control:
                return SimpleNamespace(st_mode=info.st_mode, st_uid=self.socket_uid)
            return info

        self.lstat = mock.patch.object(Path, 'lstat', owned_lstat)
        self.lstat.start()
        self.addCleanup(self.lstat.stop)

    def pool_state(self):
        return deploy.managed_pool_state(self.drm, self.control)

    def test_idle_greeter_pool_passes_without_client_capture_or_virtual_scanout(self):
        original_state = deploy.managed_pool_state
        original_read = Path.read_text

        def read_text(path, *args, **kwargs):
            if str(path) == '/sys/fs/cgroup/system.slice/vibepollo.service/cgroup.procs':
                return '123\n'
            return original_read(path, *args, **kwargs)

        with mock.patch.object(deploy, 'unit_properties', return_value={
                'ActiveState': 'active', 'ControlGroup': '/system.slice/vibepollo.service',
                'InvocationID': 'a' * 32}), \
                mock.patch.object(Path, 'read_text', read_text), \
                mock.patch.object(deploy, 'run', return_value=mock.Mock(stdout='users:(("host",pid=123,fd=1))')), \
                mock.patch.object(deploy, 'capture_logs', return_value='Found H.264 encoder: h264_nvenc [nvenc]'), \
                mock.patch.object(deploy, 'managed_pool_state', side_effect=lambda: original_state(self.drm, self.control)):
            status, detail = deploy.health()
        self.assertEqual(status, 'healthy')
        self.assertIn('dormant managed pool (idle)', detail)
        self.assertIn('client capture untested', detail)
        self.assertEqual(self.output.read_text(), 'disabled\n')

    def test_active_pool_is_distinct_from_dormant_pool(self):
        self.assertEqual(self.pool_state(), 'idle')
        self.output.write_text('enabled\n')
        self.assertEqual(self.pool_state(), 'active')

    def test_missing_driver_target_is_not_hidden_by_physical_scanout(self):
        self.device.rmdir()
        self.assertIsNone(self.pool_state())

    def test_unrelated_driver_is_not_a_managed_pool(self):
        (self.drm / 'card2/device').unlink()
        (self.drm / 'card2/device').symlink_to(self.drm)
        self.assertIsNone(self.pool_state())

    def test_missing_or_invalid_connector_state_is_not_ready(self):
        self.output.write_text('unknown\n')
        self.assertIsNone(self.pool_state())
        self.output.unlink()
        self.assertIsNone(self.pool_state())

    def test_missing_control_endpoint_is_not_ready(self):
        self.control.unlink()
        self.assertIsNone(self.pool_state())

    def test_untrusted_control_socket_is_rejected_for_idle_and_active_pool(self):
        for enabled in ('disabled', 'enabled'):
            self.output.write_text(enabled + '\n')
            with self.subTest(enabled=enabled, reason='owner'):
                self.socket_uid = 1000
                self.assertIsNone(self.pool_state())
                self.socket_uid = 0
            with self.subTest(enabled=enabled, reason='permissions'):
                self.control.chmod(0o666)
                self.assertIsNone(self.pool_state())
                self.control.chmod(0o660)

    def test_regular_file_and_symlink_are_not_control_sockets(self):
        saved_socket = self.root / 'saved.sock'
        self.control.rename(saved_socket)
        self.control.write_text('not a socket')
        self.assertIsNone(self.pool_state())
        self.control.unlink()
        self.control.symlink_to(saved_socket)
        self.assertIsNone(self.pool_state())


class RollbackTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='vibepollo-rollback-test-')
        self.directory = Path(self.temporary.name)
        self.manifest = {'status': 'MUTATING', 'payload_mutated': True,
                         'before': {}, 'after': {}, 'controller_active': True}

    def tearDown(self):
        self.temporary.cleanup()

    def test_success_restores_before_starting_controller(self):
        order = []
        with mock.patch.object(deploy, 'Files') as files, \
                mock.patch.object(deploy, 'quiesce', side_effect=lambda: order.append('stop')), \
                mock.patch.object(deploy, 'run'), \
                mock.patch.object(deploy, 'start_controller', side_effect=lambda active: order.append('start')):
            files.return_value.restore.side_effect = lambda saved: order.append('restore')
            deploy.rollback(self.directory, self.manifest)
        self.assertEqual(order, ['stop', 'restore', 'start'])
        self.assertEqual(self.manifest['status'], 'ROLLED_BACK')

    def test_failed_restore_keeps_admission_closed_and_never_restarts(self):
        with mock.patch.object(deploy, 'Files') as files, \
                mock.patch.object(deploy, 'quiesce') as stop, \
                mock.patch.object(deploy, 'start_controller') as start:
            files.return_value.restore.side_effect = OSError('disk error')
            with self.assertRaises(OSError):
                deploy.rollback(self.directory, self.manifest)
        self.assertEqual(self.manifest['status'], 'ROLLBACK_FAILED')
        self.assertEqual(stop.call_count, 2)
        start.assert_not_called()

    def test_prepared_recovery_does_not_restore_stale_files(self):
        self.manifest.update(status='PREPARED', payload_mutated=False)
        with mock.patch.object(deploy, 'Files') as files, \
                mock.patch.object(deploy, 'quiesce'), \
                mock.patch.object(deploy, 'start_controller') as start:
            deploy.rollback(self.directory, self.manifest)
        files.return_value.restore.assert_not_called()
        start.assert_called_once_with(True)
        self.assertEqual(self.manifest['status'], 'ABORTED')

    def test_completed_install_metadata_drift_blocks_rollback(self):
        name = 'usr/bin/vibepollo-1.19.0-beta.5'
        self.manifest.update(status='COMMITTED', after={name: {'sha256': 'new'}},
                             installed_metadata={name: {'mode': 0o755}})
        with mock.patch.object(deploy, 'fingerprint', return_value={'sha256': 'new'}), \
                mock.patch.object(deploy, 'metadata', return_value={'mode': 0o4755}), \
                mock.patch.object(deploy, 'quiesce') as stop:
            with self.assertRaisesRegex(deploy.DeployError, 'metadata changed'):
                deploy.rollback(self.directory, self.manifest)
        stop.assert_not_called()

    def test_partial_install_rejects_drift_in_old_and_new_file_metadata(self):
        name = 'usr/bin/vibepollo-1.19.0-beta.5'
        original = {'uid': 0, 'gid': 0, 'mode': 0o755, 'xattrs': {}}
        self.manifest.update(after={name: {'sha256': 'new'}},
                             before={name: dict(original, sha256='old')},
                             intended_metadata={name: original})
        for content in ('old', 'new', 'third-party'):
            with self.subTest(content=content), \
                    mock.patch.object(deploy, 'fingerprint', return_value={'sha256': content}), \
                    mock.patch.object(deploy, 'metadata', return_value=dict(original, mode=0o4755)), \
                    mock.patch.object(deploy, 'run', return_value=mock.Mock(stdout='')), \
                    mock.patch.object(deploy, 'quiesce') as stop:
                with self.assertRaises(deploy.DeployError):
                    deploy.rollback(self.directory, self.manifest)
                stop.assert_not_called()


if __name__ == '__main__':
    unittest.main()
