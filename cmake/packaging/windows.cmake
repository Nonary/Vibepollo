# windows specific packaging
include("${CMAKE_SOURCE_DIR}/cmake/packaging/windows_virtual_display_contract.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/packaging/windows_virtual_gamepad_contract.cmake")

install(TARGETS sunshine RUNTIME DESTINATION "." COMPONENT application)

# Hardening: include zlib1.dll (loaded via LoadLibrary() in openssl's libcrypto.a)
# Check for zlib in Sunshine or Apollo install directories
if(EXISTS "${ZLIB}")
    install(FILES "${ZLIB}" DESTINATION "." COMPONENT application)
elseif(EXISTS "C:/Program Files (x86)/Sunshine/zlib1.dll")
    install(FILES "C:/Program Files (x86)/Sunshine/zlib1.dll" DESTINATION "." COMPONENT application)
elseif(EXISTS "C:/Program Files/Apollo/zlib1.dll")
    install(FILES "C:/Program Files/Apollo/zlib1.dll" DESTINATION "." COMPONENT application)
else()
    message(FATAL_ERROR "zlib1.dll not found in expected locations")
endif()

if(WEBRTC_RUNTIME_DLL)
    install(FILES "${WEBRTC_RUNTIME_DLL}" DESTINATION "." COMPONENT application)
endif()

# NVIDIA TrueHDR runtime. Installer builds stage the pinned runtime at CPack
# time so RTX HDR cannot be shipped in a silently disabled state. Force the
# cache value on so older local build trees do not keep the previous optional
# default. Only the TrueHDR feature DLL is bundled; VSR is not used.
set(SUNSHINE_REQUIRE_TRUEHDR_RUNTIME ON CACHE BOOL "Fail Windows packaging when the TrueHDR runtime DLLs are missing." FORCE)
set(SUNSHINE_TRUEHDR_RUNTIME_DIR "${CMAKE_BINARY_DIR}/truehdr-runtime" CACHE PATH "Directory containing vibeshine_truehdr.dll and the NVIDIA NGX TrueHDR runtime DLL")
if("${SUNSHINE_TRUEHDR_RUNTIME_DIR}" STREQUAL "${CMAKE_BINARY_DIR}")
    set(SUNSHINE_TRUEHDR_RUNTIME_DIR "${CMAKE_BINARY_DIR}/truehdr-runtime" CACHE PATH "Directory containing vibeshine_truehdr.dll and the NVIDIA NGX TrueHDR runtime DLL" FORCE)
endif()
set(SUNSHINE_TRUEHDR_RUNTIME_FILES "")
foreach(_truehdr_runtime_name IN LISTS SUNSHINE_VDD_TRUEHDR_FILES)
    list(APPEND SUNSHINE_TRUEHDR_RUNTIME_FILES
        "${SUNSHINE_TRUEHDR_RUNTIME_DIR}/${_truehdr_runtime_name}")
endforeach()
unset(_truehdr_runtime_name)
if(SUNSHINE_REQUIRE_TRUEHDR_RUNTIME)
    foreach(_truehdr_runtime_file IN LISTS SUNSHINE_TRUEHDR_RUNTIME_FILES)
        if(NOT EXISTS "${_truehdr_runtime_file}")
            message(FATAL_ERROR "Required TrueHDR runtime file missing: ${_truehdr_runtime_file}")
        endif()
        file(SIZE "${_truehdr_runtime_file}" _truehdr_runtime_file_size)
        if(_truehdr_runtime_file_size EQUAL 0)
            message(FATAL_ERROR "Required TrueHDR runtime file is empty (0 bytes): ${_truehdr_runtime_file}")
        endif()
    endforeach()
    unset(_truehdr_runtime_file_size)
    unset(_truehdr_runtime_file)
    install(FILES ${SUNSHINE_TRUEHDR_RUNTIME_FILES}
        DESTINATION "."
        COMPONENT application)
else()
    install(FILES ${SUNSHINE_TRUEHDR_RUNTIME_FILES}
        DESTINATION "."
        COMPONENT application
        OPTIONAL)
endif()

# ARM64: include minhook-detours DLL (shared library for ARM64)
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64" AND DEFINED _MINHOOK_DLL)
    install(FILES "${_MINHOOK_DLL}" DESTINATION "." COMPONENT application)
endif()

# ViGEmBus installer is no longer bundled or managed by the installer

# Adding tools
install(TARGETS dxgi-info RUNTIME DESTINATION "tools" COMPONENT dxgi)
install(TARGETS audio-info RUNTIME DESTINATION "tools" COMPONENT audio)

# Helpers and tools
# - Playnite launcher helper used for Playnite-managed app launches
# - WGC capture helper used by the WGC display backend
# - Display helper used for applying/reverting display settings
if (TARGET playnite-launcher)
    install(TARGETS playnite-launcher RUNTIME DESTINATION "tools" COMPONENT application)
endif()
if (TARGET sunshine_wgc_capture)
    install(TARGETS sunshine_wgc_capture RUNTIME DESTINATION "tools" COMPONENT application)
endif()
if (TARGET sunshine_display_helper)
    install(TARGETS sunshine_display_helper RUNTIME DESTINATION "tools" COMPONENT application)
endif()
install(FILES "${CMAKE_BINARY_DIR}/uninstall.exe" DESTINATION "." COMPONENT application)

# Drivers (SudoVDA virtual display)
set(SUDOVDA_SOURCE_DIR "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/drivers/sudovda")
set(SUDOVDA_DRIVER_FILES
    "${SUDOVDA_SOURCE_DIR}/install.ps1"
    "${SUDOVDA_SOURCE_DIR}/uninstall.bat"
    "${SUDOVDA_SOURCE_DIR}/SudoVDA.inf"
    "${SUDOVDA_SOURCE_DIR}/SudoVDA.dll"
    "${SUDOVDA_SOURCE_DIR}/sudovda.cat"
    "${SUDOVDA_SOURCE_DIR}/sudovda.cer"
    "${SUDOVDA_SOURCE_DIR}/nefconc.exe"
)

foreach(_sudovda_file IN LISTS SUDOVDA_DRIVER_FILES)
    if (NOT EXISTS "${_sudovda_file}")
        message(FATAL_ERROR "Required SudoVDA driver artifact missing: ${_sudovda_file}")
    endif()
    file(SIZE "${_sudovda_file}" _sudovda_file_size)
    if (_sudovda_file_size EQUAL 0)
        message(FATAL_ERROR "Required SudoVDA driver artifact is empty (0 bytes): ${_sudovda_file}")
    endif()
endforeach()
unset(_sudovda_file_size)
unset(_sudovda_file)

install(FILES ${SUDOVDA_DRIVER_FILES}
        DESTINATION "${SUNSHINE_VDD_SUDOVDA_DESTINATION}"
        COMPONENT sudovda)

# Drivers (Vibepollo Display Driver)
set(SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/drivers/sunshine")
set(SUNSHINE_VIRTUAL_DISPLAY_DRIVER_REFRESH_SCRIPT "${CMAKE_SOURCE_DIR}/packaging/windows/virtual_display_driver/refresh_driver_package.ps1")
set(SUNSHINE_VIRTUAL_DISPLAY_DRIVER_DOWNLOAD_SCRIPT "${CMAKE_SOURCE_DIR}/scripts/download_libvirtualdisplay_release.ps1")
set(SUNSHINE_LIBVIRTUALDISPLAY_PREBUILT_DIR "" CACHE PATH "Optional prebuilt libvirtualdisplay package root with driver/, tools/, and vulkan-layer/")
if(SUNSHINE_LIBVIRTUALDISPLAY_PREBUILT_DIR)
    set(SUNSHINE_EFFECTIVE_LIBVIRTUALDISPLAY_PREBUILT_DIR "${SUNSHINE_LIBVIRTUALDISPLAY_PREBUILT_DIR}")
    set(SUNSHINE_DOWNLOAD_LIBVIRTUALDISPLAY_RELEASE OFF)
else()
    set(SUNSHINE_EFFECTIVE_LIBVIRTUALDISPLAY_PREBUILT_DIR
        "${CMAKE_BINARY_DIR}/libvirtualdisplay-release-${SUNSHINE_VDD_LIBVIRTUALDISPLAY_RELEASE_TAG}")
    set(SUNSHINE_DOWNLOAD_LIBVIRTUALDISPLAY_RELEASE ON)
endif()
set(SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SIGNING_ARGS "")
set(SUNSHINE_VIRTUAL_DISPLAY_DRIVER_FILES "")
foreach(_sunshine_driver_relative_file IN LISTS SUNSHINE_VDD_DRIVER_REQUIRED_FILES)
    list(APPEND SUNSHINE_VIRTUAL_DISPLAY_DRIVER_FILES
        "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}/${_sunshine_driver_relative_file}")
endforeach()
unset(_sunshine_driver_relative_file)

set(SUNSHINE_VIRTUAL_DISPLAY_VULKAN_LAYER_FILES "")
foreach(_sunshine_vulkan_relative_file IN LISTS SUNSHINE_VDD_VULKAN_LAYER_FILES)
    list(APPEND SUNSHINE_VIRTUAL_DISPLAY_VULKAN_LAYER_FILES
        "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}/${_sunshine_vulkan_relative_file}")
endforeach()
unset(_sunshine_vulkan_relative_file)
set(SUNSHINE_VIRTUAL_DISPLAY_PACKAGE_FILES
    ${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_FILES}
    ${SUNSHINE_VIRTUAL_DISPLAY_VULKAN_LAYER_FILES}
)
foreach(_sunshine_driver_optional_name IN LISTS SUNSHINE_VDD_DRIVER_OPTIONAL_FILES)
    set(_sunshine_driver_optional_file
        "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}/${_sunshine_driver_optional_name}")
    if(EXISTS "${_sunshine_driver_optional_file}")
        list(APPEND SUNSHINE_VIRTUAL_DISPLAY_DRIVER_FILES "${_sunshine_driver_optional_file}")
        list(APPEND SUNSHINE_VIRTUAL_DISPLAY_PACKAGE_FILES "${_sunshine_driver_optional_file}")
    endif()
endforeach()
unset(_sunshine_driver_optional_file)
unset(_sunshine_driver_optional_name)

foreach(_sunshine_driver_file IN LISTS SUNSHINE_VIRTUAL_DISPLAY_PACKAGE_FILES)
    if (NOT EXISTS "${_sunshine_driver_file}")
        message(FATAL_ERROR "Required Vibepollo Display Driver artifact missing: ${_sunshine_driver_file}")
    endif()
    file(SIZE "${_sunshine_driver_file}" _sunshine_driver_file_size)
    if (_sunshine_driver_file_size EQUAL 0)
        message(FATAL_ERROR "Required Vibepollo Display Driver artifact is empty (0 bytes): ${_sunshine_driver_file}")
    endif()
endforeach()
unset(_sunshine_driver_file_size)
unset(_sunshine_driver_file)

if(EXISTS "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_REFRESH_SCRIPT}")
    if(SUNSHINE_DOWNLOAD_LIBVIRTUALDISPLAY_RELEASE)
        if(NOT EXISTS "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_DOWNLOAD_SCRIPT}")
            message(FATAL_ERROR "Required libvirtualdisplay release downloader is missing: ${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_DOWNLOAD_SCRIPT}")
        endif()
        add_custom_target(download_sunshine_virtual_display_driver_release
            COMMAND powershell -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass
                    -File "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_DOWNLOAD_SCRIPT}"
                    -Repository "${SUNSHINE_VDD_LIBVIRTUALDISPLAY_REPOSITORY}"
                    -Tag "${SUNSHINE_VDD_LIBVIRTUALDISPLAY_RELEASE_TAG}"
                    -OutDir "${SUNSHINE_EFFECTIVE_LIBVIRTUALDISPLAY_PREBUILT_DIR}"
            DEPENDS "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_DOWNLOAD_SCRIPT}"
            COMMENT "Downloading pinned Vibepollo Display Driver release"
            VERBATIM)
    endif()

    add_custom_target(validate_sunshine_virtual_display_driver_assets
        COMMAND powershell -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass
                -File "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_REFRESH_SCRIPT}"
                -ValidateOnly
                -LibVirtualDisplayDir "${SUNSHINE_LIBVIRTUALDISPLAY_SOURCE_DIR}"
                -PrebuiltPackageDir "${SUNSHINE_EFFECTIVE_LIBVIRTUALDISPLAY_PREBUILT_DIR}"
                -PackageDir "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}"
        DEPENDS "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_REFRESH_SCRIPT}"
                ${SUNSHINE_VIRTUAL_DISPLAY_PACKAGE_FILES}
        COMMENT "Validating Vibepollo Display Driver package assets"
        VERBATIM)

    if(SUNSHINE_DOWNLOAD_LIBVIRTUALDISPLAY_RELEASE)
        add_dependencies(validate_sunshine_virtual_display_driver_assets
            download_sunshine_virtual_display_driver_release)
    endif()

    add_custom_target(refresh_sunshine_virtual_display_driver_assets
        COMMAND powershell -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass
                -File "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_REFRESH_SCRIPT}"
                -Build
                -LibVirtualDisplayDir "${SUNSHINE_LIBVIRTUALDISPLAY_SOURCE_DIR}"
                -PrebuiltPackageDir "${SUNSHINE_EFFECTIVE_LIBVIRTUALDISPLAY_PREBUILT_DIR}"
                -PackageDir "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}"
                ${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SIGNING_ARGS}
        DEPENDS "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_REFRESH_SCRIPT}"
        COMMENT "Refreshing Vibepollo Display Driver package assets from the pinned release"
        VERBATIM)

    if(SUNSHINE_DOWNLOAD_LIBVIRTUALDISPLAY_RELEASE)
        add_dependencies(refresh_sunshine_virtual_display_driver_assets
            download_sunshine_virtual_display_driver_release)
    endif()

    if(TARGET package_msi AND SUNSHINE_VDD_REFRESH_BEFORE_MSI)
        add_dependencies(package_msi refresh_sunshine_virtual_display_driver_assets)
    endif()
endif()

install(FILES ${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_FILES}
        DESTINATION "${SUNSHINE_VDD_DRIVER_DESTINATION}"
        COMPONENT virtual_display_driver)
install(FILES ${SUNSHINE_VIRTUAL_DISPLAY_VULKAN_LAYER_FILES}
        DESTINATION "${SUNSHINE_VDD_VULKAN_LAYER_DESTINATION}"
        COMPONENT virtual_display_driver)

# Drivers (Vibepollo VHF virtual gamepad)
#
# This is intentionally independent from the display-driver refresh flow. The
# gamepad package is an immutable libvirtualgamepad producer release. It arrives
# unsigned; the MSI SignPath request signs only its catalog and setup tool.
set(SUNSHINE_VIRTUAL_GAMEPAD_ROOT_CLEANUP_SCRIPT
    "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/drivers/vhf-gamepad/cleanup.ps1")
if(NOT EXISTS "${SUNSHINE_VIRTUAL_GAMEPAD_ROOT_CLEANUP_SCRIPT}")
    message(FATAL_ERROR
        "Required VHF gamepad root-device cleanup script is missing: ${SUNSHINE_VIRTUAL_GAMEPAD_ROOT_CLEANUP_SCRIPT}")
endif()
# Keep this narrow, ownership-scoped cleanup script in every MSI. A later
# build might intentionally stop bundling the driver package, but it must
# still remove a ROOT\\VIBESHINEVIRTUALGAMEPAD source node created by an
# earlier bundle on final uninstall.
install(FILES "${SUNSHINE_VIRTUAL_GAMEPAD_ROOT_CLEANUP_SCRIPT}"
        DESTINATION "${SUNSHINE_VHF_GAMEPAD_DRIVER_DESTINATION}"
        COMPONENT assets)

set(SUNSHINE_VHF_GAMEPAD_WIX_BUNDLED 0)
set(SUNSHINE_VHF_GAMEPAD_WIX_ALLOW_LOCAL_TEST 0)
set(SUNSHINE_LIBVIRTUALGAMEPAD_PREBUILT_DIR "" CACHE PATH
    "Optional prebuilt libvirtualgamepad package root with driver/, tools/, and manifest.json")
if(SUNSHINE_BUNDLE_VHF_GAMEPAD_DRIVER)
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64)$")
        message(FATAL_ERROR
            "SUNSHINE_BUNDLE_VHF_GAMEPAD_DRIVER currently supports only AMD64 because the pinned libvirtualgamepad package is x64. Disable it for ${CMAKE_SYSTEM_PROCESSOR} builds.")
    endif()
    set(SUNSHINE_VHF_GAMEPAD_WIX_BUNDLED 1)
    if(SUNSHINE_ALLOW_LOCAL_VHF_GAMEPAD_TEST_PACKAGE)
        set(SUNSHINE_VHF_GAMEPAD_WIX_ALLOW_LOCAL_TEST 1)
        if(NOT SUNSHINE_LIBVIRTUALGAMEPAD_PREBUILT_DIR)
            message(FATAL_ERROR
                "SUNSHINE_ALLOW_LOCAL_VHF_GAMEPAD_TEST_PACKAGE requires SUNSHINE_LIBVIRTUALGAMEPAD_PREBUILT_DIR.")
        endif()
    else()
        # Only the archive identity is required. The catalogue is signed by
        # the MSI signing request, so there is no upstream signer to pin.
        foreach(_vhf_gamepad_pin IN ITEMS
                SUNSHINE_VHF_GAMEPAD_RELEASE_TAG
                SUNSHINE_VHF_GAMEPAD_RELEASE_ASSET_SHA256
                SUNSHINE_VHF_GAMEPAD_SOURCE_REVISION
                SUNSHINE_VHF_GAMEPAD_DRIVER_VER
                SUNSHINE_VHF_GAMEPAD_PROTOCOL_VERSION)
            if("${${_vhf_gamepad_pin}}" STREQUAL "")
                message(FATAL_ERROR
                    "SUNSHINE_BUNDLE_VHF_GAMEPAD_DRIVER requires ${_vhf_gamepad_pin} for a production package.")
            endif()
        endforeach()
        unset(_vhf_gamepad_pin)
        sunshine_vhf_is_fixed_length_hex(
            _vhf_gamepad_pin_valid
            "${SUNSHINE_VHF_GAMEPAD_RELEASE_ASSET_SHA256}"
            64)
        if(NOT _vhf_gamepad_pin_valid)
            message(FATAL_ERROR "SUNSHINE_VHF_GAMEPAD_RELEASE_ASSET_SHA256 must be a SHA-256 value.")
        endif()
        sunshine_vhf_is_fixed_length_hex(
            _vhf_gamepad_pin_valid
            "${SUNSHINE_VHF_GAMEPAD_SOURCE_REVISION}"
            40)
        if(NOT _vhf_gamepad_pin_valid)
            message(FATAL_ERROR "SUNSHINE_VHF_GAMEPAD_SOURCE_REVISION must be a full commit SHA.")
        endif()
        unset(_vhf_gamepad_pin_valid)
        # Still validated when supplied, for the hand-signed package case.
        foreach(_vhf_gamepad_signer IN ITEMS
                SUNSHINE_VHF_GAMEPAD_CATALOG_SIGNER_THUMBPRINT
                SUNSHINE_VHF_GAMEPAD_DEVICE_SETUP_SIGNER_THUMBPRINT)
            if(NOT "${${_vhf_gamepad_signer}}" STREQUAL "")
                sunshine_vhf_is_fixed_length_hex(
                    _vhf_gamepad_signer_valid
                    "${${_vhf_gamepad_signer}}"
                    40)
                if(NOT _vhf_gamepad_signer_valid)
                    message(FATAL_ERROR
                        "${_vhf_gamepad_signer} must be a 40-character SHA-1 thumbprint when set.")
                endif()
            endif()
        endforeach()
        unset(_vhf_gamepad_signer_valid)
        unset(_vhf_gamepad_signer)
    endif()

    set(SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_SOURCE_DIR
        "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/drivers/vhf-gamepad")
    set(SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_REFRESH_SCRIPT
        "${CMAKE_SOURCE_DIR}/packaging/windows/virtual_gamepad_driver/refresh_driver_package.ps1")
    set(SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_DOWNLOAD_SCRIPT
        "${CMAKE_SOURCE_DIR}/scripts/download_libvirtualgamepad_release.ps1")
    if(SUNSHINE_LIBVIRTUALGAMEPAD_PREBUILT_DIR)
        set(SUNSHINE_EFFECTIVE_LIBVIRTUALGAMEPAD_PREBUILT_DIR
            "${SUNSHINE_LIBVIRTUALGAMEPAD_PREBUILT_DIR}")
        set(SUNSHINE_DOWNLOAD_LIBVIRTUALGAMEPAD_RELEASE OFF)
    else()
        set(SUNSHINE_EFFECTIVE_LIBVIRTUALGAMEPAD_PREBUILT_DIR
            "${CMAKE_BINARY_DIR}/libvirtualgamepad-release-${SUNSHINE_VHF_GAMEPAD_RELEASE_TAG}")
        set(SUNSHINE_DOWNLOAD_LIBVIRTUALGAMEPAD_RELEASE ON)
    endif()

    # install(FILES) flattens input paths. Keep the signed driver package's
    # driver/ and tools/ layout intact because install.ps1 verifies those exact
    # paths before it stages the INF or creates the source device.
    set(SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_ROOT_FILES
        "${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_SOURCE_DIR}/install.ps1"
        "${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_SOURCE_DIR}/manifest.json"
        "${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_SOURCE_DIR}/release-lock.json")
    set(SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_PACKAGE_FILES
        "${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_SOURCE_DIR}/driver/VibeshineVhfGamepad.inf"
        "${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_SOURCE_DIR}/driver/VibeshineVhfGamepad.dll"
        "${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_SOURCE_DIR}/driver/VibeshineVhfGamepad.cat")
    set(SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_TOOL_FILES
        "${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_SOURCE_DIR}/tools/VibeshineVhfGamepadDeviceSetup.exe")
    set(SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_LOCAL_TEST_FILES "")

    if(NOT EXISTS "${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_SOURCE_DIR}/install.ps1")
        message(FATAL_ERROR
            "Required VHF gamepad installer script is missing: ${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_SOURCE_DIR}/install.ps1")
    endif()
    if(NOT EXISTS "${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_REFRESH_SCRIPT}")
        message(FATAL_ERROR
            "Required VHF gamepad refresh script is missing: ${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_REFRESH_SCRIPT}")
    endif()

    set(SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_LOCAL_TEST_ARGS "")
    if(SUNSHINE_ALLOW_LOCAL_VHF_GAMEPAD_TEST_PACKAGE)
        list(APPEND SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_LOCAL_TEST_ARGS -AllowLocalTestPackage)
        list(APPEND SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_LOCAL_TEST_FILES
            "${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_SOURCE_DIR}/driver/VibeshineVhfGamepad.cer")
    endif()
    set(SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_PIN_ARGS "")
    if(NOT SUNSHINE_ALLOW_LOCAL_VHF_GAMEPAD_TEST_PACKAGE)
        list(APPEND SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_PIN_ARGS
            -ReleaseTag "${SUNSHINE_VHF_GAMEPAD_RELEASE_TAG}"
            -ExpectedReleaseAssetSha256 "${SUNSHINE_VHF_GAMEPAD_RELEASE_ASSET_SHA256}"
            -ExpectedSourceRevision "${SUNSHINE_VHF_GAMEPAD_SOURCE_REVISION}"
            -ExpectedDriverVer "${SUNSHINE_VHF_GAMEPAD_DRIVER_VER}"
            -ExpectedProtocolVersion "${SUNSHINE_VHF_GAMEPAD_PROTOCOL_VERSION}")
        # Passed through only when a pre-signed package is being used.
        if(NOT "${SUNSHINE_VHF_GAMEPAD_CATALOG_SIGNER_THUMBPRINT}" STREQUAL "")
            list(APPEND SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_PIN_ARGS
                -ExpectedCatalogSignerThumbprint "${SUNSHINE_VHF_GAMEPAD_CATALOG_SIGNER_THUMBPRINT}")
        endif()
        if(NOT "${SUNSHINE_VHF_GAMEPAD_DEVICE_SETUP_SIGNER_THUMBPRINT}" STREQUAL "")
            list(APPEND SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_PIN_ARGS
                -ExpectedDeviceSetupSignerThumbprint "${SUNSHINE_VHF_GAMEPAD_DEVICE_SETUP_SIGNER_THUMBPRINT}")
        endif()
    endif()

    if(SUNSHINE_DOWNLOAD_LIBVIRTUALGAMEPAD_RELEASE)
        if(NOT EXISTS "${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_DOWNLOAD_SCRIPT}")
            message(FATAL_ERROR
                "Required libvirtualgamepad release downloader is missing: ${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_DOWNLOAD_SCRIPT}")
        endif()
        add_custom_target(download_sunshine_virtual_gamepad_driver_release
            COMMAND powershell -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass
                    -File "${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_DOWNLOAD_SCRIPT}"
                    -Repository "${SUNSHINE_VHF_GAMEPAD_REPOSITORY}"
                    -Tag "${SUNSHINE_VHF_GAMEPAD_RELEASE_TAG}"
                    -ExpectedArchiveSha256 "${SUNSHINE_VHF_GAMEPAD_RELEASE_ASSET_SHA256}"
                    -ExpectedSourceRevision "${SUNSHINE_VHF_GAMEPAD_SOURCE_REVISION}"
                    -ExpectedDriverVer "${SUNSHINE_VHF_GAMEPAD_DRIVER_VER}"
                    -ExpectedProtocolVersion "${SUNSHINE_VHF_GAMEPAD_PROTOCOL_VERSION}"
                    -OutDir "${SUNSHINE_EFFECTIVE_LIBVIRTUALGAMEPAD_PREBUILT_DIR}"
            DEPENDS "${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_DOWNLOAD_SCRIPT}"
            COMMENT "Downloading pinned VHF gamepad producer release"
            VERBATIM)
    endif()

    add_custom_target(refresh_sunshine_virtual_gamepad_driver_assets
        COMMAND powershell -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass
                -File "${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_REFRESH_SCRIPT}"
                -PrebuiltPackageDir "${SUNSHINE_EFFECTIVE_LIBVIRTUALGAMEPAD_PREBUILT_DIR}"
                -PackageDir "${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_SOURCE_DIR}"
                ${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_LOCAL_TEST_ARGS}
                ${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_PIN_ARGS}
        DEPENDS "${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_REFRESH_SCRIPT}"
                "${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_SOURCE_DIR}/install.ps1"
                "${SUNSHINE_VIRTUAL_GAMEPAD_ROOT_CLEANUP_SCRIPT}"
        COMMENT "Refreshing VHF gamepad package assets from the pinned producer release"
        VERBATIM)

    add_custom_target(validate_sunshine_virtual_gamepad_driver_assets
        COMMAND powershell -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass
                -File "${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_REFRESH_SCRIPT}"
                -ValidateOnly
                -PrebuiltPackageDir "${SUNSHINE_EFFECTIVE_LIBVIRTUALGAMEPAD_PREBUILT_DIR}"
                -PackageDir "${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_SOURCE_DIR}"
                ${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_LOCAL_TEST_ARGS}
                ${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_PIN_ARGS}
        DEPENDS "${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_REFRESH_SCRIPT}"
                "${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_SOURCE_DIR}/install.ps1"
                "${SUNSHINE_VIRTUAL_GAMEPAD_ROOT_CLEANUP_SCRIPT}"
        COMMENT "Validating consumer VHF gamepad package assets"
        VERBATIM)

    if(SUNSHINE_DOWNLOAD_LIBVIRTUALGAMEPAD_RELEASE)
        add_dependencies(refresh_sunshine_virtual_gamepad_driver_assets
            download_sunshine_virtual_gamepad_driver_release)
        add_dependencies(validate_sunshine_virtual_gamepad_driver_assets
            download_sunshine_virtual_gamepad_driver_release)
    endif()

    if(TARGET package_msi AND SUNSHINE_VHF_GAMEPAD_REFRESH_BEFORE_MSI)
        add_dependencies(package_msi refresh_sunshine_virtual_gamepad_driver_assets)
    endif()

    install(FILES ${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_ROOT_FILES}
            DESTINATION "${SUNSHINE_VHF_GAMEPAD_DRIVER_DESTINATION}"
            COMPONENT virtual_gamepad_driver)
    install(FILES ${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_PACKAGE_FILES}
            DESTINATION "${SUNSHINE_VHF_GAMEPAD_DRIVER_DESTINATION}/driver"
            COMPONENT virtual_gamepad_driver)
    install(FILES ${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_TOOL_FILES}
            DESTINATION "${SUNSHINE_VHF_GAMEPAD_DRIVER_DESTINATION}/tools"
            COMPONENT virtual_gamepad_driver)
    if(SUNSHINE_ALLOW_LOCAL_VHF_GAMEPAD_TEST_PACKAGE)
        install(FILES ${SUNSHINE_VIRTUAL_GAMEPAD_DRIVER_LOCAL_TEST_FILES}
                DESTINATION "${SUNSHINE_VHF_GAMEPAD_DRIVER_DESTINATION}/driver"
                COMPONENT virtual_gamepad_driver)
    endif()
endif()

# Mandatory scripts
install(FILES "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/sunshine-setup.ps1"
        DESTINATION "scripts"
        COMPONENT assets)
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/service/"
        DESTINATION "scripts"
        COMPONENT assets)
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/migration/"
        DESTINATION "scripts"
        COMPONENT assets)
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/path/"
        DESTINATION "scripts"
        COMPONENT assets)

# Configurable options for the service
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/autostart/"
        DESTINATION "scripts"
        COMPONENT autostart)

# scripts
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/firewall/"
        DESTINATION "scripts"
        COMPONENT firewall)

# Sunshine assets
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/assets/"
        DESTINATION "${SUNSHINE_ASSETS_DIR}"
        COMPONENT assets)

# Plugins (copy plugin folders such as `plugins/playnite` into the package)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/plugins/"
        DESTINATION "plugins"
        COMPONENT assets)

# copy assets (excluding shaders) to build directory, for running without install
file(COPY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/assets/"
        DESTINATION "${CMAKE_BINARY_DIR}/assets"
        PATTERN "shaders" EXCLUDE)

if(WEBRTC_RUNTIME_DLL)
    file(COPY "${WEBRTC_RUNTIME_DLL}"
            DESTINATION "${CMAKE_BINARY_DIR}")
endif()
# use junction for shaders directory
cmake_path(CONVERT "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/assets/shaders"
        TO_NATIVE_PATH_LIST shaders_in_build_src_native)
cmake_path(CONVERT "${CMAKE_BINARY_DIR}/assets/shaders" TO_NATIVE_PATH_LIST shaders_in_build_dest_native)
if(NOT EXISTS "${CMAKE_BINARY_DIR}/assets/shaders")
    execute_process(COMMAND cmd.exe /c mklink /J "${shaders_in_build_dest_native}" "${shaders_in_build_src_native}")
endif()

set(CPACK_PACKAGE_ICON "${CMAKE_SOURCE_DIR}\\\\apollo.ico")

# The name of the directory that will be created in C:/Program Files/
# Match the legacy NSIS layout by installing under Apollo
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Apollo")

# Setting components groups and dependencies
set(CPACK_COMPONENT_GROUP_CORE_EXPANDED true)
set(CPACK_COMPONENT_GROUP_THIRDPARTY_DISPLAY_NAME "Third Party")
set(CPACK_COMPONENT_GROUP_THIRDPARTY_DESCRIPTION "Bundled third-party installers and optional components.")

# sunshine binary
set(CPACK_COMPONENT_APPLICATION_DISPLAY_NAME "${CMAKE_PROJECT_NAME}")
set(CPACK_COMPONENT_APPLICATION_DESCRIPTION "${CMAKE_PROJECT_NAME} main application and required components.")
set(CPACK_COMPONENT_APPLICATION_GROUP "Core")
set(CPACK_COMPONENT_APPLICATION_REQUIRED true)
set(CPACK_COMPONENT_APPLICATION_DEPENDS assets)

# service auto-start script
set(CPACK_COMPONENT_AUTOSTART_DISPLAY_NAME "Launch on Startup")
set(CPACK_COMPONENT_AUTOSTART_DESCRIPTION "If enabled, launches Vibepollo automatically on system startup.")
set(CPACK_COMPONENT_AUTOSTART_GROUP "Core")

# assets
set(CPACK_COMPONENT_ASSETS_DISPLAY_NAME "Required Assets")
set(CPACK_COMPONENT_ASSETS_DESCRIPTION "Shaders, default box art, and configuration-server assets.")
set(CPACK_COMPONENT_ASSETS_GROUP "Core")
set(CPACK_COMPONENT_ASSETS_REQUIRED true)

# drivers
set(CPACK_COMPONENT_SUDOVDA_DISPLAY_NAME "SudoVDA")
set(CPACK_COMPONENT_SUDOVDA_DESCRIPTION "Bundled rollback virtual display driver.")
set(CPACK_COMPONENT_SUDOVDA_GROUP "Drivers")
set(CPACK_COMPONENT_SUDOVDA_REQUIRED true)

set(CPACK_COMPONENT_VIRTUAL_DISPLAY_DRIVER_DISPLAY_NAME "Vibepollo Display Driver")
set(CPACK_COMPONENT_VIRTUAL_DISPLAY_DRIVER_DESCRIPTION "Default virtual display driver.")
set(CPACK_COMPONENT_VIRTUAL_DISPLAY_DRIVER_GROUP "Drivers")
set(CPACK_COMPONENT_VIRTUAL_DISPLAY_DRIVER_REQUIRED true)

if(SUNSHINE_BUNDLE_VHF_GAMEPAD_DRIVER)
    set(CPACK_COMPONENT_VIRTUAL_GAMEPAD_DRIVER_DISPLAY_NAME "Vibepollo Virtual Gamepad Driver")
    set(CPACK_COMPONENT_VIRTUAL_GAMEPAD_DRIVER_DESCRIPTION
        "Pinned VHF UMDF gamepad source-driver package.")
    set(CPACK_COMPONENT_VIRTUAL_GAMEPAD_DRIVER_GROUP "Drivers")
    set(CPACK_COMPONENT_VIRTUAL_GAMEPAD_DRIVER_REQUIRED true)
endif()

# audio tool
set(CPACK_COMPONENT_AUDIO_DISPLAY_NAME "audio-info")
set(CPACK_COMPONENT_AUDIO_DESCRIPTION "CLI tool providing information about sound devices.")
set(CPACK_COMPONENT_AUDIO_GROUP "Tools")

# display tool
set(CPACK_COMPONENT_DXGI_DISPLAY_NAME "dxgi-info")
set(CPACK_COMPONENT_DXGI_DESCRIPTION "CLI tool providing information about graphics cards and displays.")
set(CPACK_COMPONENT_DXGI_GROUP "Tools")

# firewall scripts
set(CPACK_COMPONENT_FIREWALL_DISPLAY_NAME "Add Firewall Exclusions")
set(CPACK_COMPONENT_FIREWALL_DESCRIPTION "Scripts to enable or disable firewall rules.")
set(CPACK_COMPONENT_FIREWALL_GROUP "Scripts")

# gamepad scripts are bundled under assets and not exposed as a separate component

# include specific packaging (WiX only)
include(${CMAKE_MODULE_PATH}/packaging/windows_wix.cmake)
