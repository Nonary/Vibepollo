import pathlib
import unittest
import xml.etree.ElementTree as ET


ROOT = pathlib.Path(__file__).resolve().parents[4]
BOOTSTRAPPER = ROOT / "packaging/windows/bootstrapper/VibeshineInstaller.cs"
CUSTOM_ACTIONS = ROOT / "packaging/windows/wix/custom_actions.wxs"
PATCH_ACTIONS = ROOT / "packaging/windows/wix/patch_custom_actions.wxs"


class InstallerPathSecurityContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = BOOTSTRAPPER.read_text(encoding="utf-8")
        cls.custom_root = ET.parse(CUSTOM_ACTIONS).getroot()
        cls.patch_root = ET.parse(PATCH_ACTIONS).getroot()

    def test_elevated_embedded_msi_uses_protected_random_directory(self):
        resolve = self.source.split("private static string ResolveMsiPath", 1)[1]
        resolve = resolve.split("private static string ValidateResolvedMsiPayload", 1)[0]
        self.assertIn('Contains("Payload.msi", StringComparer.Ordinal)', resolve)
        self.assertNotIn("catch", resolve)
        self.assertIn("return GetOrCreateSecureEmbeddedMsiDirectory();", self.source)
        self.assertIn("Guid.NewGuid().ToString(\"N\")", self.source)
        self.assertIn("directory.Create(BuildInstallerRecoveryDirectorySecurity())", self.source)
        self.assertIn("trustedParent.Attributes & FileAttributes.ReparsePoint", self.source)
        elevated_branch = self.source.split("private static string GetEmbeddedMsiExtractRoot()", 1)[1]
        elevated_branch = elevated_branch.split("private static IEnumerable<string>", 1)[0]
        self.assertNotIn('Path.Combine(programData, "Vibeshine", "InstallerCache")', elevated_branch)

    def test_msi_is_reverified_at_process_boundary(self):
        run_msiexec = self.source.split("private static int RunMsiexec(", 1)[1]
        run_msiexec = run_msiexec.split("private static int RunProcess(", 1)[0]
        self.assertIn("ValidateMsiPayloadImmediatelyBeforeExecution(arguments);", run_msiexec)
        self.assertIn("FileHashMatches(normalized, expectedHash)", self.source)
        self.assertIn("ShouldRequireSignedMsiPayload()", self.source)
        self.assertIn("VerifyAuthenticodeSignature(normalized)", self.source)
        self.assertIn("AuthorizedDerivedMsiPayloadHashes", self.source)
        self.assertIn("RegisterAuthorizedDerivedMsiPayload(workingMsiPath", self.source)
        self.assertIn("FileHashMatches(normalized, authorizedDerivedHash)", self.source)
        self.assertIn("!IsActiveInstallerRecoveryDirectory(normalizedDirectory)", self.source)
        self.assertIn('Path.GetFileName(normalized), "firewall_cleanup.msi"', self.source)
        self.assertIn("The derived MSI did not retain the authorized ProductCode", self.source)
        derived_branch = self.source.split('if (!string.IsNullOrWhiteSpace(authorizedDerivedHash))', 1)[1]
        derived_branch = derived_branch.split("if (ShouldRequireSignedMsiPayload()", 1)[0]
        self.assertIn("return;", derived_branch)

    def test_bootstrapper_rejects_unsafe_install_roots_for_ui_and_cli(self):
        validate = self.source.split("internal static string ValidateInstallRootPath", 1)[1]
        validate = validate.split("private static bool IsRegisteredInstallRoot", 1)[0]
        self.assertIn("A filesystem root cannot be used", validate)
        self.assertIn("must be a child folder beneath Program Files", validate)
        self.assertIn("programFilesRoots.Any", validate)
        self.assertIn("PathIsUnderDirectory(fullPath, root)", validate)
        self.assertNotIn("RejectUntrustedWritableInstallAncestor", validate)
        self.assertIn("FileAttributes.ReparsePoint", validate)
        self.assertNotIn("pending.Push", validate)
        self.assertNotIn("File.GetAttributes(entry)", validate)
        self.assertIn("Directory.EnumerateFileSystemEntries(fullPath).Any()", validate)
        self.assertIn("!IsRegisteredInstallRoot(fullPath)", validate)
        self.assertIn("TryNormalizeAndValidateCliInstallRoot(cliArgs", self.source)
        self.assertIn("installDirectory = ValidateInstallRootPath(installDirectory);", self.source)

    def test_users_read_execute_does_not_overlap_mutation_mask(self):
        # FileSystemRights values from the Windows access-mask contract. RX has
        # read/execute/read-permissions/synchronize only; Modify includes writes.
        users_read_execute = 0x1200A9
        write_data = 0x000002
        modify = 0x1301BF
        primitive_mutation_mask = 0x0D0156
        self.assertEqual(0, users_read_execute & primitive_mutation_mask)
        self.assertNotEqual(0, write_data & primitive_mutation_mask)
        self.assertNotEqual(0, modify & primitive_mutation_mask)

    def test_install_root_filesystem_scan_runs_after_elevation(self):
        normalize_install = self.source.split("private static string NormalizeInstallPath", 1)[1]
        normalize_install = normalize_install.split("private static string CheckDiskSpace", 1)[0]
        self.assertNotIn("ValidateInstallRootPath", normalize_install)

        interactive = self.source.split("public static InstallerResult RunInteractiveInstall(", 1)[1]
        interactive = interactive.split("SweepStaleInstallerRecoveryDirectories();", 1)[0]
        self.assertLess(interactive.index("RunElevatedBootstrapperInstall("),
                        interactive.index("ValidateInstallRootPath(installDirectory)"))

        cli = self.source.split("public static InstallerResult RunCli(", 1)[1]
        cli = cli.split("if (!HasRestartBehavior(cliArgs))", 1)[0]
        self.assertLess(cli.index("return RunElevatedBootstrapperCli(arguments, cliArgs);"),
                        cli.index("TryNormalizeAndValidateCliInstallRoot(cliArgs"))

    def test_direct_msi_uses_native_program_files_policy_without_shell_interpolation(self):

        actions = {
            node.attrib.get("Id"): node
            for node in self.custom_root.iter()
            if node.tag.endswith("CustomAction") and node.attrib.get("Id")
        }
        self.assertIn("BlockUnsafeInstallRoot", actions)
        self.assertNotIn("Value", actions["BlockUnsafeInstallRoot"].attrib)
        self.assertEqual("InstallRootGuard", actions["ValidateInstallRoot"].get("BinaryKey"))
        self.assertEqual("ValidateInstallRoot", actions["ValidateInstallRoot"].get("DllEntry"))
        for removed in (
            "SetValidateInstallRoot", "SecureInstallRoot",
            "SetSecureInstallRoot", "ValidateInstalledPayload", "SetValidateInstalledPayload",
        ):
            self.assertNotIn(removed, actions)
        wix_source = CUSTOM_ACTIONS.read_text(encoding="utf-8")
        self.assertNotIn("/T", wix_source)
        self.assertNotIn("/reset", wix_source)

        scheduled = {
            node.attrib.get("Action"): node
            for node in self.patch_root.iter()
            if node.tag == "Custom" and node.attrib.get("Action")
        }
        policy = scheduled["BlockUnsafeInstallRoot"]
        self.assertEqual("CostFinalize", policy.attrib.get("After"))
        self.assertIn("INSTALL_ROOT ~<< ProgramFilesFolder", policy.text)
        self.assertIn("INSTALL_ROOT ~<< ProgramFiles64Folder", policy.text)
        self.assertIn("INSTALL_ROOT ~= ProgramFilesFolder", policy.text)
        self.assertIn("INSTALL_ROOT ~= ProgramFiles64Folder", policy.text)
        self.assertEqual("InstallFiles", scheduled["SetInstallSudovda"].attrib.get("After"))


if __name__ == "__main__":
    unittest.main()
