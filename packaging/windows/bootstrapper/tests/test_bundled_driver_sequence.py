"""Keep shared driver setup inside Vibepollo's established MSI behavior."""
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[4]


class BundledDriverSequence(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.actions = {node.get("Id"): node for node in ET.parse(
            ROOT / "packaging/windows/wix/custom_actions.wxs").iter()
            if node.tag.endswith("CustomAction")}
        cls.sequence = {node.get("Action"): node for node in ET.parse(
            ROOT / "packaging/windows/wix/patch_custom_actions.wxs").iter("Custom")}

    def test_single_coordinator_follows_existing_acl_housekeeping(self):
        self.assertEqual("InstallFiles", self.sequence["SetResetAcls"].get("After"))
        self.assertEqual("SetResetAcls", self.sequence["ResetAcls"].get("After"))
        self.assertEqual("ResetAcls", self.sequence["SetInstallBundledDrivers"].get("After"))
        self.assertEqual("SetInstallBundledDrivers", self.sequence["InstallBundledDrivers"].get("After"))
        self.assertEqual("InstallBundledDrivers", self.sequence["SetInstallUsbIpTransport"].get("After"))
        for retired in ("InstallSudovda", "InstallVirtualDisplayDriver", "InstallVirtualGamepadDriver"):
            self.assertNotIn(retired, self.sequence)
            self.assertNotIn("Set" + retired, self.sequence)

    def test_downstream_optional_failures_remain_nonfatal(self):
        for name in ("ResetAcls", "InstallSudovda", "InstallVirtualDisplayDriver",
                     "InstallVirtualGamepadDriver", "InstallBundledDrivers", "RegisterVulkanHdrLayer"):
            self.assertEqual("ignore", self.actions[name].get("Return"), name)
        for name in ("SetRegisterVulkanHdrLayer", "RegisterVulkanHdrLayer"):
            self.assertEqual("NOT REMOVE", self.sequence[name].text)
        for name in ("SetMigrateConfig", "MigrateConfig", "SetRunInstallerMigrations", "RunInstallerMigrations"):
            self.assertEqual("NOT Installed AND NOT REMOVE", self.sequence[name].text)

    def test_dispatcher_preserves_selection_and_explicit_consent(self):
        command = self.actions["SetInstallBundledDrivers"].get("Value")
        self.assertTrue(command.startswith('"[System64Folder]WindowsPowerShell\\v1.0\\powershell.exe" '))
        self.assertIn('-File "[INSTALL_ROOT]drivers\\install-bundled-drivers.ps1"', command)
        self.assertNotIn("cmd.exe", command.lower())
        self.assertNotIn("-command", command.lower())
        for property_name in ("INSTALL_DISPLAY_DRIVERS", "INSTALL_SUDOVDA", "INSTALL_VIRTUAL_DISPLAY_DRIVER",
                              "INSTALL_VIRTUAL_GAMEPAD_DRIVER", "ALLOW_LOCAL_GAMEPAD_CERTIFICATE_TRUST",
                              "LOCAL_GAMEPAD_CERTIFICATE_THUMBPRINT"):
            self.assertIn("[" + property_name + "]", command)

    def test_payload_verification_uses_the_downstream_msi_root(self):
        workflow = (ROOT / ".github/workflows/ci-windows.yml").read_text()
        self.assertIn('Apollo/drivers/$scriptName', workflow)
        self.assertNotIn('Sunshine/drivers/$scriptName', workflow)


if __name__ == "__main__":
    unittest.main()
