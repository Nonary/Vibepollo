/**
 * @file tests/integration/test_virtual_display_packaging.cpp
 * @brief Tests for Vibepollo Display Driver packaging invariants.
 */
#include "../tests_common.h"

#ifdef _WIN32

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

namespace {
  std::string read_source_file(const std::filesystem::path &relative_path) {
    const auto path = std::filesystem::path {SUNSHINE_SOURCE_DIR} / relative_path;
    std::ifstream file {path, std::ios::binary};
    if (!file) {
      ADD_FAILURE() << "Failed to open " << path.string();
      return {};
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

  void expect_contains(const std::string &content, const std::string &needle) {
    EXPECT_NE(content.find(needle), std::string::npos) << "missing: " << needle;
  }
}  // namespace

TEST(SunshineVirtualDisplayPackaging, PackageTargetRefreshesDriverAssetsFromSource) {
  const auto cmake = read_source_file("cmake/packaging/windows.cmake");

  expect_contains(
    cmake,
    "add_dependencies(package_msi refresh_sunshine_virtual_display_driver_assets)"
  );
  EXPECT_EQ(
    cmake.find("add_dependencies(package_msi validate_sunshine_virtual_display_driver_assets)"),
    std::string::npos
  );
}

TEST(SunshineVirtualDisplayPackaging, VulkanHdrLayerPayloadIsRequiredAndInstalled) {
  const auto cmake = read_source_file("cmake/packaging/windows.cmake");
  const auto layer_json = read_source_file("src_assets/windows/drivers/sunshine/vulkan-layer/VkLayer_sunshine_hdr.json");
  const auto layer_dll_path = std::filesystem::path {SUNSHINE_SOURCE_DIR} /
                              "src_assets/windows/drivers/sunshine/vulkan-layer/VkLayer_sunshine_hdr.dll";

  expect_contains(cmake, "SUNSHINE_VIRTUAL_DISPLAY_VULKAN_LAYER_FILES");
  expect_contains(cmake, "SUNSHINE_VIRTUAL_DISPLAY_PACKAGE_FILES");
  expect_contains(cmake, "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}/vulkan-layer/VkLayer_sunshine_hdr.dll");
  expect_contains(cmake, "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}/vulkan-layer/VkLayer_sunshine_hdr.json");
  expect_contains(cmake, "foreach(_sunshine_driver_file IN LISTS SUNSHINE_VIRTUAL_DISPLAY_PACKAGE_FILES)");
  expect_contains(cmake, "install(FILES ${SUNSHINE_VIRTUAL_DISPLAY_VULKAN_LAYER_FILES}");
  expect_contains(layer_json, "\"name\": \"VK_LAYER_SUNSHINE_virtual_hdr\"");
  expect_contains(layer_json, "\"library_path\": \".\\\\VkLayer_sunshine_hdr.dll\"");
  const auto manifest = nlohmann::json::parse(layer_json);
  ASSERT_TRUE(manifest.contains("layer"));
  EXPECT_FALSE(manifest["layer"].contains("enable_environment"));
  EXPECT_EQ(manifest["layer"]["disable_environment"]["DISABLE_SUNSHINE_VIRTUAL_HDR"], "1");
  ASSERT_TRUE(std::filesystem::exists(layer_dll_path)) << layer_dll_path.string();
  EXPECT_GT(std::filesystem::file_size(layer_dll_path), 0u);
}

TEST(SunshineVirtualDisplayPackaging, RefreshScriptBuildsDriverProbeAndValidatesControlInterface) {
  const auto script = read_source_file("packaging/windows/virtual_display_driver/refresh_driver_package.ps1");

  expect_contains(script, "[string]$PrebuiltPackageDir");
  expect_contains(script, "Resolve-PrebuiltPackageRoot");
  expect_contains(script, "Refreshing staged driver assets from prebuilt package");
  expect_contains(script, "New-SelfSignedCertificate");
  expect_contains(script, "Export-PackageCertificate");
  expect_contains(script, "-DBUILD_SUNSHINE_VIRTUAL_DISPLAY_DRIVER=ON");
  expect_contains(script, "-DBUILD_VIRTUALDISPLAY_PROBE=ON");
  expect_contains(script, "-DBUILD_VIRTUALDISPLAY_VULKAN_LAYER=ON");
  expect_contains(script, "--target SunshineVirtualDisplayDriverPackageFiles virtualdisplay_probe vk_layer_sunshine_hdr");
  expect_contains(script, "Get-ChildItem -LiteralPath $driverBuildDir -Recurse -Directory -Filter 'driver-package'");
  expect_contains(script, "$probeBuildExe = Join-Path $BuildDir 'src\\driver\\virtualdisplay_probe.exe'");
  expect_contains(script, "$packageProbe = Join-Path $packageRoot 'virtualdisplay_probe.exe'");
  expect_contains(script, "$vulkanLayerBuildDll = Join-Path $BuildDir 'src\\driver\\VkLayer_sunshine_hdr.dll'");
  expect_contains(script, "$vulkanLayerBuildJson = Join-Path $BuildDir 'src\\driver\\VkLayer_sunshine_hdr.json'");
  expect_contains(script, "$packageVulkanLayerDir = Join-Path $packageRoot 'vulkan-layer'");
  expect_contains(script, "Copy-Item -Force -LiteralPath $probeBuildExe -Destination $packageProbe");
  expect_contains(script, "Copy-Item -Force -LiteralPath $vulkanLayerBuildDll -Destination $packageVulkanLayerDll");
  expect_contains(script, "Copy-Item -Force -LiteralPath $vulkanLayerBuildJson -Destination $packageVulkanLayerJson");
  expect_contains(script, "Assert-SameFile -Expected $expectedPackageProbe -Actual $packageProbe");
  expect_contains(script, "Assert-SameFile -Expected $expectedPackageVulkanLayerDll -Actual $packageVulkanLayerDll");
  expect_contains(script, "Assert-SameFile -Expected $expectedPackageVulkanLayerJson -Actual $packageVulkanLayerJson");
}

TEST(SunshineVirtualDisplayPackaging, LocalPackageRefreshSkipsDriverCatalogSigning) {
  const auto cmake = read_source_file("cmake/packaging/windows.cmake");

  expect_contains(cmake, "SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SIGNING_ARGS");
  expect_contains(cmake, "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SIGNING_ARGS}");
  expect_contains(cmake, "list(APPEND SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SIGNING_ARGS -SkipSigning)");
}

TEST(SunshineVirtualDisplayPackaging, InstallerUsesBoundedTemporaryDisplayHealthCheck) {
  const auto installer = read_source_file("src_assets/windows/drivers/sunshine/install.ps1");

  expect_contains(installer, "$probePath = Join-Path $scriptDir 'virtualdisplay_probe.exe'");
  expect_contains(installer, "$vulkanLayerDllPath = Join-Path $vulkanLayerDir 'VkLayer_sunshine_hdr.dll'");
  expect_contains(installer, "$vulkanLayerJsonPath = Join-Path $vulkanLayerDir 'VkLayer_sunshine_hdr.json'");
  expect_contains(installer, "foreach ($artifact in @($infPath, $dllPath, $catPath, $nefConc, $probePath))");
  expect_contains(installer, "function Assert-VulkanLayerPackage");
  expect_contains(installer, "foreach ($artifact in @($vulkanLayerDllPath, $vulkanLayerJsonPath))");
  expect_contains(installer, "function Open-LocalMachineRegistryKey");
  expect_contains(installer, "[Microsoft.Win32.RegistryView]::Registry64");
  expect_contains(installer, "[Microsoft.Win32.RegistryView]::Registry32");
  expect_contains(installer, "Vulkan HDR implicit layer registrations removed from ${view}");
  expect_contains(installer, "function Assert-CatalogSignature");
  expect_contains(installer, "Get-AuthenticodeSignature -LiteralPath $catPath");
  expect_contains(installer, "$signature.Status -eq 'HashMismatch'");
  expect_contains(installer, "Driver catalog signature is not valid");
  expect_contains(installer, "$matchesBundledCertificate");
  expect_contains(installer, "Assert-CatalogSignature");
  expect_contains(installer, "function Test-TemporaryVirtualDisplay");
  expect_contains(installer, "'--self-test-temp', '1920', '1080', '60'");
  expect_contains(installer, "function Invoke-InstallerHealthCheck");
  expect_contains(installer, "'/restart-device', $instanceId");
  expect_contains(installer, "'/disable-device', $instanceId, '/force'");
  expect_contains(installer, "'/enable-device', $instanceId");
  expect_contains(installer, "VIRTUAL_DISPLAY_RESTART_REQUIRED");
  expect_contains(installer, "VIRTUAL_DISPLAY_DRIVER_WARNING");
  EXPECT_EQ(installer.find("Assert-DriverControlInterface"), std::string::npos);
  EXPECT_EQ(installer.find("Assert-DriverHdrTemporaryDisplay"), std::string::npos);
  EXPECT_EQ(installer.find("--self-test-hdr"), std::string::npos);
  EXPECT_EQ(installer.find("--query-permanent"), std::string::npos);
  EXPECT_EQ(installer.find("'--check'"), std::string::npos);
}

TEST(SunshineVirtualDisplayPackaging, InstallerDoesNotForceKillUmdfHosts) {
  const auto installer = read_source_file("src_assets/windows/drivers/sunshine/install.ps1");

  EXPECT_EQ(installer.find("Get-Process -Name 'WUDFHost'"), std::string::npos);
  EXPECT_EQ(installer.find("Stop-Process -Name 'WUDFHost'"), std::string::npos);
  expect_contains(installer, "Let PnP removal unload the UMDF host");
}

TEST(SunshineVirtualDisplayPackaging, InstallerReplacesOnlyExistingSunshineDriverStorePackages) {
  const auto installer = read_source_file("src_assets/windows/drivers/sunshine/install.ps1");

  expect_contains(installer, "Test-DriverPackageRefreshNeeded");
  expect_contains(installer, "Get-CurrentDriverStoreDllPaths");
  expect_contains(installer, "Get-FileHash -Algorithm SHA256 -LiteralPath $dllPath");
  expect_contains(installer, "$currentHashes.Count -eq 1");
  expect_contains(installer, "Installed driver package already matches packaged driver payload; skipping driver replacement.");

  expect_contains(installer, "function Test-DeviceNodePresent");
  expect_contains(installer, "timed out after $TimeoutSeconds seconds");

  const auto main_install = installer.find("$driverPackageRefreshNeeded = Test-DriverPackageRefreshNeeded");
  ASSERT_NE(main_install, std::string::npos);

  const auto stop_sunshine = installer.find("Stop-SunshineForDriverInstall", main_install);
  const auto install_package = installer.find("Install-DriverPackage", stop_sunshine);
  const auto create_device = installer.find("Creating device node.", install_package);
  const auto scan_devices = installer.find("'/scan-devices'", install_package);

  ASSERT_NE(stop_sunshine, std::string::npos);
  ASSERT_NE(install_package, std::string::npos);
  ASSERT_NE(create_device, std::string::npos);
  ASSERT_NE(scan_devices, std::string::npos);
  EXPECT_LT(stop_sunshine, install_package);
  EXPECT_LT(install_package, create_device);
  EXPECT_LT(create_device, scan_devices);
  const auto install_flow = installer.substr(main_install, scan_devices - main_install);
  EXPECT_EQ(install_flow.find("Remove-DriverPackage"), std::string::npos);
  expect_contains(installer, "Stop-Service -Name 'SunshineService' -Force");
  EXPECT_EQ(installer.find("Remove-LegacyVirtualDisplayDrivers"), std::string::npos);
  EXPECT_EQ(installer.find("SudoVDA"), std::string::npos);
  EXPECT_EQ(installer.find("MttVDD"), std::string::npos);
  EXPECT_EQ(installer.find("SudoVDA.inf"), std::string::npos);
  EXPECT_EQ(installer.find("MttVDD.inf"), std::string::npos);
}

TEST(SunshineVirtualDisplayPackaging, WixRunsSunshineDriverInstallerWithSixtyFourBitPowerShell) {
  const auto actions = read_source_file("packaging/windows/wix/custom_actions.wxs");

  expect_contains(
    actions,
    "<CustomAction Id=\"InstallSudovda\" BinaryKey=\"WixCA\" DllEntry=\"WixQuietExec\" Execute=\"deferred\" Return=\"check\" Impersonate=\"no\" />"
  );
  expect_contains(
    actions,
    "<CustomAction Id=\"InstallVirtualDisplayDriver\" BinaryKey=\"WixCA\" DllEntry=\"WixQuietExec\" Execute=\"deferred\" Return=\"ignore\" Impersonate=\"no\" />"
  );
  expect_contains(
    actions,
    "<CustomAction Id=\"RegisterVulkanHdrLayer\" BinaryKey=\"WixCA\" DllEntry=\"WixQuietExec\" Execute=\"deferred\" Return=\"ignore\" Impersonate=\"no\" />"
  );
  expect_contains(
    actions,
    "<CustomAction Id=\"UnregisterVulkanHdrLayer\" BinaryKey=\"WixCA\" DllEntry=\"WixQuietExec\" Execute=\"deferred\" Return=\"ignore\" Impersonate=\"no\" />"
  );
  expect_contains(
    actions,
    "<CustomAction Id=\"RestoreNvPrefsUndo\" BinaryKey=\"WixCA\" DllEntry=\"WixQuietExec\" Execute=\"deferred\" Return=\"ignore\" Impersonate=\"no\" />"
  );
  expect_contains(
    actions,
    "Property=\"InstallVirtualDisplayDriver\""
  );
  expect_contains(
    actions,
    "Property=\"RegisterVulkanHdrLayer\""
  );
  expect_contains(
    actions,
    "Property=\"UnregisterVulkanHdrLayer\""
  );
  expect_contains(
    actions,
    "[System64Folder]WindowsPowerShell\\v1.0\\powershell.exe&quot; -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass -File &quot;[INSTALL_ROOT]drivers\\sunshine\\install.ps1&quot; -InstallerBestEffort"
  );
  expect_contains(
    actions,
    "[System64Folder]WindowsPowerShell\\v1.0\\powershell.exe&quot; -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass -File &quot;[INSTALL_ROOT]drivers\\sunshine\\install.ps1&quot; -RegisterVulkanLayerOnly"
  );
  expect_contains(
    actions,
    "[System64Folder]WindowsPowerShell\\v1.0\\powershell.exe&quot; -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass -File &quot;[INSTALL_ROOT]drivers\\sunshine\\install.ps1&quot; -Uninstall"
  );
  expect_contains(
    actions,
    "[System64Folder]WindowsPowerShell\\v1.0\\powershell.exe&quot; -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass -File &quot;[INSTALL_ROOT]drivers\\sunshine\\install.ps1&quot; -UnregisterVulkanLayerOnly"
  );
  expect_contains(
    actions,
    "installer-migrations.ps1&quot; -InstallVirtualDisplayDriver &quot;[INSTALL_VIRTUAL_DISPLAY_DRIVER]&quot;"
  );
}

TEST(SunshineVirtualDisplayPackaging, WixSchedulesDriverInstallAfterFilesBeforeMigrations) {
  const auto patch = read_source_file("packaging/windows/wix/patch_custom_actions.wxs");

  expect_contains(patch, "<Property Id=\"INSTALL_SUDOVDA\" Value=\"1\" Secure=\"yes\"/>");
  expect_contains(patch, "<Property Id=\"INSTALL_VIRTUAL_DISPLAY_DRIVER\" Secure=\"yes\"/>");
  expect_contains(
    patch,
    "<Custom Action=\"SetResetAcls\" After=\"InstallFiles\">NOT REMOVE</Custom>"
  );
  expect_contains(
    patch,
    "<Custom Action=\"SetInstallSudovda\" After=\"ResetAcls\">NOT REMOVE AND INSTALL_SUDOVDA = \"1\"</Custom>"
  );
  expect_contains(
    patch,
    "<Custom Action=\"InstallSudovda\" After=\"SetInstallSudovda\">NOT REMOVE AND INSTALL_SUDOVDA = \"1\"</Custom>"
  );
  expect_contains(
    patch,
    "<Custom Action=\"SetInstallVirtualDisplayDriver\" After=\"InstallSudovda\">NOT REMOVE AND INSTALL_VIRTUAL_DISPLAY_DRIVER &lt;&gt; \"0\"</Custom>"
  );
  expect_contains(
    patch,
    "<Custom Action=\"InstallVirtualDisplayDriver\" After=\"SetInstallVirtualDisplayDriver\">NOT REMOVE AND INSTALL_VIRTUAL_DISPLAY_DRIVER &lt;&gt; \"0\"</Custom>"
  );
  expect_contains(
    patch,
    "<Custom Action=\"SetRegisterVulkanHdrLayer\" After=\"InstallVirtualDisplayDriver\">NOT REMOVE AND INSTALL_VIRTUAL_DISPLAY_DRIVER &lt;&gt; \"0\"</Custom>"
  );
  expect_contains(
    patch,
    "<Custom Action=\"RegisterVulkanHdrLayer\" After=\"SetRegisterVulkanHdrLayer\">NOT REMOVE AND INSTALL_VIRTUAL_DISPLAY_DRIVER &lt;&gt; \"0\"</Custom>"
  );
  expect_contains(
    patch,
    "<Custom Action=\"SetMigrateConfig\" After=\"RegisterVulkanHdrLayer\">NOT Installed AND NOT REMOVE</Custom>"
  );
  expect_contains(
    patch,
    "<Custom Action=\"SetRestoreNvPrefsUndo\" Before=\"RemoveFiles\">REMOVE=\"ALL\" AND NOT UPGRADINGPRODUCTCODE</Custom>"
  );
  expect_contains(
    patch,
    "<Custom Action=\"SetUnregisterVulkanHdrLayer\" After=\"RestoreNvPrefsUndo\">REMOVE=\"ALL\" AND NOT UPGRADINGPRODUCTCODE</Custom>"
  );
  expect_contains(
    patch,
    "<Custom Action=\"UnregisterVulkanHdrLayer\" After=\"SetUnregisterVulkanHdrLayer\">REMOVE=\"ALL\" AND NOT UPGRADINGPRODUCTCODE</Custom>"
  );
  expect_contains(
    patch,
    "<Custom Action=\"SetUninstallSudovda\" After=\"UnregisterVulkanHdrLayer\">REMOVE=\"ALL\" AND NOT UPGRADINGPRODUCTCODE AND REMOVEVIRTUALDISPLAYDRIVER = \"1\"</Custom>"
  );
  expect_contains(
    patch,
    "<Custom Action=\"UninstallSudovda\" After=\"SetUninstallSudovda\">REMOVE=\"ALL\" AND NOT UPGRADINGPRODUCTCODE AND REMOVEVIRTUALDISPLAYDRIVER = \"1\"</Custom>"
  );
  expect_contains(
    patch,
    "<Custom Action=\"SetUninstallVirtualDisplayDriver\" After=\"UninstallSudovda\">REMOVE=\"ALL\" AND NOT UPGRADINGPRODUCTCODE AND REMOVEVIRTUALDISPLAYDRIVER = \"1\"</Custom>"
  );
}

TEST(SunshineVirtualDisplayPackaging, CmakeUsesPrebuiltDriverPackageOnlyInGithubActions) {
  const auto cmake = read_source_file("cmake/packaging/windows.cmake");

  expect_contains(cmake, "SUNSHINE_LIBVIRTUALDISPLAY_PREBUILT_DIR");
  expect_contains(cmake, "SUNSHINE_EFFECTIVE_LIBVIRTUALDISPLAY_PREBUILT_DIR");
  expect_contains(cmake, "NOT \"$ENV{GITHUB_ACTIONS}\" STREQUAL \"true\"");
  expect_contains(cmake, "Ignoring SUNSHINE_LIBVIRTUALDISPLAY_PREBUILT_DIR outside GitHub Actions");
  expect_contains(cmake, "-PrebuiltPackageDir \"${SUNSHINE_EFFECTIVE_LIBVIRTUALDISPLAY_PREBUILT_DIR}\"");
}

TEST(SunshineVirtualDisplayPackaging, LocalInstallerPackagingUsesDeterministicMsiPayload) {
  const auto targets = read_source_file("cmake/targets/windows.cmake");
  const auto bootstrapper = read_source_file("packaging/windows/bootstrapper/build_bootstrapper.ps1");

  expect_contains(targets, "-MsiPath \"${CMAKE_BINARY_DIR}/cpack_artifacts/${CPACK_PACKAGE_FILE_NAME}.msi\"");
  expect_contains(bootstrapper, "function Find-LatestMsi");
}

TEST(SunshineVirtualDisplayPackaging, LocalDriverRefreshSkipsSigningByDefault) {
  const auto cmake = read_source_file("cmake/packaging/windows.cmake");

  expect_contains(
    cmake,
    "Local package builds must not require a driver signing private key."
  );
  expect_contains(cmake, "if(NOT \"$ENV{GITHUB_ACTIONS}\" STREQUAL \"true\")");
  expect_contains(cmake, "list(APPEND SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SIGNING_ARGS -SkipSigning)");
  EXPECT_EQ(cmake.find("SUNSHINE_SKIP_LOCAL_VIRTUAL_DISPLAY_DRIVER_SIGNING"), std::string::npos);
}

TEST(SunshineVirtualDisplayPackaging, MsiReplacementIsTransactional) {
  const auto wix = read_source_file("packaging/windows/wix/WIX.template.in");
  const auto wix_cmake = read_source_file("cmake/packaging/windows_wix.cmake");
  const auto bootstrapper = read_source_file("packaging/windows/bootstrapper/VibeshineInstaller.cs");

  // Upgrades, same-version rebuilds, and downgrades all run as a single MSI
  // transaction; a failed install rolls back to the previous version instead
  // of leaving neither installed.
  expect_contains(wix, "Schedule=\"afterInstallInitialize\"");
  expect_contains(wix, "AllowDowngrades=\"yes\"");
  EXPECT_EQ(wix.find("Schedule=\"afterInstallValidate\""), std::string::npos);
  EXPECT_EQ(wix.find("DowngradeErrorMessage"), std::string::npos);

  // Prerelease builds are strictly ordered in the ProductVersion third field
  // (patch * 100 + ordinal) so MSI can see beta->beta and beta->stable as
  // real upgrades; the human-readable semver is preserved for ARP.
  expect_contains(wix_cmake, "_WIX_PRERELEASE_ORDINAL");
  expect_contains(wix_cmake, "math(EXPR _WIX_PAT \"${_WIX_PAT} * 100 + ${_WIX_PRERELEASE_ORDINAL}\")");
  expect_contains(wix_cmake, "-dVibeshineSemVer=${PROJECT_VERSION_FULL}");
  expect_contains(read_source_file("packaging/windows/wix/custom_actions.wxs"), "$(var.VibeshineSemVer)");

  // The bootstrapper only falls back to uninstall-then-install for legacy
  // payloads that cannot replace in-transaction, and stashes the installed
  // MSI beforehand so a failed second phase restores the previous version.
  expect_contains(bootstrapper, "PayloadSupportsTransactionalReplacement");
  expect_contains(bootstrapper, "cli_remove_vibeshine_same_or_downgrade");
  expect_contains(bootstrapper, "TryStashInstalledVibeshinePayload");
  expect_contains(bootstrapper, "TryRestoreStashedVibeshinePayload");
}

TEST(SunshineVirtualDisplayPackaging, DirectMsiConflictRemovalBlocksInsteadOfUninstalling) {
  const auto actions = read_source_file("packaging/windows/wix/custom_actions.wxs");
  const auto patch = read_source_file("packaging/windows/wix/patch_custom_actions.wxs");
  const auto script = read_source_file("packaging/windows/wix/remove_conflicting_products.vbs");

  expect_contains(actions, "Id=\"RemoveConflictingProducts\" BinaryKey=\"RemoveConflictingProductsVbs\" VBScriptCall=\"RemoveConflictingProducts\" Execute=\"immediate\"");
  expect_contains(patch, "<Custom Action=\"RemoveConflictingProducts\" Before=\"InstallValidate\">NOT Installed AND NOT REMOVE AND SKIP_REMOVE_CONFLICTING_PRODUCTS &lt;&gt; \"1\"</Custom>");
  expect_contains(script, "HKEY_CURRENT_USER");
  expect_contains(script, "conflicting products must be removed by the bootstrapper or by the user");
  expect_contains(script, "nameUpper = \"SUNSHINE\"");
  expect_contains(script, "nameUpper = \"APOLLO\"");
  expect_contains(script, "nameUpper = \"VIBEPOLLO\"");
  EXPECT_EQ(script.find("shell.Run"), std::string::npos);
  EXPECT_EQ(script.find("Left(nameUpper"), std::string::npos);
}

TEST(SunshineVirtualDisplayPackaging, BootstrapperAdministrativeInstallDoesNotPreUninstall) {
  const auto bootstrapper = read_source_file("packaging/windows/bootstrapper/VibeshineInstaller.cs");

  const auto competing = bootstrapper.find("private static bool ShouldPreUninstallCompetingProducts");
  ASSERT_NE(competing, std::string::npos);
  const auto problematic = bootstrapper.find("private static bool ShouldPreUninstallProblematicUpgradeSource");
  ASSERT_NE(problematic, std::string::npos);
  const auto vibeshine = bootstrapper.find("private static bool ShouldPreUninstallVibeshineInstallSource");
  ASSERT_NE(vibeshine, std::string::npos);

  const auto competing_body = bootstrapper.substr(competing, problematic - competing);
  const auto problematic_body = bootstrapper.substr(problematic, vibeshine - problematic);
  EXPECT_EQ(competing_body.find("\"/a\""), std::string::npos);
  EXPECT_EQ(problematic_body.find("\"/a\""), std::string::npos);
  expect_contains(competing_body, "\"/i\"");
  expect_contains(competing_body, "\"/package\"");
  expect_contains(problematic_body, "\"/i\"");
  expect_contains(problematic_body, "\"/package\"");
}

TEST(SunshineVirtualDisplayPackaging, BootstrapperClassifiesConflictingProductsByExactName) {
  const auto bootstrapper = read_source_file("packaging/windows/bootstrapper/VibeshineInstaller.cs");

  const auto classifier = bootstrapper.find("private static InstalledProductKind GetInstalledProductKind");
  ASSERT_NE(classifier, std::string::npos);
  const auto next_function = bootstrapper.find("private static bool IsBrowserWebAppRegistration", classifier);
  ASSERT_NE(next_function, std::string::npos);
  const auto body = bootstrapper.substr(classifier, next_function - classifier);

  expect_contains(body, "string.Equals(trimmedDisplayName, \"Vibeshine\"");
  expect_contains(body, "string.Equals(trimmedDisplayName, \"Vibepollo\"");
  expect_contains(body, "string.Equals(trimmedDisplayName, \"Apollo\"");
  expect_contains(body, "string.Equals(trimmedDisplayName, \"Sunshine\"");
  EXPECT_EQ(body.find("StartsWith(\"Sunshine\""), std::string::npos);
  EXPECT_EQ(body.find("StartsWith(\"Vibepollo\""), std::string::npos);
}

TEST(SunshineVirtualDisplayPackaging, BootstrapperQuotesForwardedArgumentsWithEmbeddedQuotes) {
  const auto bootstrapper = read_source_file("packaging/windows/bootstrapper/VibeshineInstaller.cs");

  expect_contains(bootstrapper, "private static string QuoteArgument(string argument)");
  expect_contains(bootstrapper, "builder.Append('\\\\', backslashes * 2 + 1);");
  expect_contains(bootstrapper, "builder.Append('\\\\', backslashes * 2);");
  EXPECT_EQ(bootstrapper.find("if (argument.Contains(\"\\\"\"))"), std::string::npos);
}

TEST(SunshineVirtualDisplayPackaging, FactoryResetRequiresSafeInstallRootSentinel) {
  const auto script = read_source_file("src_assets/windows/misc/migration/factory-reset-appdata.ps1");
  const auto bootstrapper = read_source_file("packaging/windows/bootstrapper/VibeshineInstaller.cs");

  expect_contains(script, "function Test-SafeInstallRoot");
  expect_contains(script, "Refusing factory reset for unsafe install root");
  expect_contains(script, "scripts");
  expect_contains(script, "factory-reset-appdata.ps1");
  expect_contains(script, "'session_history'");
  expect_contains(bootstrapper, "private static bool IsSafeInstallRootForFactoryReset");
  expect_contains(bootstrapper, "Path.Combine(fullRoot, \"scripts\", \"factory-reset-appdata.ps1\")");
}

TEST(SunshineVirtualDisplayPackaging, WindowsCiRequiresValidSignPathSignatures) {
  const auto workflow = read_source_file(".github/workflows/ci-windows.yml");

  expect_contains(workflow, "$sig.Status -eq 'Valid'");
  EXPECT_EQ(workflow.find("$sig.Status -ne 'NotSigned'"), std::string::npos);
}

TEST(SunshineVirtualDisplayPackaging, WindowsCiCanSelfSignDriverWithoutPersistentSecret) {
  const auto workflow = read_source_file(".github/workflows/ci-windows.yml");

  expect_contains(workflow, "VDD_SIGNING_CERT_PFX_BASE64:");
  expect_contains(workflow, "required: false");
  expect_contains(workflow, "driver package refresh will generate a self-signed catalog certificate");
  expect_contains(workflow, "VDD_SIGNING_CERT_PASSWORD is required when VDD_SIGNING_CERT_PFX_BASE64 is set.");
  EXPECT_EQ(
    workflow.find("VDD_SIGNING_CERT_PFX_BASE64 is required to sign the virtual display driver catalog."),
    std::string::npos
  );
}

TEST(SunshineVirtualDisplayPackaging, BootstrapperOffersSudoVdaRollback) {
  const auto bootstrapper = read_source_file("packaging/windows/bootstrapper/VibeshineInstaller.cs");

  expect_contains(bootstrapper, "InternalInstallVirtualDisplay = true;");
  expect_contains(bootstrapper, "Content = \"Use SudoVDA\"");
  expect_contains(bootstrapper, "IsChecked = _useSudoVdaSelectedInConfig");
  expect_contains(bootstrapper, "Vibepollo Display Driver is installed and selected by default");
  expect_contains(bootstrapper, "Enable this option to use SudoVDA instead.");
  expect_contains(bootstrapper, "contentStack.Children.Add(tipsSection);");
  expect_contains(bootstrapper, "contentStack.Children.Add(_installVirtualDisplaySection);");
  expect_contains(bootstrapper, "driverStack.Children.Add(_installVirtualDisplayCheckBox);");
  EXPECT_EQ(bootstrapper.find("tipsStack.Children.Add(_installVirtualDisplayCheckBox);"), std::string::npos);
  EXPECT_EQ(bootstrapper.find("contentStack.Children.Add(_installVirtualDisplayCheckBox);"), std::string::npos);
  EXPECT_LT(
    bootstrapper.find("contentStack.Children.Add(tipsSection);"),
    bootstrapper.find("contentStack.Children.Add(_installVirtualDisplaySection);")
  );
  EXPECT_LT(
    bootstrapper.find("contentStack.Children.Add(_installVirtualDisplaySection);"),
    bootstrapper.find("contentStack.Children.Add(divider);")
  );
  expect_contains(bootstrapper, "\"--internal-install-virtual-display-driver\",");
  expect_contains(bootstrapper, "installVirtualDisplayDriver ? \"1\" : \"0\",");
  expect_contains(bootstrapper, "\"INSTALL_VIRTUAL_DISPLAY_DRIVER=\" + (installVirtualDisplayDriver ? \"1\" : \"0\")");
  expect_contains(bootstrapper, "return _installVirtualDisplayCheckBox.IsChecked != true;");
  expect_contains(bootstrapper, "CollectInstallComponentFailures(logPath, installVirtualDisplayDriver)");
  expect_contains(bootstrapper, "elevatedArgs.AddRange(arguments.ForwardedArguments);");
}

TEST(SunshineVirtualDisplayPackaging, BootstrapperReportsSunshineDriverRestartWarnings) {
  const auto bootstrapper = read_source_file("packaging/windows/bootstrapper/VibeshineInstaller.cs");

  expect_contains(bootstrapper, "VIRTUAL_DISPLAY_RESTART_REQUIRED");
  expect_contains(bootstrapper, "VIRTUAL_DISPLAY_DRIVER_WARNING");
  expect_contains(
    bootstrapper,
    "Virtual display driver installed, but Windows restart is required before virtual display can function."
  );
  expect_contains(bootstrapper, "InstallLogIndicatesDriverRebootRequired(logPath)");
  expect_contains(bootstrapper, "exitCode = 3010");
}

TEST(SunshineVirtualDisplayPackaging, BootstrapperShowsVirtualDisplayChoiceOnUpgrade) {
  const auto bootstrapper = read_source_file("packaging/windows/bootstrapper/VibeshineInstaller.cs");

  expect_contains(bootstrapper, "var showInstallLocation = !hasInstalledProduct;");
  expect_contains(bootstrapper, "_installSection.Visibility = showInstallLocation ? Visibility.Visible : Visibility.Collapsed;");
  expect_contains(bootstrapper, "_installPathGrid.Visibility = showInstallLocation ? Visibility.Visible : Visibility.Collapsed;");
  expect_contains(bootstrapper, "_installVirtualDisplayCheckBox.IsEnabled = allowInstallInputs && _showInstallVirtualDisplayOption;");
  expect_contains(bootstrapper, "_installVirtualDisplaySection.Visibility = _showInstallVirtualDisplayOption ? Visibility.Visible : Visibility.Collapsed;");
}

TEST(SunshineVirtualDisplayPackaging, InstallerSelectionSeedsWebUiSunshineDriverFlag) {
  const auto migration = read_source_file("src_assets/windows/misc/migration/installer-migrations.ps1");
  const auto config = read_source_file("src/config.cpp");
  const auto header = read_source_file("src/config.h");
  const auto webStore = read_source_file("src_assets/common/assets/web/stores/config.ts");
  const auto audioVideo = read_source_file("src_assets/common/assets/web/configs/tabs/AudioVideo.vue");
  const auto locale = read_source_file("src_assets/common/assets/web/public/assets/locale/en.json");
  const auto docs = read_source_file("docs/configuration.md");

  expect_contains(migration, "[string]$InstallVirtualDisplayDriver");
  expect_contains(migration, "Update-SunshineVirtualDriverPreference");
  expect_contains(migration, "dd_use_sunshine_virtual_display_driver");
  expect_contains(migration, "if ($null -eq $enabled)");
  expect_contains(migration, "-Value $(if ($enabled) { 'enabled' } else { 'disabled' })");
  expect_contains(migration, "$isLegacySplitEncodeProperty = $property.Name -eq 'nvenc_force_split_encode'");
  expect_contains(migration, "$targetName -eq 'nvenc_split_encode' -and -not $isLegacySplitEncodeProperty");
  expect_contains(migration, "Updated Vibepollo Display Driver preference from installer selection.");
  expect_contains(header, "use_sunshine_virtual_display_driver");
  expect_contains(config, "true,  // use_sunshine_virtual_display_driver");
  expect_contains(config, "bool_f(vars, \"dd_use_sunshine_virtual_display_driver\", video.dd.use_sunshine_virtual_display_driver);");
  expect_contains(config, "\"dd_use_sunshine_virtual_display_driver\"");
  expect_contains(webStore, "dd_use_sunshine_virtual_display_driver: true");
  expect_contains(webStore, "'dd_use_sunshine_virtual_display_driver'");
  expect_contains(audioVideo, "useSudoVdaDriver");
  expect_contains(audioVideo, "config.value?.dd_use_sunshine_virtual_display_driver === false");
  expect_contains(audioVideo, "store.updateOption('dd_use_sunshine_virtual_display_driver', !useSudoVda)");
  expect_contains(audioVideo, "return 'per_client';");
  expect_contains(audioVideo, "config.dd_use_sunshine_virtual_display_driver_desc");
  expect_contains(audioVideo, "currentDriverStatusMessage");
  expect_contains(audioVideo, "virtual_display_status_sudovda_ready");
  expect_contains(audioVideo, "virtual_display_status_vibeshine_ready");
  expect_contains(locale, "\"dd_use_sunshine_virtual_display_driver\": \"Use SudoVDA\"");
  expect_contains(locale, "Switch back to SudoVDA for virtual displays");
  expect_contains(locale, "\"virtual_display_status_sudovda_ready\": \"SudoVDA driver ready\"");
  expect_contains(locale, "\"virtual_display_status_vibeshine_ready\": \"Vibepollo driver ready\"");
  expect_contains(docs, "### dd_use_sunshine_virtual_display_driver");
  expect_contains(docs, "Disable this to switch back to the bundled SudoVDA rollback driver.");
  expect_contains(docs, "<td colspan=\"2\">@code{}true@endcode</td>");
  EXPECT_NE(audioVideo.find("v-model:checked=\"useSudoVdaDriver\""), std::string::npos);
}

TEST(SunshineVirtualDisplayPackaging, BootstrapperCliPreservesSunshineDriverSelection) {
  const auto bootstrapper = read_source_file("packaging/windows/bootstrapper/VibeshineInstaller.cs");

  expect_contains(bootstrapper, "PreserveCliVirtualDisplayDriverSelection(cliArgs);");
  expect_contains(bootstrapper, "HasProperty(cliArgs, \"INSTALL_VIRTUAL_DISPLAY_DRIVER\")");
  expect_contains(bootstrapper, "TryReadCliSunshineVirtualDisplayDriverSelection(cliArgs, out useSunshineDriver)");
  expect_contains(bootstrapper, "cliArgs.Add(\"INSTALL_VIRTUAL_DISPLAY_DRIVER=\" + (useSunshineDriver ? \"1\" : \"0\"));");
  expect_contains(bootstrapper, "GetPropertyValue(cliArgs, \"INSTALL_ROOT\")");
  expect_contains(bootstrapper, "dd_use_sunshine_virtual_display_driver");
}

TEST(SunshineVirtualDisplayPackaging, RuntimeFeatureFlagFallsBackToSudoVda) {
  const auto cmake = read_source_file("cmake/compile_definitions/windows.cmake");
  const auto dispatcher = read_source_file("src/platform/windows/virtual_display.cpp");
  const auto sunshineDriver = read_source_file("src/platform/windows/virtual_display_sunshine.cpp");
  const auto sudoDriver = read_source_file("src/platform/windows/virtual_display_sudovda.cpp");
  const auto audioVideo = read_source_file("src_assets/common/assets/web/configs/tabs/AudioVideo.vue");

  expect_contains(cmake, "src/platform/windows/virtual_display.cpp");
  expect_contains(cmake, "src/platform/windows/virtual_display_sunshine.cpp");
  expect_contains(cmake, "src/platform/windows/virtual_display_sudovda.cpp");
  expect_contains(cmake, "third-party/sudovda/sudovda.h");
  expect_contains(dispatcher, "config::video.dd.use_sunshine_virtual_display_driver");
  expect_contains(dispatcher, "VDISPLAY_SUNSHINE::isVirtualDisplayDriverInstalled()");
  expect_contains(dispatcher, "VDISPLAY_SUDOVDA::isSudaVDADriverInstalled()");
  expect_contains(dispatcher, "VDISPLAY_SUDOVDA::createVirtualDisplay");
  expect_contains(sunshineDriver, "namespace VDISPLAY_SUNSHINE");
  expect_contains(sudoDriver, "namespace VDISPLAY_SUDOVDA");
  EXPECT_EQ(audioVideo.find(":disabled=\"platform === 'windows' && useSudoVdaDriver\""), std::string::npos);
}

TEST(SunshineVirtualDisplayPackaging, RuntimeAvailabilityChecksDoNotRepairOrReinstallMissingDrivers) {
  const auto sunshineDriver = read_source_file("src/platform/windows/virtual_display_sunshine.cpp");
  const auto sudoDriver = read_source_file("src/platform/windows/virtual_display_sudovda.cpp");

  expect_contains(sunshineDriver, "bool is_sunshine_driver_installed_passive()");
  expect_contains(sunshineDriver, "return find_virtual_display_device_instance_id().has_value();");
  const auto sunshineStatusPos = sunshineDriver.find("bool isVirtualDisplayDriverInstalled() {");
  ASSERT_NE(sunshineStatusPos, std::string::npos);
  const auto sunshineStatus = sunshineDriver.substr(sunshineStatusPos, 400);
  expect_contains(sunshineStatus, "return is_sunshine_driver_installed_passive();");
  EXPECT_EQ(sunshineStatus.find("ensure_driver_is_ready"), std::string::npos);

  expect_contains(sudoDriver, "bool is_sudovda_driver_installed_passive()");
  expect_contains(sudoDriver, "return find_sudovda_device_instance_id().has_value();");
  const auto sudoStatusPos = sudoDriver.find("bool isSudaVDADriverInstalled() {");
  ASSERT_NE(sudoStatusPos, std::string::npos);
  const auto sudoStatus = sudoDriver.substr(sudoStatusPos, 400);
  expect_contains(sudoStatus, "return is_sudovda_driver_installed_passive();");
  EXPECT_EQ(sudoStatus.find("ensure_driver_is_ready"), std::string::npos);
}

TEST(SunshineVirtualDisplayPackaging, SunshineDriverUsesConfiguredRenderAdapterPreference) {
  const auto sunshineDriver = read_source_file("src/platform/windows/virtual_display_sunshine.cpp");

  expect_contains(sunshineDriver, "SetRenderAdapterRequest request {};");
  expect_contains(sunshineDriver, "request.adapter_luid = sunshine_driver::from_windows_luid(adapter_luid);");
  expect_contains(sunshineDriver, "client.set_render_adapter(request);");

  const auto byNamePos = sunshineDriver.find("bool setRenderAdapterByName(const std::wstring &adapterName) {");
  ASSERT_NE(byNamePos, std::string::npos);
  const auto byName = sunshineDriver.substr(byNamePos, sunshineDriver.find("bool setRenderAdapterWithMostDedicatedMemory", byNamePos) - byNamePos);
  expect_contains(byName, "std::wstring_view(desc.Description) != adapterName");
  expect_contains(byName, "return set_render_adapter_luid(desc.AdapterLuid");

  const auto automaticPos = sunshineDriver.find("bool setRenderAdapterWithMostDedicatedMemory() {");
  ASSERT_NE(automaticPos, std::string::npos);
  const auto automatic = sunshineDriver.substr(automaticPos, sunshineDriver.find("bool wait_for_virtual_display_ready", automaticPos) - automaticPos);
  expect_contains(automatic, "desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE");
  expect_contains(automatic, "dedicated > best_dedicated");
  expect_contains(automatic, "return set_render_adapter_luid(best_luid");
  EXPECT_EQ(automatic.find("ignores automatic render adapter override request"), std::string::npos);
}

TEST(SunshineVirtualDisplayPackaging, SunshineDriverGeneratesProtocolValidLeaseIds) {
  const auto sunshineDriver = read_source_file("src/platform/windows/virtual_display_sunshine.cpp");

  const auto generatorPos = sunshineDriver.find("std::uint64_t generate_driver_lease_id() {");
  ASSERT_NE(generatorPos, std::string::npos);
  const auto generator = sunshineDriver.substr(generatorPos, sunshineDriver.find("bool is_missing_lease_error", generatorPos) - generatorPos);
  expect_contains(generator, "sunshine_driver::kMinOpaqueLeaseId");
  expect_contains(generator, "lease_id < sunshine_driver::kMinOpaqueLeaseId");
  expect_contains(generator, "rng() | sunshine_driver::kMinOpaqueLeaseId");
}

TEST(SunshineVirtualDisplayPackaging, SunshineDriverReopensStaleControlTransportForLeaseFeeding) {
  const auto sunshineDriver = read_source_file("src/platform/windows/virtual_display_sunshine.cpp");

  const auto closePos = sunshineDriver.find("void closeVDisplayDevice() {");
  ASSERT_NE(closePos, std::string::npos);
  const auto closeBody = sunshineDriver.substr(closePos, sunshineDriver.find("void ensureVirtualDisplayRegistryDefaults", closePos) - closePos);
  expect_contains(closeBody, "setWatchdogFeedingEnabled(false);");
  expect_contains(closeBody, "g_watchdog_grace_deadline_ns.store(0, std::memory_order_release);");
  expect_contains(closeBody, "VIRTUAL_DISPLAY_DRIVER_TRANSPORT.reset();");
  EXPECT_EQ(closeBody.find("!VIRTUAL_DISPLAY_DRIVER_TRANSPORT || !VIRTUAL_DISPLAY_DRIVER_TRANSPORT->valid()"), std::string::npos);

  expect_contains(sunshineDriver, "bool ensure_control_transport_responsive(std::string_view operation)");
  expect_contains(sunshineDriver, "driver_transport_responsive(VIRTUAL_DISPLAY_DRIVER_TRANSPORT.get())");
  expect_contains(sunshineDriver, "ensure_control_transport_responsive(\"Sunshine virtual display lease feed\")");
  expect_contains(sunshineDriver, "ensure_control_transport_responsive(\"Sunshine virtual display watchdog feed\")");
  expect_contains(sunshineDriver, "log_control_failure(\"Sunshine virtual display lease feed\", fed.status, fed.native_error);");
}

TEST(SunshineVirtualDisplayPackaging, SunshineDriverKeepsTransportFailuresOutOfProtocolMismatchStatus) {
  const auto sunshineDriver = read_source_file("src/platform/windows/virtual_display_sunshine.cpp");

  const auto openPos = sunshineDriver.find("DRIVER_STATUS openVDisplayDevice() {");
  ASSERT_NE(openPos, std::string::npos);
  const auto openBody = sunshineDriver.substr(openPos, sunshineDriver.find("static bool ensure_driver_is_ready_impl", openPos) - openPos);

  expect_contains(openBody, "const auto version = client.query_protocol_version();");
  expect_contains(openBody, "log_control_failure(\"Sunshine virtual display protocol query\", version.status, version.native_error);");
  expect_contains(openBody, "const bool incompatible_protocol = version.status == sunshine_driver::ControlStatus::ProtocolIncompatible;");
  expect_contains(openBody, "const auto failed_status =");
  expect_contains(openBody, "incompatible_protocol ? DRIVER_STATUS::VERSION_INCOMPATIBLE : DRIVER_STATUS::FAILED;");
  expect_contains(openBody, "return failed_status;");
}

TEST(SunshineVirtualDisplayPackaging, WindowsCiUsesPinnedLibvirtualdisplayRelease) {
  const auto workflow = read_source_file(".github/workflows/ci-windows.yml");

  expect_contains(workflow, "$releaseTag = $env:LIBVIRTUALDISPLAY_RELEASE_TAG");
  EXPECT_EQ(workflow.find("gh release list --repo Nonary/libvirtualdisplay"), std::string::npos);
}

TEST(SunshineVirtualDisplayPackaging, WindowsCiRequiresPinnedTruehdrRuntimeForReleasePackaging) {
  const auto workflow = read_source_file(".github/workflows/ci-windows.yml");
  const auto cmake = read_source_file("cmake/packaging/windows.cmake");
  const auto downloader = read_source_file("scripts/download_truehdr_runtime_release.ps1");

  expect_contains(workflow, "require_truehdr_runtime:");
  expect_contains(workflow, "default: Nonary/vibeshine_truehdr_runtime");
  expect_contains(workflow, R"(TRUEHDR_RUNTIME_ROOT: ${{ github.workspace }}\.vibeshine-deps\truehdr-runtime)");
  expect_contains(workflow, "Download pinned TrueHDR runtime");
  expect_contains(workflow, R"(.\scripts\download_truehdr_runtime_release.ps1 @args)");
  expect_contains(workflow, "\"SUNSHINE_TRUEHDR_RUNTIME_DIR=$env:TRUEHDR_RUNTIME_ROOT\" >> $env:GITHUB_ENV");
  expect_contains(workflow, "-DSUNSHINE_REQUIRE_TRUEHDR_RUNTIME=ON");
  expect_contains(workflow, "$optionalFirstParty = @('vibeshine_truehdr.dll')");
  expect_contains(workflow, "required TrueHDR first-party PE missing from MSI");

  expect_contains(cmake, "option(SUNSHINE_REQUIRE_TRUEHDR_RUNTIME");
  expect_contains(cmake, "${SUNSHINE_TRUEHDR_RUNTIME_DIR}/vibeshine_truehdr.dll");
  expect_contains(cmake, "${SUNSHINE_TRUEHDR_RUNTIME_DIR}/nvngx_truehdr.dll");
  expect_contains(cmake, "install(FILES ${SUNSHINE_TRUEHDR_RUNTIME_FILES}");

  expect_contains(downloader, "[string]$Repository = \"Nonary/vibeshine_truehdr_runtime\"");
  expect_contains(downloader, R"(Join-Path $scriptRoot ".vibeshine-deps\truehdr-runtime")");
  expect_contains(downloader, "$requiredDlls = @(\"vibeshine_truehdr.dll\", \"nvngx_truehdr.dll\")");
}

TEST(SunshineVirtualDisplayPackaging, RtspLaunchIgnoresUnmatchedUniqueIdForPerClientDisplayIdentity) {
  const auto nvhttp = read_source_file("src/nvhttp.cpp");

  expect_contains(nvhttp, "remember_tls_client_identity(req, *identity);");
  expect_contains(nvhttp, "get_remembered_tls_client_identity(request)");
  expect_contains(nvhttp, "resolve_known_client_uuid_from_launch_id");
  expect_contains(nvhttp, "Ignoring unmatched launch uniqueid for per-client settings");
  expect_contains(nvhttp, "is a placeholder and conflicts with launch uniqueid; using paired client UUID");
  expect_contains(nvhttp, "Ignoring placeholder TLS client identity");
  EXPECT_EQ(nvhttp.find("launch_session->client_uuid = get_arg(args, \"uniqueid\", \"\");"), std::string::npos);
  EXPECT_EQ(nvhttp.find("client_uuid = get_arg(args, \"uniqueid\", \"\");"), std::string::npos);
}

TEST(SunshineVirtualDisplayPackaging, ClientPermissionChecksUseRememberedTlsIdentityBeforeThreadLocalCert) {
  const auto nvhttp = read_source_file("src/nvhttp.cpp");

  const auto getVerifiedPos = nvhttp.find("inline crypto::named_cert_t *get_verified_cert(req_https_t request)");
  ASSERT_NE(getVerifiedPos, std::string::npos);
  const auto clientPermPos = nvhttp.find("inline PERM client_perm", getVerifiedPos);
  ASSERT_NE(clientPermPos, std::string::npos);
  const auto getVerifiedBody = nvhttp.substr(getVerifiedPos, clientPermPos - getVerifiedPos);

  const auto rememberedPos = getVerifiedBody.find("auto remembered = get_remembered_tls_client_identity(request)");
  const auto threadLocalPos = getVerifiedBody.find("if (!tl_peer_certificate)");
  ASSERT_NE(rememberedPos, std::string::npos);
  ASSERT_NE(threadLocalPos, std::string::npos);
  EXPECT_LT(rememberedPos, threadLocalPos);
  expect_contains(getVerifiedBody, "named_cert_p && named_cert_p->uuid == remembered->uuid");
}

TEST(SunshineVirtualDisplayPackaging, BootstrapperPassesVirtualDisplayRemovalChoiceToMsi) {
  const auto bootstrapper = read_source_file("packaging/windows/bootstrapper/VibeshineInstaller.cs");

  expect_contains(bootstrapper, "\"--internal-uninstall-remove-virtual-display-driver\",");
  expect_contains(bootstrapper, "removeVirtualDisplayDriver ? \"1\" : \"0\"");
  expect_contains(bootstrapper, "\"REMOVEVIRTUALDISPLAYDRIVER=\" + (removeVirtualDisplayDriver ? \"1\" : \"0\")");
}

TEST(SunshineVirtualDisplayPackaging, InstallerKeepsSudoVdaRollbackAndSunshineDriverDefault) {
  const auto cmake = read_source_file("cmake/packaging/windows.cmake");
  const auto actions = read_source_file("packaging/windows/wix/custom_actions.wxs");
  const auto patch = read_source_file("packaging/windows/wix/patch_custom_actions.wxs");
  const auto installer = read_source_file("src_assets/windows/drivers/sunshine/install.ps1");
  const auto sudoInstaller = read_source_file("src_assets/windows/drivers/sudovda/install.ps1");

  expect_contains(cmake, "drivers/sudovda");
  expect_contains(cmake, "drivers/sunshine");
  expect_contains(cmake, "Vibepollo Display Driver");
  expect_contains(actions, "SudoVdaRegistryDefaults");
  expect_contains(actions, "InstallSudovda");
  expect_contains(actions, "drivers\\sudovda\\install.ps1");
  expect_contains(actions, "drivers\\sunshine\\install.ps1");
  expect_contains(patch, "<Property Id=\"INSTALL_SUDOVDA\" Value=\"1\" Secure=\"yes\"/>");
  expect_contains(patch, "<Property Id=\"INSTALL_VIRTUAL_DISPLAY_DRIVER\" Secure=\"yes\"/>");
  expect_contains(patch, "INSTALL_SUDOVDA = \"1\"");
  expect_contains(patch, "INSTALL_VIRTUAL_DISPLAY_DRIVER &lt;&gt; \"0\"");
  EXPECT_EQ(cmake.find("drivers/vdd"), std::string::npos);
  EXPECT_EQ(actions.find("drivers\\vdd"), std::string::npos);
  EXPECT_EQ(patch.find("drivers\\vdd"), std::string::npos);
  EXPECT_EQ(installer.find("SudoVDA"), std::string::npos);
  EXPECT_EQ(installer.find("MttVDD"), std::string::npos);
  expect_contains(sudoInstaller, "$_.OriginalName -match '^SudoVDA\\.inf$'");
  expect_contains(sudoInstaller, "$_.ProviderName -match 'SudoMaker'");
  expect_contains(sudoInstaller, "function Assert-CertificateMatchesCatalog");
  expect_contains(sudoInstaller, "Driver catalog signer does not match bundled certificate.");
}

#endif
