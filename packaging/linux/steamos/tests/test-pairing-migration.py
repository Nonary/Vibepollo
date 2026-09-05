#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
import copy
import importlib.util
import json
import pathlib
import ssl
import subprocess
import tempfile
import unittest

MODULE = pathlib.Path(__file__).resolve().parents[1] / 'local/pairing_migration.py'
SPEC = importlib.util.spec_from_file_location('pairing_migration', MODULE)
migration = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(migration)


class PairingMigrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = tempfile.TemporaryDirectory(prefix='vibeshine-pairing-cert-')
        root = pathlib.Path(cls.fixture.name)
        cls.certificates = []
        for index in range(2):
            cert = root / f'{index}.pem'
            subprocess.run([
                'openssl', 'req', '-x509', '-newkey', 'ec',
                '-pkeyopt', 'ec_paramgen_curve:prime256v1', '-nodes', '-days', '1',
                '-subj', f'/CN=MigrationFixture{index}',
                '-keyout', str(root / f'{index}.key'), '-out', str(cert),
            ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            cls.certificates.append(cert.read_text())

    @classmethod
    def tearDownClass(cls):
        cls.fixture.cleanup()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='vibeshine-pairing-migration-')
        self.addCleanup(self.temporary.cleanup)
        self.path = pathlib.Path(self.temporary.name) / 'sunshine_state.json'
        self.state = {
            'root': {
                'uniqueid': 'existing-host-identity',
                'named_devices': [self.device(0), self.device(1)],
            },
            'username': 'fixture', 'password': 'fixture-hash', 'salt': 'fixture-salt',
        }

    def device(self, index):
        return {
            'name': f'Client {index}',
            'uuid': f'00000000-0000-0000-0000-{index:012d}',
            'cert': self.certificates[index],
            'enabled': True,
        }

    def write(self):
        self.path.write_text(json.dumps(self.state))
        self.original = self.path.read_bytes()

    def add_alias(self, **overrides):
        alias = copy.deepcopy(self.state['root']['named_devices'][0])
        alias.update(name='Legacy alias', uuid='00000000-0000-0000-0000-000000000099', **overrides)
        self.state['root']['named_devices'].append(alias)
        return alias

    def test_distinct_clients_remain_byte_identical(self):
        self.write()
        self.assertEqual(migration.normalize_pairing_state(self.path), 0)
        self.assertEqual(self.path.read_bytes(), self.original)

    def test_alias_keeps_first_uuid_name_host_identity_and_credentials(self):
        expected = copy.deepcopy(self.state)
        self.add_alias()
        self.write()
        self.assertEqual(migration.normalize_pairing_state(self.path), 1)
        self.assertEqual(json.loads(self.path.read_text()), expected)
        self.assertEqual(migration.normalize_pairing_state(self.path), 0)

    def test_pem_formatting_uses_same_certificate_identity(self):
        alias = self.add_alias()
        certificate = ssl.PEM_cert_to_DER_cert(alias['cert'])
        alias['cert'] = ssl.DER_cert_to_PEM_cert(certificate).replace('\n', '\r\n')
        self.write()
        self.assertEqual(migration.normalize_pairing_state(self.path), 1)

    def test_permission_conflict_preserves_input(self):
        self.add_alias(enabled=False)
        self.write()
        with self.assertRaisesRegex(ValueError, 'conflicting permissions'):
            migration.normalize_pairing_state(self.path)
        self.assertEqual(self.path.read_bytes(), self.original)

    def test_config_conflict_preserves_input(self):
        self.add_alias(config_overrides={'encoder': 'software'})
        self.write()
        with self.assertRaisesRegex(ValueError, 'conflicting permissions'):
            migration.normalize_pairing_state(self.path)
        self.assertEqual(self.path.read_bytes(), self.original)

    def test_identical_settings_can_be_consolidated(self):
        self.state['root']['named_devices'][0]['config_overrides'] = {'encoder': 'vaapi'}
        self.add_alias()
        self.write()
        self.assertEqual(migration.normalize_pairing_state(self.path), 1)

    def test_invalid_certificate_preserves_input(self):
        self.state['root']['named_devices'][0]['cert'] = 'broken'
        self.write()
        with self.assertRaisesRegex(ValueError, 'invalid client certificate'):
            migration.normalize_pairing_state(self.path)
        self.assertEqual(self.path.read_bytes(), self.original)

    def test_legacy_unnamed_devices_are_unchanged(self):
        del self.state['root']['named_devices']
        self.state['root']['devices'] = [{'certs': self.certificates}]
        self.write()
        self.assertEqual(migration.normalize_pairing_state(self.path), 0)
        self.assertEqual(self.path.read_bytes(), self.original)


if __name__ == '__main__':
    unittest.main()
