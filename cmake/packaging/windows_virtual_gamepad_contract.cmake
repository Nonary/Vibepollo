include_guard(GLOBAL)

function(sunshine_vhf_prepare_package_stage output_variable local_test source_dir binary_dir)
    if(local_test)
        # A local test driver must not replace the checked-in release payload.
        set(_stage "${binary_dir}/local-vhf-gamepad")
        file(MAKE_DIRECTORY "${_stage}")
        configure_file("${source_dir}/install.ps1" "${_stage}/install.ps1" COPYONLY)
        configure_file("${source_dir}/cleanup.ps1" "${_stage}/cleanup.ps1" COPYONLY)
    else()
        set(_stage "${source_dir}")
    endif()
    set(${output_variable} "${_stage}" PARENT_SCOPE)
endfunction()

function(sunshine_vhf_is_fixed_length_hex output_variable value expected_length)
    if(NOT expected_length MATCHES "^[0-9]+$" OR expected_length LESS 1)
        message(FATAL_ERROR "VHF hexadecimal validation requires a positive expected length.")
    endif()

    string(LENGTH "${value}" actual_length)
    if(actual_length EQUAL expected_length AND value MATCHES "^[0-9a-fA-F]+$")
        set(${output_variable} TRUE PARENT_SCOPE)
    else()
        set(${output_variable} FALSE PARENT_SCOPE)
    endif()
endfunction()

# The virtual-gamepad package is independently versioned and released from
# Nonary/libvirtualgamepad. The application consumes its immutable archive; it
# must never build either driver while assembling a consumer MSI.
#
# The producer archive is intentionally unsigned. Production signing remains
# downstream: the same consumer-MSI signing request that deep-signs the MSI
# signs its VHF catalog and root-device setup tool. See docs/signpath/ for the
# repository-specific executable signing configuration.
#
# What that moves, and what it does not:
#   - Archive integrity is still pinned, by release tag and SHA-256 of the
#     downloaded asset. That is unchanged and remains the check that the bits
#     came from the pinned release.
#   - Ingest proves the CAT, DLL, and setup tool have Authenticode status
#     NotSigned with no signer. After SignPath, the CAT and setup tool must have
#     intact signatures while the catalog-bound DLL must remain NotSigned.
#   - The signed catalogue is what makes the driver installable, and it is what
#     attests to VibeshineVhfGamepad.dll. The driver binary is deliberately
#     never Authenticode-signed: the catalogue hashes it, so signing it would
#     invalidate the catalogue.
set(SUNSHINE_VHF_GAMEPAD_REQUIRED_FILES
    install.ps1
    cleanup.ps1
    driver/VibeshineVhfGamepad.inf
    driver/VibeshineVhfGamepad.dll
    driver/VibeshineVhfGamepad.cat
    tools/VibeshineVhfGamepadDeviceSetup.exe
    manifest.json
    release-lock.json)
set(SUNSHINE_VHF_GAMEPAD_DRIVER_DESTINATION "drivers/vhf-gamepad")
set(SUNSHINE_VHF_GAMEPAD_REPOSITORY "Nonary/libvirtualgamepad")
set(SUNSHINE_VHF_GAMEPAD_RELEASE_TAG "v0.1.0-beta.11" CACHE STRING
    "Pinned immutable libvirtualgamepad release tag.")
set(SUNSHINE_VHF_GAMEPAD_RELEASE_ASSET_SHA256
    "a23716e933753b9eef8e87675c388cc326ff61c0ef943e1c1d99ba6e7e28992c" CACHE STRING
    "SHA-256 of the pinned libvirtualgamepad Windows x64 release archive.")
set(SUNSHINE_VHF_GAMEPAD_SOURCE_REVISION
    "93dc007a608b341aa998378221f8958f045ee7f2" CACHE STRING
    "Commit targeted by the pinned lightweight libvirtualgamepad release tag.")
set(SUNSHINE_VHF_GAMEPAD_DRIVER_VER "11/14/2026,0.1.0.96" CACHE STRING
    "DriverVer recorded by the pinned libvirtualgamepad release.")
set(SUNSHINE_VHF_GAMEPAD_PROTOCOL_VERSION 2 CACHE STRING
    "Protocol version recorded by the pinned libvirtualgamepad release.")
# Optional. The upstream archive is unsigned, so these are only meaningful for
# an already-signed package supplied by hand; leave them empty for the normal
# flow and the ingest-time signature check is skipped.
set(SUNSHINE_VHF_GAMEPAD_CATALOG_SIGNER_THUMBPRINT "" CACHE STRING
    "Optional expected Authenticode signer thumbprint for a pre-signed VHF catalog.")
set(SUNSHINE_VHF_GAMEPAD_DEVICE_SETUP_SIGNER_THUMBPRINT "" CACHE STRING
    "Optional expected Authenticode signer thumbprint for a pre-signed VHF root-device setup tool.")
set(SUNSHINE_VHF_GAMEPAD_PREBUILT_SCOPE "pinned_release")
# Where the production signature comes from. "msi_request" means the catalogue
# and the root-device tool are signed inside the consumer MSI signing request
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

# Local packaging remains opt-in. Release CI supplies the already downloaded,
# independently validated producer artifact and enables the bundle explicitly.
option(SUNSHINE_BUNDLE_VHF_GAMEPAD_DRIVER
       "Bundle the pinned libvirtualgamepad UMDF/VHF package in the Windows installer."
       OFF)
option(SUNSHINE_ALLOW_LOCAL_VHF_GAMEPAD_TEST_PACKAGE
       "Allow an explicitly supplied local self-signed VHF gamepad package for development packaging."
       OFF)

# Legacy immutable pins intentionally remain gamepad-only. A candidate advertised
# as fixing ordered mouse delivery requires this default-on gate and the actual
# published mapper release/pins, or an explicit local test package.
option(SUNSHINE_REQUIRE_VHF_MOUSE_MAPPER_PACKAGE
       "Require an explicit mapper feature and complete unsigned mapper payload in the bundled candidate."
       ON)
set(SUNSHINE_VHF_MOUSE_MAPPER_FILES
    mouse_mapper/VibeshineVhfMouseMapper.inf
    mouse_mapper/VibeshineVhfMouseMapper.sys
    mouse_mapper/VibeshineVhfMouseMapper.cat)
# The producer mapper INF/SYS/CAT remain unsigned and byte-pinned. Consumer
# MSI signing remains limited to the UMDF CAT and setup EXE. The final installer
# still requires Windows kernel trust; unsigned publication does not grant it.
set(SUNSHINE_VHF_MOUSE_MAPPER_SIGNING_MODE "unsigned_producer")
