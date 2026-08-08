include_guard(GLOBAL)

# This is the declarative source of truth for the virtual-display payload and
# installer policy. Windows packaging consumes the file lists and destinations
# below. The standalone component test configures the same values into a typed
# C++ view, so it never opens package scripts or payload files at runtime.
set(SUNSHINE_VDD_DRIVER_REQUIRED_FILES
    install.ps1
    SunshineVirtualDisplayDriver.inf
    SunshineVirtualDisplayDriver.dll
    SunshineVirtualDisplayDriver.cat
    nefconc.exe
    virtualdisplay_probe.exe)
set(SUNSHINE_VDD_DRIVER_OPTIONAL_FILES SunshineVirtualDisplayDriver.cer)
set(SUNSHINE_VDD_VULKAN_LAYER_FILES
    vulkan-layer/VkLayer_sunshine_hdr.dll
    vulkan-layer/VkLayer_sunshine_hdr.json)
set(SUNSHINE_VDD_TRUEHDR_FILES vibeshine_truehdr.dll nvngx_truehdr.dll)

set(SUNSHINE_VDD_DRIVER_DESTINATION "drivers/sunshine")
set(SUNSHINE_VDD_VULKAN_LAYER_DESTINATION "drivers/sunshine/vulkan-layer")
set(SUNSHINE_VDD_SUDOVDA_DESTINATION "drivers/sudovda")
set(SUNSHINE_VDD_TRUEHDR_REPOSITORY "Nonary/vibeshine_truehdr_runtime")
set(SUNSHINE_VDD_TRUEHDR_RELEASE_TAG "v1.0.0")
set(SUNSHINE_VDD_LIBVIRTUALDISPLAY_REPOSITORY "Nonary/libvirtualdisplay")
# Windows packaging always stages this pinned release before it refreshes the
# product payload. The local source checkout remains available for development,
# but is not used to build the driver as part of product packaging.
set(SUNSHINE_VDD_LIBVIRTUALDISPLAY_RELEASE_TAG "v1.6.3")
set(SUNSHINE_VDD_VULKAN_LAYER_NAME "VK_LAYER_SUNSHINE_virtual_hdr")
set(SUNSHINE_VDD_VULKAN_LAYER_LIBRARY ".\\VkLayer_sunshine_hdr.dll")
set(SUNSHINE_VDD_VULKAN_LAYER_DISABLE_ENVIRONMENT "DISABLE_SUNSHINE_VIRTUAL_HDR")

set(SUNSHINE_VDD_REFRESH_BEFORE_MSI ON)
set(SUNSHINE_VDD_PREBUILT_SCOPE "pinned_release")
set(SUNSHINE_VDD_LOCAL_SIGNING_MODE "self_signed_catalog")
set(SUNSHINE_VDD_INSTALLER_POWERSHELL_ARCHITECTURE "system64")
set(SUNSHINE_VDD_COMMAND_LINE_QUOTING "windows_backslash_quote")
set(SUNSHINE_VDD_DEFAULT_BACKEND "sunshine")
set(SUNSHINE_VDD_ROLLBACK_BACKEND "sudovda")
set(SUNSHINE_VDD_CONFLICT_PRODUCT_NAMES Sunshine Apollo Vibepollo Vibeshine)
set(SUNSHINE_VDD_MSI_INSTALL_OPERATIONS /i /package)
set(SUNSHINE_VDD_DRIVER_REBOOT_MARKERS
    VIRTUAL_DISPLAY_RESTART_REQUIRED
    "[SunshineVirtualDisplay] A reboot is required"
    "[SudoVDA] A reboot is required")
set(SUNSHINE_VDD_RENDER_ADAPTER_SELECTION_ORDER
    configured_adapter
    highest_dedicated_memory
    exclude_software_adapters)

set(SUNSHINE_VDD_HEALTH_CHECK_WIDTH 1920)
set(SUNSHINE_VDD_HEALTH_CHECK_HEIGHT 1080)
set(SUNSHINE_VDD_HEALTH_CHECK_REFRESH_HZ 60)
set(SUNSHINE_VDD_HEALTH_RECOVERY_SEQUENCE restart_device disable_device enable_device)
set(SUNSHINE_VDD_INSTALL_SEQUENCE
    install_files
    reset_acls
    install_sudovda
    install_sunshine_driver
    register_vulkan_layer
    migrate_config)
set(SUNSHINE_VDD_UNINSTALL_SEQUENCE
    restore_nvidia_preferences
    unregister_vulkan_layer
    uninstall_sudovda
    uninstall_sunshine_driver
    remove_files)

set(SUNSHINE_VDD_INSTALLER_BEST_EFFORT ON)
set(SUNSHINE_VDD_FORCE_KILL_UMDF OFF)
set(SUNSHINE_VDD_REMOVE_LEGACY_DRIVERS OFF)
set(SUNSHINE_VDD_REPLACE_ONLY_CHANGED_PACKAGE ON)
set(SUNSHINE_VDD_REQUIRE_VALID_CATALOG_SIGNATURE ON)
set(SUNSHINE_VDD_MSI_TRANSACTIONAL_REPLACEMENT ON)
set(SUNSHINE_VDD_MSI_ALLOW_DOWNGRADES ON)
set(SUNSHINE_VDD_CONFLICT_NAMES_EXACT ON)
set(SUNSHINE_VDD_FACTORY_RESET_REQUIRES_INSTALL_SENTINEL ON)
set(SUNSHINE_VDD_CI_REQUIRES_VALID_RELEASE_SIGNATURES ON)
set(SUNSHINE_VDD_CI_SELF_SIGN_WITHOUT_PERSISTENT_SECRET ON)
set(SUNSHINE_VDD_UPGRADE_SHOWS_DRIVER_CHOICE ON)
set(SUNSHINE_VDD_INSTALL_SELECTION_SEEDS_RUNTIME_FLAG ON)
set(SUNSHINE_VDD_CLI_PRESERVES_DRIVER_SELECTION ON)
set(SUNSHINE_VDD_UNINSTALL_PASSES_REMOVAL_CHOICE ON)
set(SUNSHINE_VDD_FORWARD_QUOTED_CLI_ARGUMENTS ON)
set(SUNSHINE_VDD_DIRECT_MSI_CONFLICTS_BLOCK ON)
set(SUNSHINE_VDD_ADMIN_INSTALL_PREUNINSTALL OFF)
set(SUNSHINE_VDD_DRIVER_REBOOT_RETURNS_3010 ON)
set(SUNSHINE_VDD_RUNTIME_SUDOVDA_FALLBACK_ENABLED ON)
