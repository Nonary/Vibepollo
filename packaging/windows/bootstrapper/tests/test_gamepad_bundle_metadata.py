"""Portable authoring contract; native MSI reader fixtures run on Windows."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[4]
WIX = '{http://schemas.microsoft.com/wix/2006/wi}'


class GamepadBundleMetadataTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.patch = ET.parse(ROOT / 'packaging/windows/wix/patch_custom_actions.wxs')
        cls.custom = ET.parse(ROOT / 'packaging/windows/wix/custom_actions.wxs')
        cls.source = (ROOT / 'packaging/windows/bootstrapper/VibeshineInstaller.cs').read_text()

    def test_actual_reader_resolves_authored_static_capability(self):
        # Discover the property's name from the production reader rather than
        # testing a duplicate definition or a fabricated PayloadMsiInfo object.
        name = re.search(r'var virtualGamepadDriverBundled = ReadMsiProperty\(packageHandle, "([^"]+)"\)', self.source).group(1)
        property_node = self.patch.find(f'.//Property[@Id="{name}"]')
        self.assertIsNotNone(property_node)
        self.assertEqual(property_node.get('Value'), '$(var.VhfGamepadDriverBundled)')
        self.assertNotEqual(name.upper(), name, 'private MSI capability cannot be a command-line property')
        self.assertEqual(len(property_node), 0, 'capability cannot depend on registry searches')
        for bundled in ('0', '1'):
            for selection in (None, '0', '1'):
                with self.subTest(bundled=bundled, selection=selection):
                    metadata = {node.get('Id'): node.get('Value', '') for node in self.patch.findall('.//Property')}
                    metadata['INSTALL_VIRTUAL_GAMEPAD_DRIVER'] = selection or ''
                    metadata[name] = metadata[name].replace('$(var.VhfGamepadDriverBundled)', bundled)
                    self.assertEqual(metadata[name] == '1', bundled == '1')
        for action in self.custom.findall(f'.//{WIX}CustomAction'):
            self.assertNotEqual(action.get('Property'), name, 'runtime actions must not rewrite capability')
        self.assertIn('HasBundledVirtualGamepadDriver = string.Equals(virtualGamepadDriverBundled, "1", StringComparison.Ordinal)', self.source)

    def test_selection_defaults_and_explicit_optouts_remain_independent(self):
        selection = self.patch.find('.//Property[@Id="INSTALL_VIRTUAL_GAMEPAD_DRIVER"]')
        self.assertNotIn('Value', selection.attrib)
        self.assertEqual(selection.get('Secure'), 'yes')
        default = self.custom.find(f'.//{WIX}CustomAction[@Id="DefaultGamepadDriverSelection"]')
        self.assertEqual(default.get('Value'), '$(var.VhfGamepadDriverBundled)')
        self.assertEqual(default.get('Property'), 'INSTALL_VIRTUAL_GAMEPAD_DRIVER')
        for action in ('RestoreSavedGamepadDriverSelection', 'DefaultGamepadDriverSelection'):
            condition = self.patch.find(f'.//InstallExecuteSequence/Custom[@Action="{action}"]').text
            self.assertIn('INSTALL_VIRTUAL_GAMEPAD_DRIVER=""', condition)

    def test_native_fixtures_are_in_the_existing_safety_suite(self):
        runner = (ROOT / 'packaging/windows/bootstrapper/tests/test_driver_selections.ps1').read_text()
        self.assertIn("'test_gamepad_bundle_metadata.ps1'", runner)
        safety = (ROOT / 'packaging/windows/test_driver_install_safety.ps1').read_text()
        self.assertIn("'packaging/windows/bootstrapper/tests/test_driver_selections.ps1'", safety)


if __name__ == '__main__':
    unittest.main()
