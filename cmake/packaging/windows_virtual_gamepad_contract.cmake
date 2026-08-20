include_guard(GLOBAL)

# The virtual-gamepad package is independently versioned and released from
# Nonary/libvirtualgamepad. Vibeshine consumes its immutable archive; it must
# never build the UMDF driver while assembling an application MSI.
#
# That archive is NOT production-signed, and cannot be. SignPath is authorised
# for this repository only, so libvirtualgamepad has no way to produce a
# release-signed catalogue. The driver is therefore signed downstream, as part
# of the same SignPath request that deep-signs the MSI: see
# docs/signpath/msi-file.artifact-config.xml.
#
# What that moves, and what it does not:
#   - Archive integrity is still pinned, by release tag and SHA-256 of the
#     downloaded asset. That is unchanged and remains the check that the bits
#     came from the pinned release.
#   - Authenticode verification moves from ingest to after signing. There is no
#     signature to check when the archive is unpacked, so requiring one there
#     would only be theatre.
#   - The signed catalogue is what makes the driver installable, and it is what
#     attests to VibeshineVhfGamepad.dll. The driver binary is deliberately
#     never Authenticode-signed: the catalogue hashes it, so signing it would
#     invalidate the catalogue.
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
# Optional. The upstream archive is unsigned, so these are only meaningful for
# an already-signed package supplied by hand; leave them empty for the normal
# flow and the ingest-time signature check is skipped.
set(SUNSHINE_VHF_GAMEPAD_CATALOG_SIGNER_THUMBPRINT "" CACHE STRING
    "Optional expected Authenticode signer thumbprint for a pre-signed VHF catalog.")
set(SUNSHINE_VHF_GAMEPAD_DEVICE_SETUP_SIGNER_THUMBPRINT "" CACHE STRING
    "Optional expected Authenticode signer thumbprint for a pre-signed VHF root-device setup tool.")
set(SUNSHINE_VHF_GAMEPAD_PREBUILT_SCOPE "pinned_release")
# Where the production signature comes from. "msi_request" means the catalogue
# and the root-device tool are signed inside the Vibeshine MSI signing request
# rather than arriving signed.
set(SUNSHINE_VHF_GAMEPAD_SIGNING_MODE "msi_request")
set(SUNSHINE_VHF_GAMEPAD_LOCAL_SIGNING_MODE "explicit_local_test_only")
set(SUNSHINE_VHF_GAMEPAD_INSTALLER_POWERSHELL_ARCHITECTURE "system64")
set(SUNSHINE_VHF_GAMEPAD_REFRESH_BEFORE_MSI ON)
# Both OFF because the archive is unsigned when it is unpacked. The signature
# these once demanded is applied later, by the MSI signing request.
set(SUNSHINE_VHF_GAMEPAD_REQUIRE_VALID_CATALOG_SIGNATURE OFF)
set(SUNSHINE_VHF_GAMEPAD_REQUIRE_SIGNED_SETUP_TOOL OFF)
set(SUNSHINE_VHF_GAMEPAD_DRIVER_REBOOT_MARKER "VIRTUAL_GAMEPAD_RESTART_REQUIRED")
set(SUNSHINE_VHF_GAMEPAD_INSTALLER_BEST_EFFORT ON)
set(SUNSHINE_VHF_GAMEPAD_REMOVE_ROOT_ON_FINAL_UNINSTALL ON)
set(SUNSHINE_VHF_GAMEPAD_REMOVE_DRIVERSTORE_ONLY_ON_REQUEST ON)

# Keep the payload opt-in until a release pins a tag and its SHA-256. The
# gate is no longer "has upstream signed it", because upstream cannot; it is
# "is there a pinned archive to consume".
option(SUNSHINE_BUNDLE_VHF_GAMEPAD_DRIVER
       "Bundle the pinned libvirtualgamepad UMDF/VHF package in the Windows installer."
       OFF)
option(SUNSHINE_ALLOW_LOCAL_VHF_GAMEPAD_TEST_PACKAGE
       "Allow an explicitly supplied local self-signed VHF gamepad package for development packaging."
       OFF)
