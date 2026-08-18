include_guard(GLOBAL)

# The virtual-gamepad package is independently versioned and released from
# Nonary/libvirtualgamepad. Vibeshine consumes its immutable signed archive; it
# must never build the UMDF driver while assembling an application MSI.
set(SUNSHINE_VHF_GAMEPAD_REQUIRED_FILES
    install.ps1
    driver/VibeshineVhfGamepad.inf
    driver/VibeshineVhfGamepad.dll
    driver/VibeshineVhfGamepad.cat
    tools/VibeshineVhfGamepadDeviceSetup.exe
    manifest.json
    release-lock.json)
set(SUNSHINE_VHF_GAMEPAD_DRIVER_DESTINATION "drivers/vhf-gamepad")
set(SUNSHINE_VHF_GAMEPAD_REPOSITORY "Nonary/libvirtualgamepad")
set(SUNSHINE_VHF_GAMEPAD_RELEASE_TAG "" CACHE STRING
    "Pinned libvirtualgamepad release tag. Leave empty until the first production-signed release exists.")
set(SUNSHINE_VHF_GAMEPAD_RELEASE_ASSET_SHA256 "" CACHE STRING
    "SHA-256 of the pinned libvirtualgamepad Windows x64 release archive.")
set(SUNSHINE_VHF_GAMEPAD_CATALOG_SIGNER_THUMBPRINT "" CACHE STRING
    "Expected Authenticode signer thumbprint for the pinned libvirtualgamepad catalog.")
set(SUNSHINE_VHF_GAMEPAD_DEVICE_SETUP_SIGNER_THUMBPRINT "" CACHE STRING
    "Expected Authenticode signer thumbprint for the pinned libvirtualgamepad root-device setup tool.")
set(SUNSHINE_VHF_GAMEPAD_PREBUILT_SCOPE "pinned_release")
set(SUNSHINE_VHF_GAMEPAD_LOCAL_SIGNING_MODE "explicit_local_test_only")
set(SUNSHINE_VHF_GAMEPAD_INSTALLER_POWERSHELL_ARCHITECTURE "system64")
set(SUNSHINE_VHF_GAMEPAD_REFRESH_BEFORE_MSI ON)
set(SUNSHINE_VHF_GAMEPAD_REQUIRE_VALID_CATALOG_SIGNATURE ON)
set(SUNSHINE_VHF_GAMEPAD_REQUIRE_SIGNED_SETUP_TOOL ON)
set(SUNSHINE_VHF_GAMEPAD_DRIVER_REBOOT_MARKER "VIRTUAL_GAMEPAD_RESTART_REQUIRED")
set(SUNSHINE_VHF_GAMEPAD_INSTALLER_BEST_EFFORT ON)
set(SUNSHINE_VHF_GAMEPAD_REMOVE_ROOT_ON_FINAL_UNINSTALL ON)
set(SUNSHINE_VHF_GAMEPAD_REMOVE_DRIVERSTORE_ONLY_ON_REQUEST ON)

# There is no immutable production archive yet. Keep the new payload opt-in
# until its catalog and root-device tool have completed the independent release
# signing flow. A release can enable this option by default when it pins a tag.
option(SUNSHINE_BUNDLE_VHF_GAMEPAD_DRIVER
       "Bundle the pinned libvirtualgamepad UMDF/VHF package in the Windows installer."
       OFF)
option(SUNSHINE_ALLOW_LOCAL_VHF_GAMEPAD_TEST_PACKAGE
       "Allow an explicitly supplied local self-signed VHF gamepad package for development packaging."
       OFF)
