#!/usr/bin/env python3
"""Local pool deployment regressions; root integration cases use disposable paths."""
import argparse
import contextlib
import importlib.util
import io
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
                (installer, 'UNITS', self.units), (installer, 'VENDOR_UNITS', self.vendor)):
            self.stack.enter_context(mock.patch.object(obj, name, value))
        original_safe = loader.safe_directory
        def safe_fixture_directory(path):
            if path == self.root or self.root in path.parents:
                original_safe(path)
        self.stack.enter_context(mock.patch.object(loader, 'safe_directory', safe_fixture_directory))
        self.stack.enter_context(mock.patch.object(loader, 'modinfo', side_effect=lambda module, field: {
            'name': 'vibeshine_drm', 'version': '1.19.0', 'srcversion': '012345',
            'vermagic': loader.platform.release() + ' SMP', 'depends': ''}[field]))
        self.commands = self.stack.enter_context(mock.patch.object(loader, 'command'))
        self.stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
        installer.stage(argparse.Namespace(module=self.module, peercred=self.peercred,
                                           driver_source=self.source, output=self.stage,
                                           user='deck', capture_helper=None))
        self.manifest = loader.verify(self.stage, privileged=False)
        self.destination = pathlib.Path(self.manifest['install_root'])

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
