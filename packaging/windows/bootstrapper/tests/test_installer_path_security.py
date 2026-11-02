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

    def test_bootstrapper_rejects_only_unusable_install_roots_for_ui_and_cli(self):
        validate = self.source.split("internal static string ValidateInstallRootPath", 1)[1]
        validate = validate.split("private static bool IsRegisteredInstallRoot", 1)[0]
        self.assertIn("A filesystem root cannot be used", validate)
        self.assertNotIn("programFilesRoots", validate)
        self.assertNotIn("RejectUntrustedWritableInstallAncestor", validate)
        self.assertIn("Directory.EnumerateFileSystemEntries(fullPath).Any()", validate)
        self.assertIn("!IsRegisteredInstallRoot(fullPath)", validate)
        self.assertIn("!IsProductFolder(fullPath)", validate)
        self.assertIn("TryNormalizeAndValidateCliInstallRoot(cliArgs", self.source)
        self.assertIn("installDirectory = ValidateInstallRootPath(installDirectory);", self.source)

    def test_managed_validation_leaves_links_and_tree_scan_to_native_guard(self):
        validate = self.source.split("internal static string ValidateInstallRootPath", 1)[1]
        validate = validate.split("private static bool IsRegisteredInstallRoot", 1)[0]
        # A recursive scan here would fail on unreadable entries and harmless links.
        self.assertNotIn("ReparsePoint", validate)
        self.assertNotIn("new Stack<string>", validate)
        self.assertNotIn("File.GetAttributes", validate)
        self.assertIn("catch (UnauthorizedAccessException)", validate)
        self.assertIn("IndexOfAny(new[] { '%', ';' })", validate)  # other characters are quoted
        self.assertIn("catch (IOException)", validate)
        # An uninstall or failed upgrade must never strand the product's own folder.
        product = self.source.split("private static bool IsProductFolder(string installRoot)", 1)[1]
        product = product.split("public static bool IsSunshineVirtualDisplayDriverEnabledInConfiguration", 1)[0]
        for name in ("Sunshine", "Vibeshine", "Apollo", "Vibepollo", "sunshine.exe", "sunshine.conf"):
            self.assertIn(name, product)
        # A matching name alone must not make someone's D:\\Photos\\Sunshine count as ours.
        self.assertIn("PathIsUnderDirectory(installRoot, root)", product)
        migration = (ROOT / "src_assets/windows/misc/migration/migrate-config.bat").read_text(encoding="utf-8")
        self.assertIn("Join-Path $env:NEW_DIR 'apps.json'", migration)
        self.assertNotIn("'%NEW_DIR%", migration)

    def test_old_guard_obstacles_clear_after_preflight_and_before_any_removal(self):
        for start, end in (("public static InstallerResult RunInteractiveInstall(", "private static bool IsCertificateThumbprint("),
                           ("public static InstallerResult RunCli(", "private static void PreserveCliInstallDirectory(")):
            body = self.source.split(start, 1)[1].split(end, 1)[0]
            clear = body.index("ClearPreviousGuardObstacles(preflight.LogPath);")
            self.assertLess(body.index("if (!preflight.Succeeded) return preflight;"), clear)
            for effect in ("UninstallCompetingProducts(", "TryPreUninstallDowngradeSourceVersion("):
                self.assertLess(clear, body.index(effect))
        # Removal paths reach the old guard too.
        uninstall = self.source.split("public static InstallerResult RunInteractiveUninstall(", 1)[1]
        self.assertLess(uninstall.index("ClearPreviousGuardObstacles("), uninstall.index("UninstallInstalledProducts("))
        cli = self.source.split("public static InstallerResult RunCli(", 1)[1]
        self.assertIn("if (isUninstallOperation) ClearPreviousGuardObstacles(", cli)
        cleanup = self.source.split("private static void ClearPreviousGuardObstacles(string logPath)", 1)[1]
        cleanup = cleanup.split("private static bool SatisfyPreviousGuard", 1)[0]
        # Only builds that carry the old guard, never a link, and a failure never stops the install.
        self.assertIn("product.Version.Build >= 200 && product.Version.Build <= 590", cleanup)
        self.assertIn("!product.IsPerUser", cleanup)
        self.assertIn("FileAttributes.ReparsePoint", cleanup)
        # Saving a folder updates its children, so hardlinks are settled before the first write.
        self.assertLess(cleanup.index("HasOtherHardLinks(entry)"), cleanup.index("SatisfyPreviousGuard(read, write"))
        self.assertIn("catch (Exception ex)", cleanup)
        self.assertLess(cleanup.index("the install folder is a link"), cleanup.index("new Stack<string>"))
        satisfy = self.source.split("private static bool SatisfyPreviousGuard", 1)[1].split("public static InstallerResult RunInteractiveInstall", 1)[0]
        # Inherited entries only become editable after protection is saved and re-read.
        protect = satisfy.index("SetAccessRuleProtection(true, true)")
        self.assertLess(protect, satisfy.index("write(security);", protect))
        self.assertLess(satisfy.index("write(security);", protect), satisfy.index("security = read();", protect))
        self.assertIn("RemoveAccessRuleSpecific(rule)", satisfy)
        self.assertNotIn("PurgeAccessRules", satisfy)  # would also drop deny entries
        self.assertIn("WellKnownSidType.LocalSystemSid", satisfy)
        migration = (ROOT / "src_assets/windows/misc/migration/migrate-config.bat").read_text(encoding="utf-8")
        self.assertNotIn("*.txt", migration)
        self.assertNotIn("*.log", migration)

    def test_failed_guard_never_triggers_a_separate_uninstall(self):
        # The log cannot prove which package's guard failed, so recovery must not act on it.
        predicate = self.source.split("private static bool IsRecoverableMsiFirewallCleanupFailure", 1)[1].split("private const string", 1)[0]
        self.assertIn("MsiFirewallExceptionUninstallFailureLogMarker", predicate)
        self.assertNotIn("ValidateInstallRoot", predicate)

    def test_replacing_old_products_does_not_stop_before_trying(self):
        competing = self.source.split("private static InstallerResult UninstallCompetingProducts(", 1)[1]
        before = competing.split("var finalCode = 0;", 1)[0]
        self.assertNotIn("1612", before)
        # User-writable commands never run, even though they no longer stop the install.
        self.assertIn("installedProducts.RemoveAll(product => product.IsPerUser);", before)
        self.assertIn("installedProducts.RemoveAll(mismatched.Contains);", before)
        dispatch = competing.split("var finalCode = 0;", 1)[1]
        self.assertLess(dispatch.index("FindKnownMsiSourceRelease(product.ProductCode) != null"), dispatch.index("CanUninstallProduct(product)"))
        self.assertNotIn("Multiple live MSI products", self.source)

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

    def test_direct_msi_uses_native_guard_without_shell_interpolation(self):

        actions = {
            node.attrib.get("Id"): node
            for node in self.custom_root.iter()
            if node.tag.endswith("CustomAction") and node.attrib.get("Id")
        }
        self.assertNotIn("BlockUnsafeInstallRoot", actions)
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
        self.assertNotIn("BlockUnsafeInstallRoot", scheduled)
        self.assertEqual("InstallFiles", scheduled["SetInstallSudovda"].attrib.get("After"))


if __name__ == "__main__":
    unittest.main()
