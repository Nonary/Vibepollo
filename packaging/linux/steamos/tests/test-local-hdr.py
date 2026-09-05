#!/usr/bin/env python3
"""Local deployment guards; all fixtures are temporary and no service is touched."""
import importlib.util
import json
import pathlib
import platform
import tempfile
import types
import unittest
from unittest import mock

source = pathlib.Path(__file__).resolve().parents[1] / 'local/gamescope-wrapper.py'
spec = importlib.util.spec_from_file_location('local_wrapper', source)
wrapper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wrapper)
spec = importlib.util.spec_from_file_location('local_activation', source.parent / 'activate-gamescope.py')
activation = importlib.util.module_from_spec(spec)
spec.loader.exec_module(activation)


class LocalGamescopeGuards(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        inventory = ('bin/gamescope', 'libexec/gamescope', 'libexec/gamescope-session',
                     'lib/libstdc++.so.6', 'lib/libgcc_s.so.1',
                     'share/gamescope/scripts/00-gamescope/displays/valve.steamdeck.oled.lua')
        for name in inventory:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('fixture ' + name)
        self.identity = {'ID': 'steamos', 'VERSION_ID': '3.8.24', 'BUILD_ID': 'fixture'}
        self.manifest = {'os': self.identity, 'architecture': platform.machine(),
                         'stock_gamescope_package': 'gamescope 3.16.23.5-1',
                         'stock_gamescope_sha256': 'stock', 'stock_session_sha256': 'session',
                         'files': {name: wrapper.digest(self.root / name) for name in inventory}}
        self.write_manifest()
        original_digest = wrapper.digest

        def digest(path):
            if str(path) == '/usr/bin/gamescope':
                return 'stock'
            if str(path) == '/usr/lib/steamos/gamescope-session':
                return 'session'
            return original_digest(path)

        for patch in (mock.patch.object(wrapper, 'os_identity', return_value=self.identity),
                      mock.patch.object(wrapper, 'digest', side_effect=digest),
                      mock.patch.object(wrapper.subprocess, 'check_output', return_value='gamescope 3.16.23.5-1\n')):
            patch.start()
            self.addCleanup(patch.stop)

    def write_manifest(self):
        (self.root / 'local-manifest.json').write_text(json.dumps(self.manifest))

    def test_intact_candidate(self):
        self.assertEqual(wrapper.verify(self.root, privileged=False), self.manifest)

    def test_os_update_rejected(self):
        self.manifest['os'] = dict(self.identity, BUILD_ID='another-build')
        self.write_manifest()
        with self.assertRaisesRegex(RuntimeError, 'version/build'):
            wrapper.verify(self.root, privileged=False)

    def test_modified_candidate_rejected(self):
        (self.root / 'libexec/gamescope').write_text('changed')
        with self.assertRaisesRegex(RuntimeError, 'file changed'):
            wrapper.verify(self.root, privileged=False)

    def test_unlisted_file_rejected(self):
        (self.root / 'lib/extra.so').write_text('unlisted')
        with self.assertRaisesRegex(RuntimeError, 'inventory'):
            wrapper.verify(self.root, privileged=False)

    def test_incomplete_manifest_rejected(self):
        self.manifest['files'] = {}
        self.write_manifest()
        with self.assertRaisesRegex(RuntimeError, 'Incomplete'):
            wrapper.verify(self.root, privileged=False)

    def test_symlink_directory_rejected(self):
        (self.root / 'alias').symlink_to('lib', target_is_directory=True)
        with self.assertRaisesRegex(RuntimeError, 'Symlinks'):
            wrapper.verify(self.root, privileged=False)

    def test_session_guard_failure_uses_stock_session(self):
        with mock.patch.object(wrapper, 'verify', side_effect=RuntimeError('missing capability')):
            with mock.patch.object(wrapper.sys, 'argv', ['gamescope', '--vibepollo-session']):
                with mock.patch.object(wrapper.os, 'execv', side_effect=SystemExit) as execute:
                    with self.assertRaises(SystemExit):
                        wrapper.main()
                    execute.assert_called_once_with('/usr/lib/steamos/gamescope-session', ['gamescope-session'])

    def test_empty_capability_is_rejected_without_index_error(self):
        with mock.patch.object(wrapper.subprocess, 'check_output', return_value=''):
            with self.assertRaisesRegex(RuntimeError, 'CAP_SYS_NICE'):
                wrapper.verify_capabilities(self.root / 'libexec/gamescope')

    def test_package_update_rejected(self):
        with mock.patch.object(wrapper.subprocess, 'check_output', return_value='gamescope 4.0-1\n'):
            with self.assertRaisesRegex(RuntimeError, 'package changed'):
                wrapper.verify(self.root, privileged=False)

    def test_malformed_manifest_rejected(self):
        self.manifest['files'] = []
        self.write_manifest()
        with self.assertRaisesRegex(RuntimeError, 'schema'):
            wrapper.verify(self.root, privileged=False)

    def test_stage_replaces_only_compositor_exec_and_records_rpath(self):
        candidate = dict(self.manifest, source={'patch_sha256': 'a' * 64})
        (self.root / 'manifest.json').write_text(json.dumps(candidate))
        original = 'export TEST=untouched\nexec gamescope \\\n  -w 1280 -h 800\n'
        read_text = pathlib.Path.read_text

        def read(path, *args, **kwargs):
            if str(path) == '/usr/lib/steamos/gamescope-session':
                return original
            return read_text(path, *args, **kwargs)

        with tempfile.TemporaryDirectory() as work:
            output = pathlib.Path(work) / 'stage'
            args = types.SimpleNamespace(candidate=self.root, output=output, patchelf=pathlib.Path('/tool/patchelf'))
            with mock.patch.object(activation, 'wrapper', wrapper):
                with mock.patch.object(pathlib.Path, 'read_text', read):
                    with mock.patch.object(activation.subprocess, 'run') as run:
                        activation.stage(args)
            manifest = json.loads((output / 'local-manifest.json').read_text())
            destination = pathlib.Path(manifest['install_root'])
            self.assertEqual((output / 'libexec/gamescope-session').read_text(), original.replace(
                'exec gamescope', 'exec ' + str(destination / 'bin/gamescope')))
            run.assert_called_once_with(['/tool/patchelf', '--set-rpath', str(destination / 'lib'),
                                         str(output / 'libexec/gamescope')], check=True)
            self.assertEqual(set(manifest['files']), {
                str(path.relative_to(output)) for path in output.rglob('*')
                if path.is_file() and path.name != 'local-manifest.json'})


if __name__ == '__main__':
    unittest.main()
