/**
 * @file tests/integration/test_virtual_display_packaging.cpp
 * @brief Tests for Sunshine virtual display driver packaging invariants.
 */
#include "../tests_common.h"

#ifdef _WIN32

#include <filesystem>
#include <fstream>
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

TEST(SunshineVirtualDisplayPackaging, RefreshScriptBuildsDriverProbeAndValidatesControlInterface) {
  const auto script = read_source_file("packaging/windows/virtual_display_driver/refresh_driver_package.ps1");

  expect_contains(script, "-DBUILD_SUNSHINE_VIRTUAL_DISPLAY_DRIVER=ON");
  expect_contains(script, "-DBUILD_VIRTUALDISPLAY_PROBE=ON");
  expect_contains(script, "--target SunshineVirtualDisplayDriver virtualdisplay_probe");
  expect_contains(script, "$probeBuildExe = Join-Path $BuildDir 'src\\driver\\virtualdisplay_probe.exe'");
  expect_contains(script, "$packageProbe = Join-Path $packageRoot 'virtualdisplay_probe.exe'");
  expect_contains(script, "Copy-Item -Force -LiteralPath $probeBuildExe -Destination $packageProbe");
  expect_contains(script, "Assert-SameFile -Expected $probeBuildExe -Actual $packageProbe");
}

TEST(SunshineVirtualDisplayPackaging, InstallerValidatesPackagedProbeButDoesNotRunRuntimeQa) {
  const auto installer = read_source_file("src_assets/windows/drivers/sunshine/install.ps1");

  expect_contains(installer, "$probePath = Join-Path $scriptDir 'virtualdisplay_probe.exe'");
  expect_contains(installer, "foreach ($artifact in @($infPath, $dllPath, $catPath, $nefConc, $probePath))");
  EXPECT_EQ(installer.find("Assert-DriverControlInterface"), std::string::npos);
  EXPECT_EQ(installer.find("Assert-DriverHdrTemporaryDisplay"), std::string::npos);
  EXPECT_EQ(installer.find("--self-test-hdr"), std::string::npos);
  EXPECT_EQ(installer.find("--query-permanent"), std::string::npos);
}

TEST(SunshineVirtualDisplayPackaging, InstallerDoesNotForceKillUmdfHosts) {
  const auto installer = read_source_file("src_assets/windows/drivers/sunshine/install.ps1");

  EXPECT_EQ(installer.find("Get-Process -Name 'WUDFHost'"), std::string::npos);
  EXPECT_EQ(installer.find("Stop-Process -Name 'WUDFHost'"), std::string::npos);
  expect_contains(installer, "Let PnP removal unload the UMDF host");
}

TEST(SunshineVirtualDisplayPackaging, InstallerReplacesExistingSunshineDriverStorePackages) {
  const auto installer = read_source_file("src_assets/windows/drivers/sunshine/install.ps1");

  const auto stop_sunshine = installer.find("Stop-SunshineForDriverInstall");
  const auto legacy_cleanup = installer.find("Remove-LegacyVirtualDisplayDrivers");
  const auto remove_device = installer.find("Remove-DeviceNode", legacy_cleanup);
  const auto remove_package = installer.find("Remove-DriverPackage", remove_device);
  const auto install_package = installer.find("Install-DriverPackage", remove_package);

  ASSERT_NE(stop_sunshine, std::string::npos);
  ASSERT_NE(legacy_cleanup, std::string::npos);
  ASSERT_NE(remove_device, std::string::npos);
  ASSERT_NE(remove_package, std::string::npos);
  ASSERT_NE(install_package, std::string::npos);
  EXPECT_LT(stop_sunshine, legacy_cleanup);
  EXPECT_LT(legacy_cleanup, remove_device);
  EXPECT_LT(remove_device, remove_package);
  EXPECT_LT(remove_package, install_package);
  expect_contains(installer, "Stop-Service -Name 'SunshineService' -Force");
}

TEST(SunshineVirtualDisplayPackaging, WixRunsSunshineDriverInstallerWithSixtyFourBitPowerShell) {
  const auto actions = read_source_file("packaging/windows/wix/custom_actions.wxs");

  expect_contains(
    actions,
    "<CustomAction Id=\"InstallVirtualDisplayDriver\" BinaryKey=\"WixCA\" DllEntry=\"WixQuietExec\" Execute=\"deferred\" Return=\"check\" Impersonate=\"no\" />"
  );
  expect_contains(
    actions,
    "Property=\"InstallVirtualDisplayDriver\""
  );
  expect_contains(
    actions,
    "[System64Folder]WindowsPowerShell\\v1.0\\powershell.exe&quot; -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass -File &quot;[INSTALL_ROOT]drivers\\sunshine\\install.ps1&quot;"
  );
  expect_contains(
    actions,
    "[System64Folder]WindowsPowerShell\\v1.0\\powershell.exe&quot; -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass -File &quot;[INSTALL_ROOT]drivers\\sunshine\\install.ps1&quot; -Uninstall"
  );
}

TEST(SunshineVirtualDisplayPackaging, WixSchedulesDriverInstallAfterFilesBeforeMigrations) {
  const auto patch = read_source_file("packaging/windows/wix/patch_custom_actions.wxs");

  expect_contains(patch, "<Property Id=\"INSTALL_VIRTUAL_DISPLAY_DRIVER\" Value=\"1\" Secure=\"yes\"/>");
  expect_contains(
    patch,
    "<Custom Action=\"SetInstallVirtualDisplayDriver\" After=\"ResetAcls\">NOT REMOVE AND INSTALL_VIRTUAL_DISPLAY_DRIVER = \"1\"</Custom>"
  );
  expect_contains(
    patch,
    "<Custom Action=\"InstallVirtualDisplayDriver\" After=\"SetInstallVirtualDisplayDriver\">NOT REMOVE AND INSTALL_VIRTUAL_DISPLAY_DRIVER = \"1\"</Custom>"
  );
  expect_contains(
    patch,
    "<Custom Action=\"SetMigrateConfig\" After=\"InstallVirtualDisplayDriver\">NOT REMOVE</Custom>"
  );
  expect_contains(
    patch,
    "<Custom Action=\"SetUninstallVirtualDisplayDriver\" After=\"RestoreNvPrefsUndo\">REMOVE=\"ALL\" AND NOT UPGRADINGPRODUCTCODE AND REMOVEVIRTUALDISPLAYDRIVER = \"1\"</Custom>"
  );
}

TEST(SunshineVirtualDisplayPackaging, BootstrapperInstallsVirtualDisplayDriverByDefault) {
  const auto bootstrapper = read_source_file("packaging/windows/bootstrapper/VibeshineInstaller.cs");

  expect_contains(bootstrapper, "InternalInstallVirtualDisplay = true;");
  expect_contains(bootstrapper, "IsChecked = true,");
  expect_contains(bootstrapper, "\"--internal-install-virtual-display-driver\",");
  expect_contains(bootstrapper, "installVirtualDisplayDriver ? \"1\" : \"0\",");
  expect_contains(bootstrapper, "\"INSTALL_VIRTUAL_DISPLAY_DRIVER=\" + (installVirtualDisplayDriver ? \"1\" : \"0\")");
  expect_contains(bootstrapper, "CollectInstallComponentFailures(logPath, installVirtualDisplayDriver)");
  expect_contains(bootstrapper, "elevatedArgs.AddRange(arguments.ForwardedArguments);");
}

TEST(SunshineVirtualDisplayPackaging, BootstrapperPassesVirtualDisplayRemovalChoiceToMsi) {
  const auto bootstrapper = read_source_file("packaging/windows/bootstrapper/VibeshineInstaller.cs");

  expect_contains(bootstrapper, "\"--internal-uninstall-remove-virtual-display-driver\",");
  expect_contains(bootstrapper, "removeVirtualDisplayDriver ? \"1\" : \"0\"");
  expect_contains(bootstrapper, "\"REMOVEVIRTUALDISPLAYDRIVER=\" + (removeVirtualDisplayDriver ? \"1\" : \"0\")");
}

TEST(SunshineVirtualDisplayPackaging, DriverPackageNoLongerInstallsLegacyDrivers) {
  const auto cmake = read_source_file("cmake/packaging/windows.cmake");
  const auto actions = read_source_file("packaging/windows/wix/custom_actions.wxs");
  const auto patch = read_source_file("packaging/windows/wix/patch_custom_actions.wxs");

  EXPECT_EQ(cmake.find("drivers/vdd"), std::string::npos);
  EXPECT_EQ(cmake.find("drivers/sudovda"), std::string::npos);
  EXPECT_EQ(actions.find("drivers\\vdd"), std::string::npos);
  EXPECT_EQ(actions.find("drivers\\sudovda"), std::string::npos);
  EXPECT_EQ(patch.find("drivers\\vdd"), std::string::npos);
  EXPECT_EQ(patch.find("drivers\\sudovda"), std::string::npos);
}

#endif
