# windows specific packaging
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

# Optional NVIDIA TrueHDR runtime. Release builders download a pinned runtime
# bundle before configure; local builders may place these files in this
# directory before packaging. Only the TrueHDR feature DLL is bundled; VSR is not
# used.
option(SUNSHINE_REQUIRE_TRUEHDR_RUNTIME "Fail Windows packaging when the TrueHDR runtime DLLs are missing." OFF)
set(SUNSHINE_TRUEHDR_RUNTIME_DIR "${CMAKE_BINARY_DIR}" CACHE PATH "Directory containing vibeshine_truehdr.dll and the NVIDIA NGX TrueHDR runtime DLL")
set(SUNSHINE_TRUEHDR_RUNTIME_FILES
        "${SUNSHINE_TRUEHDR_RUNTIME_DIR}/vibeshine_truehdr.dll"
        "${SUNSHINE_TRUEHDR_RUNTIME_DIR}/nvngx_truehdr.dll")
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

# CTM bridge agent (ctm-usbip.exe) — OPTIONAL out-of-tree prebuilt.
# We do NOT vendor any CTM source (upstream has no license); the agent is treated
# as an opaque artifact. Point SUNSHINE_CTM_AGENT_DIR at an unpacked CTM-USBIP
# release folder (containing ctm-usbip.exe plus its DLLs/maps/profiles) to stage it
# into "<install>/tools/ctm/", which is the default path the supervisor looks up.
# When unset/missing, packaging proceeds without it and the user can instead point
# the `ctm_path` config key at an external install.
set(SUNSHINE_CTM_AGENT_DIR "" CACHE PATH "Optional unpacked CTM-USBIP release dir to bundle under tools/ctm")
if(SUNSHINE_CTM_AGENT_DIR AND EXISTS "${SUNSHINE_CTM_AGENT_DIR}/ctm-usbip.exe")
    install(DIRECTORY "${SUNSHINE_CTM_AGENT_DIR}/"
        DESTINATION "tools/ctm"
        COMPONENT application
        USE_SOURCE_PERMISSIONS)
endif()

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
        DESTINATION "drivers/sudovda"
        COMPONENT sudovda)

# Drivers (Vibepollo Display Driver)
set(SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/drivers/sunshine")
set(SUNSHINE_VIRTUAL_DISPLAY_DRIVER_REFRESH_SCRIPT "${CMAKE_SOURCE_DIR}/packaging/windows/virtual_display_driver/refresh_driver_package.ps1")
set(SUNSHINE_LIBVIRTUALDISPLAY_PREBUILT_DIR "" CACHE PATH "GitHub Actions only: path to a prebuilt libvirtualdisplay package root with driver/ and tools/")
set(SUNSHINE_EFFECTIVE_LIBVIRTUALDISPLAY_PREBUILT_DIR "${SUNSHINE_LIBVIRTUALDISPLAY_PREBUILT_DIR}")
if(SUNSHINE_LIBVIRTUALDISPLAY_PREBUILT_DIR AND NOT "$ENV{GITHUB_ACTIONS}" STREQUAL "true")
    message(WARNING "Ignoring SUNSHINE_LIBVIRTUALDISPLAY_PREBUILT_DIR outside GitHub Actions; local installer builds refresh the driver from SUNSHINE_LIBVIRTUALDISPLAY_SOURCE_DIR.")
    set(SUNSHINE_EFFECTIVE_LIBVIRTUALDISPLAY_PREBUILT_DIR "")
endif()
set(SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SIGNING_ARGS "")
set(SUNSHINE_VIRTUAL_DISPLAY_DRIVER_FILES
    "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}/install.ps1"
    "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}/SunshineVirtualDisplayDriver.inf"
    "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}/SunshineVirtualDisplayDriver.dll"
    "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}/SunshineVirtualDisplayDriver.cat"
    "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}/nefconc.exe"
    "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}/virtualdisplay_probe.exe"
)
set(SUNSHINE_VIRTUAL_DISPLAY_VULKAN_LAYER_FILES
    "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}/vulkan-layer/VkLayer_sunshine_hdr.dll"
    "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}/vulkan-layer/VkLayer_sunshine_hdr.json"
)
set(SUNSHINE_VIRTUAL_DISPLAY_PACKAGE_FILES
    ${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_FILES}
    ${SUNSHINE_VIRTUAL_DISPLAY_VULKAN_LAYER_FILES}
)
foreach(_sunshine_driver_optional_file IN ITEMS
        "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}/SunshineVirtualDisplayDriver.cer")
    if(EXISTS "${_sunshine_driver_optional_file}")
        list(APPEND SUNSHINE_VIRTUAL_DISPLAY_DRIVER_FILES "${_sunshine_driver_optional_file}")
        list(APPEND SUNSHINE_VIRTUAL_DISPLAY_PACKAGE_FILES "${_sunshine_driver_optional_file}")
    endif()
endforeach()
unset(_sunshine_driver_optional_file)

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

    add_custom_target(refresh_sunshine_virtual_display_driver_assets
        COMMAND powershell -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass
                -File "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_REFRESH_SCRIPT}"
                -Build
                -LibVirtualDisplayDir "${SUNSHINE_LIBVIRTUALDISPLAY_SOURCE_DIR}"
                -PrebuiltPackageDir "${SUNSHINE_EFFECTIVE_LIBVIRTUALDISPLAY_PREBUILT_DIR}"
                -PackageDir "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}"
                ${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SIGNING_ARGS}
        DEPENDS "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_REFRESH_SCRIPT}"
        COMMENT "Building and refreshing Vibepollo Display Driver package assets"
        VERBATIM)

    if(TARGET package_msi)
        add_dependencies(package_msi refresh_sunshine_virtual_display_driver_assets)
    endif()
endif()

install(FILES ${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_FILES}
        DESTINATION "drivers/sunshine"
        COMPONENT virtual_display_driver)
install(FILES ${SUNSHINE_VIRTUAL_DISPLAY_VULKAN_LAYER_FILES}
        DESTINATION "drivers/sunshine/vulkan-layer"
        COMPONENT virtual_display_driver)

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
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/gamepad/"
        DESTINATION "scripts"
        COMPONENT assets)

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
set(CPACK_COMPONENT_ASSETS_DESCRIPTION "Shaders, default box art, and web UI.")
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
