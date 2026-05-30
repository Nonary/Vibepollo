/**
 * @file tests/integration/test_virtual_display_packaging.cpp
 * @brief Tests for Vibepollo Display Driver packaging invariants.
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

  expect_contains(script, "[string]$PrebuiltPackageDir");
  expect_contains(script, "Resolve-PrebuiltPackageRoot");
  expect_contains(script, "Refreshing staged driver assets from prebuilt package");
  expect_contains(script, "New-SelfSignedCertificate");
  expect_contains(script, "Export-PackageCertificate");
  expect_contains(script, "-DBUILD_SUNSHINE_VIRTUAL_DISPLAY_DRIVER=ON");
  expect_contains(script, "-DBUILD_VIRTUALDISPLAY_PROBE=ON");
  expect_contains(script, "--target SunshineVirtualDisplayDriver virtualdisplay_probe");
  expect_contains(script, "$probeBuildExe = Join-Path $BuildDir 'src\\driver\\virtualdisplay_probe.exe'");
  expect_contains(script, "$packageProbe = Join-Path $packageRoot 'virtualdisplay_probe.exe'");
  expect_contains(script, "Copy-Item -Force -LiteralPath $probeBuildExe -Destination $packageProbe");
  expect_contains(script, "Assert-SameFile -Expected $expectedPackageProbe -Actual $packageProbe");
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

TEST(SunshineVirtualDisplayPackaging, InstallerReplacesOnlyExistingSunshineDriverStorePackages) {
  const auto installer = read_source_file("src_assets/windows/drivers/sunshine/install.ps1");

  const auto stop_sunshine = installer.find("Stop-SunshineForDriverInstall");
  const auto remove_device = installer.find("Remove-DeviceNode", stop_sunshine);
  const auto remove_package = installer.find("Remove-DriverPackage", remove_device);
  const auto install_package = installer.find("Install-DriverPackage", remove_package);

  ASSERT_NE(stop_sunshine, std::string::npos);
  ASSERT_NE(remove_device, std::string::npos);
  ASSERT_NE(remove_package, std::string::npos);
  ASSERT_NE(install_package, std::string::npos);
  EXPECT_LT(stop_sunshine, remove_device);
  EXPECT_LT(remove_device, remove_package);
  EXPECT_LT(remove_package, install_package);
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
  expect_contains(
    actions,
    "installer-migrations.ps1&quot; -InstallVirtualDisplayDriver &quot;[INSTALL_VIRTUAL_DISPLAY_DRIVER]&quot;"
  );
}

TEST(SunshineVirtualDisplayPackaging, WixSchedulesDriverInstallAfterFilesBeforeMigrations) {
  const auto patch = read_source_file("packaging/windows/wix/patch_custom_actions.wxs");

  expect_contains(patch, "<Property Id=\"INSTALL_SUDOVDA\" Value=\"1\" Secure=\"yes\"/>");
  expect_contains(patch, "<Property Id=\"INSTALL_VIRTUAL_DISPLAY_DRIVER\" Value=\"0\" Secure=\"yes\"/>");
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
    "<Custom Action=\"SetInstallVirtualDisplayDriver\" After=\"InstallSudovda\">NOT REMOVE AND INSTALL_VIRTUAL_DISPLAY_DRIVER = \"1\"</Custom>"
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
    "<Custom Action=\"SetUninstallSudovda\" After=\"RestoreNvPrefsUndo\">REMOVE=\"ALL\" AND NOT UPGRADINGPRODUCTCODE AND REMOVEVIRTUALDISPLAYDRIVER = \"1\"</Custom>"
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

TEST(SunshineVirtualDisplayPackaging, CmakePassesPrebuiltDriverPackageToRefreshScript) {
  const auto cmake = read_source_file("cmake/packaging/windows.cmake");

  expect_contains(cmake, "SUNSHINE_LIBVIRTUALDISPLAY_PREBUILT_DIR");
  expect_contains(cmake, "-PrebuiltPackageDir \"${SUNSHINE_LIBVIRTUALDISPLAY_PREBUILT_DIR}\"");
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

TEST(SunshineVirtualDisplayPackaging, BootstrapperOffersSunshineDriverOptIn) {
  const auto bootstrapper = read_source_file("packaging/windows/bootstrapper/VibeshineInstaller.cs");

  expect_contains(bootstrapper, "InternalInstallVirtualDisplay = false;");
  expect_contains(bootstrapper, "IsChecked = false,");
  expect_contains(bootstrapper, "Install experimental Vibepollo Display Driver");
  expect_contains(bootstrapper, "may improve performance and smoothness for games on virtual displays");
  expect_contains(bootstrapper, "you can easily switch back in Options if you have issues");
  expect_contains(bootstrapper, "contentStack.Children.Add(tipsSection);");
  expect_contains(bootstrapper, "contentStack.Children.Add(driverSection);");
  expect_contains(bootstrapper, "driverStack.Children.Add(_installVirtualDisplayCheckBox);");
  EXPECT_EQ(bootstrapper.find("tipsStack.Children.Add(_installVirtualDisplayCheckBox);"), std::string::npos);
  EXPECT_EQ(bootstrapper.find("contentStack.Children.Add(_installVirtualDisplayCheckBox);"), std::string::npos);
  EXPECT_LT(
    bootstrapper.find("contentStack.Children.Add(tipsSection);"),
    bootstrapper.find("contentStack.Children.Add(driverSection);")
  );
  EXPECT_LT(
    bootstrapper.find("contentStack.Children.Add(driverSection);"),
    bootstrapper.find("contentStack.Children.Add(divider);")
  );
  expect_contains(bootstrapper, "\"--internal-install-virtual-display-driver\",");
  expect_contains(bootstrapper, "installVirtualDisplayDriver ? \"1\" : \"0\",");
  expect_contains(bootstrapper, "\"INSTALL_VIRTUAL_DISPLAY_DRIVER=\" + (installVirtualDisplayDriver ? \"1\" : \"0\")");
  expect_contains(bootstrapper, "CollectInstallComponentFailures(logPath, installVirtualDisplayDriver)");
  expect_contains(bootstrapper, "elevatedArgs.AddRange(arguments.ForwardedArguments);");
}

TEST(SunshineVirtualDisplayPackaging, BootstrapperShowsVirtualDisplayChoiceOnUpgrade) {
  const auto bootstrapper = read_source_file("packaging/windows/bootstrapper/VibeshineInstaller.cs");

  expect_contains(bootstrapper, "var showInstallLocation = !hasInstalledProduct;");
  expect_contains(bootstrapper, "_installSection.Visibility = showInstallLocation ? Visibility.Visible : Visibility.Collapsed;");
  expect_contains(bootstrapper, "_installPathGrid.Visibility = showInstallLocation ? Visibility.Visible : Visibility.Collapsed;");
  expect_contains(bootstrapper, "_installVirtualDisplayCheckBox.IsEnabled = allowInstallInputs && _showInstallVirtualDisplayOption;");
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
  expect_contains(migration, "if ($null -eq $enabled -or -not $enabled)");
  expect_contains(migration, "Updated Vibepollo Display Driver preference from installer selection.");
  expect_contains(header, "use_sunshine_virtual_display_driver");
  expect_contains(config, "bool_f(vars, \"dd_use_sunshine_virtual_display_driver\", video.dd.use_sunshine_virtual_display_driver);");
  expect_contains(config, "\"dd_use_sunshine_virtual_display_driver\"");
  expect_contains(webStore, "dd_use_sunshine_virtual_display_driver: false");
  expect_contains(webStore, "'dd_use_sunshine_virtual_display_driver'");
  expect_contains(audioVideo, "sunshineVirtualDriverEnabled");
  expect_contains(audioVideo, "config.dd_use_sunshine_virtual_display_driver");
  expect_contains(audioVideo, "config.dd_use_sunshine_virtual_display_driver_desc");
  expect_contains(audioVideo, "currentDriverStatusMessage");
  expect_contains(audioVideo, "virtual_display_status_sudovda_ready");
  expect_contains(audioVideo, "virtual_display_status_vibeshine_ready");
  expect_contains(locale, "\"dd_use_sunshine_virtual_display_driver\": \"Vibepollo Display Driver\"");
  expect_contains(locale, "may improve performance and smoothness for games on virtual displays");
  expect_contains(locale, "\"virtual_display_status_sudovda_ready\": \"SudoVDA driver ready\"");
  expect_contains(locale, "\"virtual_display_status_vibeshine_ready\": \"Vibepollo driver ready\"");
  expect_contains(docs, "### dd_use_sunshine_virtual_display_driver");
  expect_contains(docs, "experimental Vibepollo Display Driver");
  EXPECT_LT(audioVideo.find("v-model:value=\"sunshineVirtualDriverEnabled\""), audioVideo.find("<FrameLimiterStep"));
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
  EXPECT_EQ(audioVideo.find(":disabled=\"platform === 'windows' && !sunshineVirtualDriverEnabled\""), std::string::npos);
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

TEST(SunshineVirtualDisplayPackaging, WindowsCiUsesPinnedLibvirtualdisplayRelease) {
  const auto workflow = read_source_file(".github/workflows/ci-windows.yml");

  expect_contains(workflow, "LIBVIRTUALDISPLAY_RELEASE_TAG: v1.2.2");
  expect_contains(workflow, "$releaseTag = $env:LIBVIRTUALDISPLAY_RELEASE_TAG");
  EXPECT_EQ(workflow.find("gh release list --repo Nonary/libvirtualdisplay"), std::string::npos);
}

TEST(SunshineVirtualDisplayPackaging, RtspLaunchIgnoresUnmatchedUniqueIdForPerClientDisplayIdentity) {
  const auto nvhttp = read_source_file("src/nvhttp.cpp");

  expect_contains(nvhttp, "remember_tls_client_identity(remote_endpoint, *identity);");
  expect_contains(nvhttp, "get_remembered_tls_client_identity(request)");
  expect_contains(nvhttp, "resolve_known_client_uuid_from_launch_id");
  expect_contains(nvhttp, "Ignoring unmatched launch uniqueid for per-client settings");
  EXPECT_EQ(nvhttp.find("launch_session->client_uuid = get_arg(args, \"uniqueid\", \"\");"), std::string::npos);
  EXPECT_EQ(nvhttp.find("client_uuid = get_arg(args, \"uniqueid\", \"\");"), std::string::npos);
}

TEST(SunshineVirtualDisplayPackaging, BootstrapperPassesVirtualDisplayRemovalChoiceToMsi) {
  const auto bootstrapper = read_source_file("packaging/windows/bootstrapper/VibeshineInstaller.cs");

  expect_contains(bootstrapper, "\"--internal-uninstall-remove-virtual-display-driver\",");
  expect_contains(bootstrapper, "removeVirtualDisplayDriver ? \"1\" : \"0\"");
  expect_contains(bootstrapper, "\"REMOVEVIRTUALDISPLAYDRIVER=\" + (removeVirtualDisplayDriver ? \"1\" : \"0\")");
}

TEST(SunshineVirtualDisplayPackaging, InstallerKeepsSudoVdaDefaultAndSunshineDriverOptIn) {
  const auto cmake = read_source_file("cmake/packaging/windows.cmake");
  const auto actions = read_source_file("packaging/windows/wix/custom_actions.wxs");
  const auto patch = read_source_file("packaging/windows/wix/patch_custom_actions.wxs");
  const auto installer = read_source_file("src_assets/windows/drivers/sunshine/install.ps1");

  expect_contains(cmake, "drivers/sudovda");
  expect_contains(cmake, "drivers/sunshine");
  expect_contains(cmake, "Vibepollo Display Driver");
  expect_contains(actions, "SudoVdaRegistryDefaults");
  expect_contains(actions, "InstallSudovda");
  expect_contains(actions, "drivers\\sudovda\\install.ps1");
  expect_contains(actions, "drivers\\sunshine\\install.ps1");
  expect_contains(patch, "<Property Id=\"INSTALL_SUDOVDA\" Value=\"1\" Secure=\"yes\"/>");
  expect_contains(patch, "<Property Id=\"INSTALL_VIRTUAL_DISPLAY_DRIVER\" Value=\"0\" Secure=\"yes\"/>");
  expect_contains(patch, "INSTALL_SUDOVDA = \"1\"");
  expect_contains(patch, "INSTALL_VIRTUAL_DISPLAY_DRIVER = \"1\"");
  EXPECT_EQ(cmake.find("drivers/vdd"), std::string::npos);
  EXPECT_EQ(actions.find("drivers\\vdd"), std::string::npos);
  EXPECT_EQ(patch.find("drivers\\vdd"), std::string::npos);
  EXPECT_EQ(installer.find("SudoVDA"), std::string::npos);
  EXPECT_EQ(installer.find("MttVDD"), std::string::npos);
}

#endif
