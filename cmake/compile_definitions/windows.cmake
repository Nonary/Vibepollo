# windows specific compile definitions

add_compile_definitions(SUNSHINE_PLATFORM="windows")

enable_language(RC)
set(CMAKE_RC_COMPILER windres)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")

# gcc complains about misleading indentation in some mingw includes
list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-misleading-indentation)

# see gcc bug 98723
add_definitions(-DUSE_BOOST_REGEX)

# curl
add_definitions(-DCURL_STATICLIB)
include_directories(SYSTEM ${CURL_STATIC_INCLUDE_DIRS})
link_directories(${CURL_STATIC_LIBRARY_DIRS})

# miniupnpc
add_definitions(-DMINIUPNP_STATICLIB)

# extra tools/binaries for audio/display devices
add_subdirectory(tools)  # todo - this is temporary, only tools for Windows are needed, for now

# nvidia
include_directories(SYSTEM "${CMAKE_SOURCE_DIR}/third-party/nvapi-open-source-sdk")
file(GLOB NVPREFS_FILES CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/third-party/nvapi-open-source-sdk/*.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/nvprefs/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/nvprefs/*.h")

# vigem
include_directories(SYSTEM "${CMAKE_SOURCE_DIR}/third-party/ViGEmClient/include")
include_directories(SYSTEM "${CMAKE_SOURCE_DIR}/third-party/sudovda")

# apollo icon
if(NOT DEFINED PROJECT_ICON_PATH)
    set(PROJECT_ICON_PATH "${CMAKE_SOURCE_DIR}/apollo.ico")
endif()

list(APPEND SUNSHINE_DEFINITIONS PROJECT_APP_USER_MODEL_ID="${WINDOWS_APP_USER_MODEL_ID}")

# Generate Windows fixed FILEVERSION metadata at build time.  The generator is
# intentionally run for every build so dirty/local builds and post-tag rebuilds
# can advance the fourth version field without requiring a fresh CMake configure.
set(SUNSHINE_WINDOWS_VERSIONINFO_DIR "${CMAKE_BINARY_DIR}/generated_versioninfo")
set(SUNSHINE_WINDOWS_VERSIONINFO_HEADER "${SUNSHINE_WINDOWS_VERSIONINFO_DIR}/windows_versioninfo_generated.h")
set(SUNSHINE_WINDOWS_VERSIONINFO_CACHE "${SUNSHINE_WINDOWS_VERSIONINFO_DIR}/windows_versioninfo_generated.cache")
set(SUNSHINE_WINDOWS_VERSIONINFO_STAMP "${SUNSHINE_WINDOWS_VERSIONINFO_DIR}/windows_versioninfo_generated.stamp")

add_custom_target(generate_windows_versioninfo
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SUNSHINE_WINDOWS_VERSIONINFO_DIR}"
    COMMAND ${CMAKE_COMMAND}
            "-DOUTPUT_FILE=${SUNSHINE_WINDOWS_VERSIONINFO_HEADER}"
            "-DCACHE_FILE=${SUNSHINE_WINDOWS_VERSIONINFO_CACHE}"
            "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DPROJECT_VERSION_FULL=${PROJECT_VERSION_FULL}"
            "-DPROJECT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}"
            "-DPROJECT_VERSION_MINOR=${PROJECT_VERSION_MINOR}"
            "-DPROJECT_VERSION_PATCH=${PROJECT_VERSION_PATCH}"
            "-DPROJECT_VERSION_PRERELEASE=${PROJECT_VERSION_PRERELEASE}"
            -P "${CMAKE_SOURCE_DIR}/cmake/prep/emit_windows_versioninfo.cmake"
    COMMAND ${CMAKE_COMMAND} -E touch "${SUNSHINE_WINDOWS_VERSIONINFO_STAMP}"
    BYPRODUCTS
            "${SUNSHINE_WINDOWS_VERSIONINFO_HEADER}"
            "${SUNSHINE_WINDOWS_VERSIONINFO_CACHE}"
            "${SUNSHINE_WINDOWS_VERSIONINFO_STAMP}"
    COMMENT "Generating Windows VERSIONINFO header"
    VERBATIM
)

set_source_files_properties("${SUNSHINE_WINDOWS_VERSIONINFO_HEADER}" PROPERTIES GENERATED TRUE)
set_source_files_properties("${CMAKE_SOURCE_DIR}/src/platform/windows/windows.rc" PROPERTIES
    OBJECT_DEPENDS "${SUNSHINE_WINDOWS_VERSIONINFO_HEADER};${SUNSHINE_WINDOWS_VERSIONINFO_STAMP}")

# Create a separate object library for the RC file with minimal includes
add_library(sunshine_rc_object OBJECT "${CMAKE_SOURCE_DIR}/src/platform/windows/windows.rc")
add_dependencies(sunshine_rc_object generate_windows_versioninfo)

# Set minimal properties for RC compilation - only what's needed for the resource file
# Otherwise compilation can fail due to "line too long" errors
set_target_properties(sunshine_rc_object PROPERTIES
    COMPILE_DEFINITIONS "PROJECT_ICON_PATH=${PROJECT_ICON_PATH};PROJECT_NAME=${PROJECT_NAME};PROJECT_VENDOR=${SUNSHINE_PUBLISHER_NAME};PROJECT_VERSION=${PROJECT_VERSION_FULL};PROJECT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR};PROJECT_VERSION_MINOR=${PROJECT_VERSION_MINOR};PROJECT_VERSION_PATCH=${PROJECT_VERSION_PATCH}"  # cmake-lint: disable=C0301
    INCLUDE_DIRECTORIES "${SUNSHINE_WINDOWS_VERSIONINFO_DIR}"
)

function(sunshine_add_windows_versioninfo target_name)
    if(NOT TARGET "${target_name}")
        message(FATAL_ERROR "sunshine_add_windows_versioninfo: target not found: ${target_name}")
    endif()

    string(MAKE_C_IDENTIFIER "${target_name}" _target_id)
    set(_rc_target "sunshine_${_target_id}_rc_object")
    set(_rc_file "${SUNSHINE_WINDOWS_VERSIONINFO_DIR}/${_target_id}_versioninfo.rc")

    if(NOT TARGET "${_rc_target}")
        set(_versioninfo_file_description "${target_name}")
        set(_versioninfo_internal_name "${target_name}")
        set(_versioninfo_original_filename "${target_name}.exe")
        set(_versioninfo_product_name "${target_name}")
        configure_file(
            "${CMAKE_SOURCE_DIR}/src/platform/windows/tool_version.rc.in"
            "${_rc_file}"
            @ONLY
        )
        set_source_files_properties("${_rc_file}" PROPERTIES
            GENERATED TRUE
            OBJECT_DEPENDS "${SUNSHINE_WINDOWS_VERSIONINFO_HEADER};${SUNSHINE_WINDOWS_VERSIONINFO_STAMP}"
        )
        add_library("${_rc_target}" OBJECT "${_rc_file}")
        add_dependencies("${_rc_target}" generate_windows_versioninfo)
        set_target_properties("${_rc_target}" PROPERTIES
            INCLUDE_DIRECTORIES "${SUNSHINE_WINDOWS_VERSIONINFO_DIR}"
        )
    endif()

    target_sources("${target_name}" PRIVATE "$<TARGET_OBJECTS:${_rc_target}>")
endfunction()

foreach(_sunshine_versioned_tool IN ITEMS
        dxgi-info
        audio-info
        sunshinesvc
        playnite-launcher
        sunshine_wgc_capture
        sunshine_display_helper)
    if(TARGET "${_sunshine_versioned_tool}")
        sunshine_add_windows_versioninfo("${_sunshine_versioned_tool}")
    endif()
endforeach()
unset(_sunshine_versioned_tool)

# ViGEmBus version
set(VIGEMBUS_PACKAGED_V "1.21.442")
set(VIGEMBUS_PACKAGED_V_2 "${VIGEMBUS_PACKAGED_V}.0")
list(APPEND SUNSHINE_DEFINITIONS VIGEMBUS_PACKAGED_VERSION="${VIGEMBUS_PACKAGED_V_2}")

set(PLATFORM_TARGET_FILES
        "${CMAKE_SOURCE_DIR}/src/platform/windows/publish.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/misc.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/misc.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/host_stats.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/pipes.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/pipes.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/misc_utils.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/misc_utils.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/process_handler.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/process_handler.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/display_settings_client.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/display_settings_client.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_coordinator.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_coordinator.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_request_helpers.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_request_helpers.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_integration.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_integration.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_session_deferral.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_session_deferral.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_watchdog.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_watchdog.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/virtual_display_cleanup.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/virtual_display_cleanup.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/hotkey_manager.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/hotkey_manager.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/playnite_ipc.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/playnite_ipc.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/playnite_protocol.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/playnite_protocol.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/playnite_sync.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/playnite_sync.cpp"
        "${CMAKE_SOURCE_DIR}/src/config_playnite.h"
        "${CMAKE_SOURCE_DIR}/src/config_playnite.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/playnite_integration.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/playnite_integration.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/input.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_base.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_ram.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_vram.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_wgc.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/audio.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/virtual_display.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/virtual_display.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/virtual_display_legacy.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/virtual_display_legacy.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/utils.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/utils.cpp"
        "${CMAKE_SOURCE_DIR}/third-party/sudovda/sudovda-ioctl.h"
        "${CMAKE_SOURCE_DIR}/third-party/sudovda/sudovda.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/frame_limiter.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/frame_limiter.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/frame_limiter_nvcp.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/frame_limiter_nvcp.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/lossless_scaling_paths.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/image_convert.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/image_convert.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/rtss_integration.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/rtss_integration.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/ipc_session.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/ipc_session.cpp"
        "${CMAKE_SOURCE_DIR}/tools/playnite_launcher/focus_utils.cpp"
        "${CMAKE_SOURCE_DIR}/tools/playnite_launcher/lossless_scaling.cpp"
        "${CMAKE_SOURCE_DIR}/third-party/ViGEmClient/src/ViGEmClient.cpp"
        "${CMAKE_SOURCE_DIR}/third-party/ViGEmClient/include/ViGEm/Client.h"
        "${CMAKE_SOURCE_DIR}/third-party/ViGEmClient/include/ViGEm/Common.h"
        "${CMAKE_SOURCE_DIR}/third-party/ViGEmClient/include/ViGEm/Util.h"
        "${CMAKE_SOURCE_DIR}/third-party/ViGEmClient/include/ViGEm/km/BusShared.h"
        ${NVPREFS_FILES})

set(OPENSSL_LIBRARIES
        libssl.a
        libcrypto.a)

list(PREPEND PLATFORM_LIBRARIES
        ${CURL_STATIC_LIBRARIES}
        avrt
        d3d11
        D3DCompiler
        dwmapi
        dxgi
        iphlpapi
        ksuser
        libssp.a
        libstdc++.a
        libwinpthread.a
        minhook::minhook
        ntdll
        pdh
        setupapi
        shlwapi
        shell32
        crypt32
        synchronization.lib
        Windowscodecs
        userenv
        ws2_32
        wsock32
)

if(SUNSHINE_ENABLE_TRAY)
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/third-party/tray/src/tray_windows.c")
endif()
