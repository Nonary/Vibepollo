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

# Drivers (Sunshine virtual display)
set(SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/drivers/sunshine")
set(SUNSHINE_VIRTUAL_DISPLAY_DRIVER_REFRESH_SCRIPT "${CMAKE_SOURCE_DIR}/packaging/windows/virtual_display_driver/refresh_driver_package.ps1")
set(SUNSHINE_VIRTUAL_DISPLAY_DRIVER_FILES
    "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}/install.ps1"
    "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}/SunshineVirtualDisplayDriver.inf"
    "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}/SunshineVirtualDisplayDriver.dll"
    "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}/SunshineVirtualDisplayDriver.cat"
    "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}/nefconc.exe"
    "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}/virtualdisplay_probe.exe"
)
foreach(_sunshine_driver_optional_file IN ITEMS
        "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}/SunshineVirtualDisplayDriver.cer")
    if(EXISTS "${_sunshine_driver_optional_file}")
        list(APPEND SUNSHINE_VIRTUAL_DISPLAY_DRIVER_FILES "${_sunshine_driver_optional_file}")
    endif()
endforeach()
unset(_sunshine_driver_optional_file)

foreach(_sunshine_driver_file IN LISTS SUNSHINE_VIRTUAL_DISPLAY_DRIVER_FILES)
    if (NOT EXISTS "${_sunshine_driver_file}")
        message(FATAL_ERROR "Required Sunshine virtual display driver artifact missing: ${_sunshine_driver_file}")
    endif()
    file(SIZE "${_sunshine_driver_file}" _sunshine_driver_file_size)
    if (_sunshine_driver_file_size EQUAL 0)
        message(FATAL_ERROR "Required Sunshine virtual display driver artifact is empty (0 bytes): ${_sunshine_driver_file}")
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
                -PackageDir "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}"
        DEPENDS "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_REFRESH_SCRIPT}"
                ${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_FILES}
        COMMENT "Validating Sunshine virtual display driver package assets"
        VERBATIM)

    add_custom_target(refresh_sunshine_virtual_display_driver_assets
        COMMAND powershell -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass
                -File "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_REFRESH_SCRIPT}"
                -Build
                -LibVirtualDisplayDir "${SUNSHINE_LIBVIRTUALDISPLAY_SOURCE_DIR}"
                -PackageDir "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}"
        DEPENDS "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_REFRESH_SCRIPT}"
        COMMENT "Building and refreshing Sunshine virtual display driver package assets"
        VERBATIM)

    if(TARGET package_msi)
        add_dependencies(package_msi refresh_sunshine_virtual_display_driver_assets)
    endif()
endif()

install(FILES ${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_FILES}
        DESTINATION "drivers/sunshine"
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
set(CPACK_COMPONENT_VIRTUAL_DISPLAY_DRIVER_DISPLAY_NAME "Sunshine Virtual Display Driver")
set(CPACK_COMPONENT_VIRTUAL_DISPLAY_DRIVER_DESCRIPTION "Driver required for Virtual Display to function.")
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
