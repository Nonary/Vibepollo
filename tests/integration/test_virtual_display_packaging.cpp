/**
 * @file tests/integration/test_virtual_display_packaging.cpp
 * @brief Tests for Vibepollo Display Driver packaging invariants.
 */
#include <gtest/gtest.h>

#ifdef _WIN32
  #include <virtual_display_package_contract.generated.h>

  #include <algorithm>
  #include <array>
  #include <ranges>
  #include <string_view>

namespace contract = sunshine::virtual_display_package_contract;

namespace {
  template <typename Range>
  bool contains(const Range &range, const std::string_view value) {
    return std::ranges::find(range, value) != range.end();
  }

  template <typename Range>
  std::size_t position(const Range &range, const std::string_view value) {
    return static_cast<std::size_t>(std::ranges::find(range, value) - range.begin());
  }
}  // namespace

TEST(SunshineVirtualDisplayPackaging, PackageTargetDownloadsPinnedDriverAssets) {
  EXPECT_TRUE(contract::refresh_before_msi);
  EXPECT_EQ(contract::prebuilt_scope, "pinned_release");
  EXPECT_EQ(contract::local_signing_mode, "self_signed_catalog");
}

TEST(SunshineVirtualDisplayPackaging, RequiredPayloadIsTypedAndInstalledInDedicatedDirectories) {
  EXPECT_TRUE(contains(contract::required_driver_files, "install.ps1"));
  EXPECT_TRUE(contains(contract::required_driver_files, "SunshineVirtualDisplayDriver.inf"));
  EXPECT_TRUE(contains(contract::required_driver_files, "SunshineVirtualDisplayDriver.dll"));
  EXPECT_TRUE(contains(contract::required_driver_files, "SunshineVirtualDisplayDriver.cat"));
  EXPECT_TRUE(contains(contract::required_driver_files, "virtualdisplay_probe.exe"));
  EXPECT_TRUE(contains(contract::optional_driver_files, "SunshineVirtualDisplayDriver.cer"));
  EXPECT_EQ(contract::driver_destination, "drivers/sunshine");
  EXPECT_EQ(contract::vulkan_layer_destination, "drivers/sunshine/vulkan-layer");
  EXPECT_EQ(contract::sudovda_destination, "drivers/sudovda");
}

TEST(SunshineVirtualDisplayPackaging, VulkanHdrLayerContractIsOptOutAndCoLocated) {
  EXPECT_TRUE(contains(contract::vulkan_layer_files, "vulkan-layer/VkLayer_sunshine_hdr.dll"));
  EXPECT_TRUE(contains(contract::vulkan_layer_files, "vulkan-layer/VkLayer_sunshine_hdr.json"));
  EXPECT_EQ(contract::vulkan_layer_name, "VK_LAYER_SUNSHINE_virtual_hdr");
  EXPECT_EQ(contract::vulkan_layer_library, ".\\VkLayer_sunshine_hdr.dll");
  EXPECT_EQ(contract::vulkan_layer_disable_environment, "DISABLE_SUNSHINE_VIRTUAL_HDR");
}

TEST(SunshineVirtualDisplayPackaging, TrueHdrRuntimeIsRequiredAndPinned) {
  EXPECT_EQ(contract::truehdr_repository, "Nonary/vibeshine_truehdr_runtime");
  EXPECT_EQ(contract::truehdr_release_tag, "v1.0.0");
  EXPECT_TRUE(contains(contract::truehdr_files, "vibeshine_truehdr.dll"));
  EXPECT_TRUE(contains(contract::truehdr_files, "nvngx_truehdr.dll"));
}

TEST(SunshineVirtualDisplayPackaging, InstallerUsesBoundedTemporaryDisplayHealthCheck) {
  EXPECT_EQ(contract::health_check_width, 1920u);
  EXPECT_EQ(contract::health_check_height, 1080u);
  EXPECT_EQ(contract::health_check_refresh_hz, 60u);
  const auto expected = std::to_array<std::string_view>({
    "restart_device", "disable_device", "enable_device"
  });
  EXPECT_EQ(contract::health_recovery_sequence, expected);
  EXPECT_TRUE(contract::installer_best_effort);
  EXPECT_FALSE(contract::force_kill_umdf);
}

TEST(SunshineVirtualDisplayPackaging, InstallerReplacesOnlyChangedSunshinePackage) {
  EXPECT_TRUE(contract::replace_only_changed_package);
  EXPECT_TRUE(contract::require_valid_catalog_signature);
  EXPECT_FALSE(contract::remove_legacy_drivers);
}

TEST(SunshineVirtualDisplayPackaging, WixUsesSystem64PowerShellAndSemanticActionOrder) {
  EXPECT_EQ(contract::installer_powershell_architecture, "system64");
  EXPECT_LT(position(contract::install_sequence, "install_files"), position(contract::install_sequence, "reset_acls"));
  EXPECT_LT(position(contract::install_sequence, "install_sudovda"), position(contract::install_sequence, "install_sunshine_driver"));
  EXPECT_LT(position(contract::install_sequence, "install_sunshine_driver"), position(contract::install_sequence, "register_vulkan_layer"));
  EXPECT_LT(position(contract::install_sequence, "register_vulkan_layer"), position(contract::install_sequence, "migrate_config"));
}

TEST(SunshineVirtualDisplayPackaging, UninstallUnregistersLayersAndDriversBeforeFileRemoval) {
  EXPECT_LT(position(contract::uninstall_sequence, "restore_nvidia_preferences"), position(contract::uninstall_sequence, "unregister_vulkan_layer"));
  EXPECT_LT(position(contract::uninstall_sequence, "unregister_vulkan_layer"), position(contract::uninstall_sequence, "uninstall_sudovda"));
  EXPECT_LT(position(contract::uninstall_sequence, "uninstall_sunshine_driver"), position(contract::uninstall_sequence, "remove_files"));
  EXPECT_TRUE(contract::uninstall_passes_removal_choice);
}

TEST(SunshineVirtualDisplayPackaging, MsiReplacementAndConflictPoliciesAreTransactional) {
  EXPECT_TRUE(contract::msi_transactional_replacement);
  EXPECT_TRUE(contract::msi_allow_downgrades);
  EXPECT_TRUE(contract::conflict_names_exact);
  EXPECT_TRUE(contract::factory_reset_requires_install_sentinel);
}

TEST(SunshineVirtualDisplayPackaging, BootstrapperForwardsArgumentsWithWindowsQuoteEscaping) {
  EXPECT_TRUE(contract::forward_quoted_cli_arguments);
  EXPECT_EQ(contract::command_line_quoting, "windows_backslash_quote");
}

TEST(SunshineVirtualDisplayPackaging, ConflictProductsUseExactDisplayNames) {
  EXPECT_TRUE(contract::conflict_names_exact);
  EXPECT_EQ(contract::conflict_product_names, std::to_array<std::string_view>({
    "Sunshine", "Apollo", "Vibepollo", "Vibeshine"
  }));
}

TEST(SunshineVirtualDisplayPackaging, DirectMsiConflictHandlingBlocksInsteadOfRemovingProducts) {
  EXPECT_TRUE(contract::direct_msi_conflicts_block);
}

TEST(SunshineVirtualDisplayPackaging, PreUninstallIsLimitedToInstallOperations) {
  EXPECT_EQ(contract::msi_install_operations, std::to_array<std::string_view>({"/i", "/package"}));
}

TEST(SunshineVirtualDisplayPackaging, AdministrativeInstallNeverPreUninstallsProducts) {
  EXPECT_FALSE(contract::admin_install_preuninstall);
}

TEST(SunshineVirtualDisplayPackaging, DriverRestartWarningsHaveExplicitMarkers) {
  EXPECT_EQ(contract::driver_reboot_markers, std::to_array<std::string_view>({
    "VIRTUAL_DISPLAY_RESTART_REQUIRED",
    "[SunshineVirtualDisplay] A reboot is required",
    "[SudoVDA] A reboot is required"
  }));
}

TEST(SunshineVirtualDisplayPackaging, DriverRestartWarningUsesMsiRebootExitCode) {
  EXPECT_TRUE(contract::driver_reboot_returns_3010);
}

TEST(SunshineVirtualDisplayPackaging, RuntimeOffersSudoVdaAsTheExplicitFallbackBackend) {
  EXPECT_TRUE(contract::runtime_sudovda_fallback_enabled);
  EXPECT_EQ(contract::rollback_backend, "sudovda");
}

TEST(SunshineVirtualDisplayPackaging, RenderAdapterSelectionUsesConfiguredThenDedicatedHardware) {
  EXPECT_EQ(contract::render_adapter_selection_order, std::to_array<std::string_view>({
    "configured_adapter", "highest_dedicated_memory", "exclude_software_adapters"
  }));
}

TEST(SunshineVirtualDisplayPackaging, DriverRefreshConsumesPinnedLibvirtualdisplayRelease) {
  EXPECT_EQ(contract::libvirtualdisplay_repository, "Nonary/libvirtualdisplay");
  EXPECT_EQ(contract::libvirtualdisplay_release_tag, "v1.6.3");
}

TEST(SunshineVirtualDisplayPackaging, DriverRefreshUsesThePinnedReleasePayload) {
  EXPECT_EQ(contract::prebuilt_scope, "pinned_release");
  EXPECT_TRUE(contract::refresh_before_msi);
}

TEST(SunshineVirtualDisplayPackaging, RuntimeDriverChoiceSeedsTheInstallerPreference) {
  EXPECT_TRUE(contract::install_selection_seeds_runtime_flag);
  EXPECT_TRUE(contract::cli_preserves_driver_selection);
}

TEST(SunshineVirtualDisplayPackaging, CiSigningPolicyAllowsEphemeralDriverCertificateOnly) {
  EXPECT_TRUE(contract::ci_requires_valid_release_signatures);
  EXPECT_TRUE(contract::ci_self_sign_without_persistent_secret);
}

TEST(SunshineVirtualDisplayPackaging, InstallerKeepsSudoVdaRollbackAndSunshineDriverDefault) {
  EXPECT_EQ(contract::default_backend, "sunshine");
  EXPECT_EQ(contract::rollback_backend, "sudovda");
  EXPECT_TRUE(contract::upgrade_shows_driver_choice);
  EXPECT_TRUE(contract::install_selection_seeds_runtime_flag);
  EXPECT_TRUE(contract::cli_preserves_driver_selection);
}

#endif
