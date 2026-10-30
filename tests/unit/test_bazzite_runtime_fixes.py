"""Local regression checks; no host services, keys or drivers are changed."""
import errno
import importlib.util
from pathlib import Path
import re
import stat
import subprocess
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


def load(name, relative):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


tray = load('bazzite_tray', 'packaging/linux/vibepollo-tray.py')
deploy = load('bazzite_deploy', 'scripts/linux_local_deploy.py')
drivers = load('bazzite_drivers', 'packaging/linux/bazzite/drivers/build-drivers.py')


class RuntimeFixes(unittest.TestCase):
    def test_only_trusted_home_alias_is_translated(self):
        path = Path('/home/Jay/.config')
        for owner, mode, target, expected in [
            (0, stat.S_IFLNK, '/var/home', Path('/var/home/Jay/.config')),
            (1000, stat.S_IFLNK, '/var/home', path),
            (0, stat.S_IFDIR, '/var/home', path),
            (0, stat.S_IFLNK, '/elsewhere', path),
        ]:
            with self.subTest(owner=owner, mode=mode, target=target):
                with patch.object(tray.os, 'lstat', return_value=SimpleNamespace(st_uid=owner, st_mode=mode)), \
                     patch.object(tray.os.path, 'realpath', return_value=target):
                    self.assertEqual(tray.canonical_config_root(path), expected)
        with patch.object(tray.os, 'lstat', side_effect=AssertionError('unrelated path probed')):
            self.assertEqual(tray.canonical_config_root('/home-other/Jay'), Path('/home-other/Jay'))

    def test_matching_readonly_xattr_is_not_written(self):
        with patch.object(deploy.os, 'getxattr', return_value=b'label'), \
             patch.object(deploy.os, 'setxattr', side_effect=OSError(errno.ENOTSUP, 'readonly')) as write:
            deploy.set_xattr_if_changed('/unused', 'security.selinux', b'label')
            write.assert_not_called()

    def test_missing_or_changed_xattr_still_requires_write(self):
        for value in (b'old', OSError(errno.ENODATA, 'absent')):
            options = {'side_effect': value} if isinstance(value, Exception) else {'return_value': value}
            with patch.object(deploy.os, 'getxattr', **options), \
                 patch.object(deploy.os, 'setxattr') as write:
                deploy.set_xattr_if_changed('/unused', 'user.test', b'new')
                write.assert_called_once_with('/unused', 'user.test', b'new')
        with patch.object(deploy.os, 'getxattr', return_value=b'old'), \
             patch.object(deploy.os, 'setxattr', side_effect=OSError(errno.EPERM, 'denied')):
            with self.assertRaises(OSError):
                deploy.set_xattr_if_changed('/unused', 'security.selinux', b'new')

    def test_xattr_read_errors_are_not_hidden(self):
        with patch.object(deploy.os, 'getxattr', side_effect=OSError(errno.EPERM, 'denied')), \
             patch.object(deploy.os, 'setxattr') as write:
            with self.assertRaises(OSError):
                deploy.set_xattr_if_changed('/unused', 'security.selinux', b'new')
            write.assert_not_called()

    def test_account_validators_accept_case_without_accepting_metacharacters(self):
        for filename in ('vibepollo-machine-host', 'vibepollo-session-controller'):
            source = (ROOT / 'packaging/linux' / filename).read_text()
            function = re.search(r'^valid_user\(\).*$', source, re.M).group()
            for name, valid in [('Jay', True), ('jay', True), ('_User-2', True),
                                ('', False), ('-Jay', False), ('Jay/foo', False),
                                ('Jay user', False), ('Jay;id', False)]:
                result = subprocess.run(['bash', '-c', function + '\nvalid_user "$1"', '_', name])
                self.assertEqual(result.returncode == 0, valid, (filename, name))

    def test_driver_spec_preserves_signatures_and_indexes_exact_kernel(self):
        dependency = 'kernel-core = 7.2.11-ogc4.1.fc44'
        spec = drivers.make_spec(drivers.KERNEL, [], dependency)
        self.assertIn('Requires: ' + dependency, spec)
        self.assertIn('%global __os_install_post %{nil}', spec)
        self.assertIn('%posttrans\n/usr/sbin/depmod -a ' + drivers.KERNEL, spec)
        self.assertNotIn('systemctl', spec)


if __name__ == '__main__':
    unittest.main()
