#!/usr/bin/env python3
"""Local pool deployment regressions; root integration cases use disposable paths."""
import argparse
import contextlib
import importlib.util
import io
import hashlib
import json
import os
import pathlib
import shutil
import stat
import subprocess
import tempfile
import types
import unittest
from unittest import mock

LOCAL = pathlib.Path(__file__).resolve().parents[1] / 'local'
spec = importlib.util.spec_from_file_location('activate_private_display', LOCAL / 'activate-private-display.py')
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)
loader = installer.loader
REAL_COMMAND = loader.command


class PrivateDisplayTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='vibeshine-private-display-test-')
        self.root = pathlib.Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)
        self.module = self.root / 'module.ko'
        self.module.write_bytes(b'fixture module')
        self.peercred = self.root / 'peercred'
        self.peercred.write_bytes(b'fixture peercred')
        self.source = self.root / 'driver'
        (self.source / 'linux/packaging').mkdir(parents=True)
        for name in ('vibeshine-vkms', 'vibeshine-vkms-quiesce'):
            (self.source / 'linux/packaging' / name).write_text('#!/bin/bash\nexit 0\n')
        self.stage = self.root / 'stage'
        self.install_parent = self.root / 'opt/vibeshine-private-display'
        self.install_parent.parent.mkdir()
        self.units = self.root / 'etc/systemd/system'
        self.units.mkdir(parents=True)
        self.vendor = self.root / 'vendor-units'
        self.vendor.mkdir()
        self.local = self.root / 'local'
        self.local.mkdir()
        shutil.copyfile(LOCAL / 'private-display-loader.py', self.local / 'private-display-loader.py')
        (self.local / 'private-display-mode-broker').write_text('#!/bin/bash\nexit 0\n')
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(mock.patch.object(
            installer.pwd, 'getpwnam', return_value=types.SimpleNamespace(pw_uid=1000, pw_gid=1000)))
        self.stack.enter_context(mock.patch.object(
            installer.grp, 'getgrgid', return_value=types.SimpleNamespace(gr_name='deck')))
        for obj, name, value in (
                (loader, 'INSTALL_PARENT', self.install_parent),
                (loader, 'MODULE_SYSFS', self.root / 'module-sysfs'),
                (loader, 'POOL', self.root / 'configfs-pool'),
                (installer, 'UNITS', self.units), (installer, 'VENDOR_UNITS', self.vendor),
                (installer, 'HERE', self.local)):
            self.stack.enter_context(mock.patch.object(obj, name, value))
        original_safe = loader.safe_directory
        def safe_fixture_directory(path):
            if path == self.root or self.root in path.parents:
                original_safe(path)
        self.stack.enter_context(mock.patch.object(loader, 'safe_directory', safe_fixture_directory))
        self.stack.enter_context(mock.patch.object(loader, 'modinfo', side_effect=lambda module, field: {
            'name': 'vibeshine_drm', 'version': '1.19.0',
            'srcversion': hashlib.sha256(module.read_bytes()).hexdigest()[:24].upper(),
            'vermagic': loader.platform.release() + ' SMP', 'depends': ''}[field]))
        self.commands = self.stack.enter_context(mock.patch.object(loader, 'command'))
        self.stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
        installer.stage(argparse.Namespace(module=self.module, peercred=self.peercred,
                                           driver_source=self.source, output=self.stage,
                                           user='deck', capture_helper=None))
        self.manifest = loader.verify(self.stage, privileged=False)
        self.destination = pathlib.Path(self.manifest['install_root'])

    def make_legacy_stage(self):
        """Reproduce the already-installed v1 inventory from before mode support."""
        manifest = json.loads((self.stage / loader.MANIFEST).read_text())
        manifest.pop('requested_mode_broker')
        manifest['files'].pop(loader.MODE_BROKER)
        (self.stage / loader.MODE_BROKER).unlink()
        unit_name = 'units/vibeshine-vkms-control@.service'
        unit = self.stage / unit_name
        unit.write_text(unit.read_text().replace('/private-display-mode-broker control', '/vibeshine-vkms control'))
        manifest['files'][unit_name] = loader.digest(unit)
        (self.stage / loader.MANIFEST).write_text(json.dumps(manifest))
        self.manifest = loader.verify(self.stage, privileged=False)

    def mark_loaded(self, manifest):
        loader.MODULE_SYSFS.mkdir(exist_ok=True)
        for field in ('version', 'srcversion'):
            (loader.MODULE_SYSFS / field).write_text(manifest['module_' + field])
        (loader.MODULE_SYSFS / 'refcnt').write_text('7')
        loader.POOL.mkdir(exist_ok=True)

    def stage_upgrade(self):
        self.module.write_bytes(b'updated module')
        candidate = self.root / 'updated-stage'
        installer.stage(argparse.Namespace(module=self.module, peercred=self.peercred,
                                           driver_source=self.source, output=candidate,
                                           user='deck', capture_helper=None))
        manifest = loader.verify(candidate, privileged=False)
        return candidate, pathlib.Path(manifest['install_root']), manifest

    def test_stage_pins_kernel_and_broker_root_ownership(self):
        socket = (self.stage / 'units/vibeshine-vkms-control.socket').read_text()
        self.assertIn('SocketUser=root\n', socket)
        self.assertIn('SocketGroup=deck\n', socket)
        self.assertIn('SocketMode=0660\n', socket)
        with mock.patch.object(loader.platform, 'release', return_value='different-kernel'):
            with self.assertRaisesRegex(RuntimeError, 'Kernel changed'):
                loader.verify(self.stage, privileged=False)

    def test_rejects_tampered_files_and_symlinks(self):
        peercred = self.stage / 'libexec/vibeshine-vkms-peercred'
        peercred.write_bytes(b'changed')
        with self.assertRaisesRegex(RuntimeError, 'checksum mismatch'):
            loader.verify(self.stage, privileged=False)
        peercred.unlink()
        peercred.symlink_to(self.peercred)
        with self.assertRaisesRegex(RuntimeError, 'Unsupported bundle entry'):
            loader.verify(self.stage, privileged=False)

    def test_nested_manifest_cannot_bypass_inventory(self):
        (self.stage / 'lib' / loader.MANIFEST).write_text('{}')
        with self.assertRaisesRegex(RuntimeError, 'checksum mismatch'):
            loader.verify(self.stage, privileged=False)

    def test_legacy_inventory_remains_verifiable(self):
        self.make_legacy_stage()
        self.assertNotIn('requested_mode_broker', loader.verify(self.stage, privileged=False))

    def test_mode_broker_feature_requires_exact_inventory(self):
        manifest_path = self.stage / loader.MANIFEST
        manifest = json.loads(manifest_path.read_text())
        manifest.pop('requested_mode_broker')
        manifest_path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(RuntimeError, 'Unexpected bundle file inventory'):
            loader.verify(self.stage, privileged=False)

    def test_new_loader_refuses_old_pool_before_any_operation(self):
        loader.POOL.mkdir()
        with mock.patch.object(loader.os, 'geteuid', return_value=0), \
                mock.patch.object(loader, 'verify', return_value=self.manifest), \
                mock.patch.object(loader, 'verify_loaded', return_value=True):
            with self.assertRaisesRegex(RuntimeError, 'lacks requested-mode support'):
                loader.operate(self.destination, 'start-pool')
        self.commands.assert_not_called()

    @unittest.skipUnless(os.geteuid() == 0, 'root-owned feature attribute fixture')
    def test_requested_mode_features_require_all_four_bounded_attributes(self):
        for number in range(1, 5):
            path = loader.POOL / 'connectors' / f'Virtual-{number}' / 'requested_mode'
            path.parent.mkdir(parents=True)
            path.write_text('0 0 0\n')
            path.chmod(0o644)
        loader.verify_pool_features(self.manifest, require_pool=True)
        path.write_text('3033 1891 119880\n')
        loader.verify_pool_features(self.manifest, require_pool=True)
        path.write_text('8193 1891 119880\n')
        with self.assertRaisesRegex(RuntimeError, 'invalid requested mode'):
            loader.verify_pool_features(self.manifest, require_pool=True)
        path.unlink()
        with self.assertRaisesRegex(RuntimeError, 'lacks requested-mode support'):
            loader.verify_pool_features(self.manifest, require_pool=True)

    def test_failed_manual_stop_does_not_quiesce_in_use_display(self):
        with mock.patch.object(loader.os, 'geteuid', return_value=0), \
                mock.patch.object(loader, 'verify', return_value=self.manifest), \
                mock.patch.object(loader, 'verify_loaded', return_value=True), \
                mock.patch.object(loader, 'system_stopping', return_value=False), \
                mock.patch.object(loader, 'require_idle', side_effect=RuntimeError('in use')):
            with self.assertRaisesRegex(RuntimeError, 'in use'):
                loader.operate(self.destination, 'stop-pool')
            loader.operate(self.destination, 'quiesce')
        self.commands.assert_not_called()

    @unittest.skipUnless(os.geteuid() == 0, 'root-owned install fixture; run inside a disposable container')
    def test_install_repairs_umask_and_is_dormant_idempotent(self):
        previous = os.umask(0o077)
        try:
            installer.install_root(argparse.Namespace(stage=self.stage))
            installer.install_root(argparse.Namespace(stage=self.stage))
        finally:
            os.umask(previous)
        self.assertEqual(stat.S_IMODE(self.install_parent.stat().st_mode), 0o755)
        self.assertEqual(stat.S_IMODE(self.destination.stat().st_mode), 0o755)
        self.assertEqual(stat.S_IMODE((self.destination / loader.MANIFEST).stat().st_mode), 0o644)
        self.assertEqual((self.install_parent / 'current').resolve(), self.destination)
        for name in loader.UNIT_NAMES:
            self.assertEqual(stat.S_IMODE((self.units / name).stat().st_mode), 0o644)
        self.assertTrue(all(call.args[0] == ['/usr/bin/systemctl', 'daemon-reload']
                            for call in self.commands.call_args_list))

    @unittest.skipUnless(os.geteuid() == 0, 'root-owned install fixture')
    def test_conflicting_unit_blocks_install_before_publishing(self):
        (self.units / 'vibeshine-vkms.service').write_text('administrator unit')
        with self.assertRaisesRegex(RuntimeError, 'Another installation'):
            installer.install_root(argparse.Namespace(stage=self.stage))
        self.assertFalse(self.destination.exists())
        self.assertEqual((self.units / 'vibeshine-vkms.service').read_text(), 'administrator unit')

    @unittest.skipUnless(os.geteuid() == 0, 'root-owned install fixture')
    def test_daemon_reload_failure_rolls_back_owned_units(self):
        self.commands.side_effect = RuntimeError('fixture reload failure')
        with mock.patch.object(installer.subprocess, 'run'):
            with self.assertRaisesRegex(RuntimeError, 'fixture reload failure'):
                installer.install_root(argparse.Namespace(stage=self.stage))
        self.assertFalse(any(self.units.iterdir()))
        self.assertFalse((self.install_parent / 'current').exists())
        self.assertTrue(self.destination.exists(), 'immutable candidate retained for a safe retry')

    @unittest.skipUnless(os.geteuid() == 0, 'root-owned upgrade fixture')
    def test_loaded_legacy_upgrade_and_boot_rollback_never_reload_or_restart(self):
        self.make_legacy_stage()
        installer.install_root(argparse.Namespace(stage=self.stage))
        old_bytes = {name: (self.units / name).read_bytes() for name in loader.UNIT_NAMES}
        self.mark_loaded(self.manifest)
        stage, destination, _ = self.stage_upgrade()
        self.commands.reset_mock()
        installer.install_root(argparse.Namespace(stage=stage))
        self.commands.assert_not_called()
        self.assertEqual((self.install_parent / 'current').resolve(), destination)
        loader.verify(self.destination)
        self.assertEqual((loader.MODULE_SYSFS / 'srcversion').read_text(), self.manifest['module_srcversion'])
        self.assertEqual((loader.MODULE_SYSFS / 'refcnt').read_text(), '7')
        self.assertTrue(loader.POOL.is_dir())
        for name in loader.UNIT_NAMES:
            self.assertEqual((self.units / name).read_bytes(), (destination / 'units' / name).read_bytes())
        installer.operate_root(argparse.Namespace(installed=destination, action='enable-root'))
        self.commands.assert_called_once_with(['/usr/bin/systemctl', '--no-reload', 'enable', 'vibeshine-vkms.service'])
        self.commands.reset_mock()
        installer.operate_root(argparse.Namespace(installed=self.destination, action='select-root'))
        self.commands.assert_not_called()
        self.assertEqual((self.install_parent / 'current').resolve(), self.destination)
        self.assertTrue(destination.is_dir())
        for name in loader.UNIT_NAMES:
            self.assertEqual((self.units / name).read_bytes(), old_bytes[name])

    @unittest.skipUnless(os.geteuid() == 0, 'root-owned upgrade fixture')
    def test_upgrade_rejects_administrator_changed_units(self):
        installer.install_root(argparse.Namespace(stage=self.stage))
        stage, destination, _ = self.stage_upgrade()
        (self.units / 'vibeshine-vkms.service').write_text('administrator change')
        with self.assertRaisesRegex(RuntimeError, 'Another installation'):
            installer.install_root(argparse.Namespace(stage=stage))
        self.assertFalse(destination.exists())
        self.assertEqual((self.install_parent / 'current').resolve(), self.destination)

    @unittest.skipUnless(os.geteuid() == 0, 'root-owned upgrade fixture')
    def test_upgrade_publish_failure_restores_selection_and_units(self):
        installer.install_root(argparse.Namespace(stage=self.stage))
        stage, _, _ = self.stage_upgrade()
        old_bytes = {name: (self.units / name).read_bytes() for name in loader.UNIT_NAMES}
        self.mark_loaded(self.manifest)
        original = installer.write_atomic
        calls = 0
        def fail_second_write(*args):
            nonlocal calls
            calls += 1
            if calls == 2:
                raise OSError('fixture publish failure')
            return original(*args)
        with mock.patch.object(installer, 'write_atomic', side_effect=fail_second_write):
            with self.assertRaisesRegex(OSError, 'fixture publish failure'):
                installer.install_root(argparse.Namespace(stage=stage))
        self.assertEqual((self.install_parent / 'current').resolve(), self.destination)
        for name in loader.UNIT_NAMES:
            self.assertEqual((self.units / name).read_bytes(), old_bytes[name])

    @unittest.skipUnless(os.geteuid() == 0, 'root-owned upgrade fixture')
    def test_start_refuses_pending_module_and_shutdown_uses_retained_release(self):
        self.make_legacy_stage()
        installer.install_root(argparse.Namespace(stage=self.stage))
        self.mark_loaded(self.manifest)
        stage, destination, _ = self.stage_upgrade()
        installer.install_root(argparse.Namespace(stage=stage))
        self.commands.reset_mock()
        with self.assertRaisesRegex(RuntimeError, 'different private-display module'):
            installer.operate_root(argparse.Namespace(installed=destination, action='start-root'))
        self.commands.assert_not_called()
        with mock.patch.object(loader, 'system_stopping', return_value=True):
            loader.operate(destination, 'stop-pool')
            self.commands.assert_not_called()
            loader.operate(destination, 'quiesce')
        self.commands.assert_called_once_with(['/usr/bin/bash', str(self.destination / 'libexec/vibeshine-vkms-quiesce')])
        self.assertTrue(loader.POOL.exists())
        with mock.patch.object(loader, 'system_stopping', return_value=False):
            self.commands.reset_mock()
            loader.operate(destination, 'quiesce')
            self.commands.assert_not_called()

    @unittest.skipUnless(os.geteuid() == 0, 'root-owned install fixture')
    def test_loaded_module_prevents_uninstall_and_idle_candidate_can_be_removed(self):
        installer.install_root(argparse.Namespace(stage=self.stage))
        loader.MODULE_SYSFS.mkdir()
        arguments = argparse.Namespace(installed=self.destination, action='uninstall-root')
        with self.assertRaisesRegex(RuntimeError, 'Disable activation and reboot'):
            installer.operate_root(arguments)
        self.assertTrue(self.destination.exists())
        loader.MODULE_SYSFS.rmdir()
        with mock.patch.object(installer.subprocess, 'run', return_value=argparse.Namespace(returncode=3)):
            installer.operate_root(arguments)
        self.assertFalse(self.destination.exists())
        self.assertFalse((self.install_parent / 'current').is_symlink())
        self.assertFalse(any(self.units.iterdir()))

    @unittest.skipUnless(os.geteuid() == 0, 'root-owned capability fixture')
    def test_capture_helper_has_private_mode_and_exact_file_capability(self):
        def command(arguments, **kwargs):
            if arguments[:2] == ['/usr/bin/systemctl', 'daemon-reload']:
                return argparse.Namespace(stdout='')
            return REAL_COMMAND(arguments, **kwargs)
        self.commands.side_effect = command
        stage = self.root / 'capture-stage'
        installer.stage(argparse.Namespace(module=self.module, peercred=self.peercred,
                                           driver_source=self.source, output=stage,
                                           user='deck', capture_helper=pathlib.Path('/usr/bin/true')))
        manifest = loader.verify(stage, privileged=False)
        destination = pathlib.Path(manifest['install_root'])
        installer.install_root(argparse.Namespace(stage=stage))
        helper = destination / loader.CAPTURE_HELPER
        self.assertEqual(stat.S_IMODE(helper.stat().st_mode), 0o750)
        self.assertEqual(helper.stat().st_uid, 0)
        self.assertEqual(helper.stat().st_gid, manifest['gid'])
        result = subprocess.run(['/usr/bin/getcap', '-n', str(helper)], check=True,
                                capture_output=True, text=True)
        self.assertEqual(result.stdout.strip(), str(helper) + ' cap_sys_admin=p')
        subprocess.run(['/usr/bin/setcap', 'cap_sys_admin=ep', str(helper)], check=True)
        with self.assertRaisesRegex(RuntimeError, 'exactly cap_sys_admin=p'):
            loader.verify(destination)


if __name__ == '__main__':
    unittest.main()
