import importlib.util
import json
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[4]
spec = importlib.util.spec_from_file_location('normalize', ROOT / 'packaging/linux/vibepollo-profile-normalize.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class LegacyProfile(unittest.TestCase):
    def test_both_hosts_and_machine_preserve_identity_and_paths(self):
        for selection in ('sunshine', 'vibeshine', 'machine-vibeshine'):
            with self.subTest(selection=selection), tempfile.TemporaryDirectory() as temporary:
                root = pathlib.Path(temporary)
                brand = 'sunshine' if selection == 'sunshine' else 'vibeshine'
                old = '/var/lib/vibeshine' if selection == 'machine-vibeshine' else '/home/deck/.config/' + brand
                (root / (brand + '.conf')).write_text(f'file_state = {old}/sunshine_state.json\nfile_apps = apps.json\n')
                state = {'root': {'uniqueid': 'identity', 'named_devices': []}, 'password': 'hash', 'salt': 'salt'}
                (root / 'sunshine_state.json').write_text(json.dumps(state))
                (root / 'apps.json').write_text(json.dumps({'apps': [{'name': 'Game', 'cmd': 'keep this command', 'image-path': old + '/covers/game.png'}]}))
                module.normalize(root, '/home/deck', selection)
                self.assertEqual(json.loads((root / 'sunshine_state.json').read_text()), state)
                self.assertIn('/var/lib/vibepollo/sunshine_state.json', (root / 'vibepollo.conf').read_text())
                app = json.loads((root / 'apps.json').read_text())['apps'][0]
                self.assertEqual(app['cmd'], 'keep this command')
                self.assertEqual(app['image-path'], '/var/lib/vibepollo/covers/game.png')
                self.assertTrue((root / (brand + '.conf')).exists())

    def test_external_state_is_refused_without_reading_it(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            (root / 'sunshine.conf').write_text('file_state = /etc/shadow\n')
            with self.assertRaisesRegex(ValueError, 'external file_state'):
                module.normalize(root, '/home/deck', 'sunshine')
            self.assertFalse((root / 'vibepollo.conf').exists())

    def test_ambiguous_configs_are_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            for brand in ('sunshine', 'vibeshine'):
                (root / (brand + '.conf')).touch()
            with self.assertRaisesRegex(ValueError, 'multiple host'):
                module.normalize(root, '/home/deck', 'auto')


if __name__ == '__main__':
    unittest.main()
