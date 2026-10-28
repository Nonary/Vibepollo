# ZIP packages use the same installed runtime as the MSI, with an as-caller
# launcher and a clean profile for physical-display capture.
set(SUNSHINE_PORTABLE_EXECUTABLES sunshine.exe tools/dxgi-info.exe tools/audio-info.exe)
foreach(_portable_target IN ITEMS playnite-launcher sunshine_wgc_capture sunshine_display_helper vibepollo_dualsense_haptics)
    if(TARGET "${_portable_target}")
        list(APPEND SUNSHINE_PORTABLE_EXECUTABLES "tools/${_portable_target}.exe")
    endif()
endforeach()

set(SUNSHINE_PORTABLE_LIBRARIES tools/vibepollo_dualsense_audio.dll)
set(_portable_runtime_files "${ZLIB}" ${SUNSHINE_TRUEHDR_RUNTIME_FILES})
if(WEBRTC_RUNTIME_DLL)
    list(APPEND _portable_runtime_files "${WEBRTC_RUNTIME_DLL}")
endif()
if(DEFINED _MINHOOK_DLL)
    list(APPEND _portable_runtime_files "${_MINHOOK_DLL}")
endif()
foreach(_portable_runtime_file IN LISTS _portable_runtime_files)
    get_filename_component(_portable_runtime_name "${_portable_runtime_file}" NAME)
    list(APPEND SUNSHINE_PORTABLE_LIBRARIES "${_portable_runtime_name}")
endforeach()

# The application uses MinGW, but downloaded helpers such as the /MD TrueHDR
# shim also need release VC runtimes. Search compiler redistribution payloads,
# never copy a developer's installed System32 CRT into a release archive.
set(SUNSHINE_PORTABLE_REDISTRIBUTABLE_DIRS "" CACHE STRING
    "Directories containing redistributable release compiler DLLs for portable packaging")
set(SUNSHINE_PORTABLE_REDISTRIBUTABLE_DIRS_EFFECTIVE ${SUNSHINE_PORTABLE_REDISTRIBUTABLE_DIRS})
foreach(_portable_runtime_file IN LISTS _portable_runtime_files)
    get_filename_component(_portable_runtime_dir "${_portable_runtime_file}" DIRECTORY)
    list(APPEND SUNSHINE_PORTABLE_REDISTRIBUTABLE_DIRS_EFFECTIVE "${_portable_runtime_dir}")
endforeach()
if(MINGW)
    get_filename_component(_portable_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    list(APPEND SUNSHINE_PORTABLE_REDISTRIBUTABLE_DIRS_EFFECTIVE "${_portable_compiler_dir}")
endif()
if(CMAKE_HOST_WIN32)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64)$")
        set(_portable_crt_arch x64)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|aarch64)$")
        set(_portable_crt_arch arm64)
    else()
        set(_portable_crt_arch x86)
    endif()
    file(TO_CMAKE_PATH "$ENV{VCToolsRedistDir}" _portable_env_redist)
    set(_portable_redist_roots "${_portable_env_redist}")
    foreach(_portable_vs_version IN ITEMS 17 16 15)
        cmake_host_system_information(RESULT _portable_vs_dir QUERY VS_${_portable_vs_version}_DIR)
        if(IS_DIRECTORY "${_portable_vs_dir}")
            file(GLOB _portable_vs_redist "${_portable_vs_dir}/VC/Redist/MSVC/*")
            list(SORT _portable_vs_redist COMPARE NATURAL ORDER DESCENDING)
            list(APPEND _portable_redist_roots ${_portable_vs_redist})
        endif()
    endforeach()
    foreach(_portable_redist_root IN LISTS _portable_redist_roots)
        if(NOT _portable_redist_root STREQUAL "")
            file(GLOB _portable_crt_dirs "${_portable_redist_root}/${_portable_crt_arch}/Microsoft.VC*.CRT")
            list(APPEND SUNSHINE_PORTABLE_REDISTRIBUTABLE_DIRS_EFFECTIVE ${_portable_crt_dirs})
        endif()
    endforeach()
endif()
list(REMOVE_DUPLICATES SUNSHINE_PORTABLE_REDISTRIBUTABLE_DIRS_EFFECTIVE)

get_filename_component(_portable_plugin_filename "${SUNSHINE_PLAYNITE_PLUGIN_DLL}" NAME)
if(_portable_plugin_filename STREQUAL "")
    message(FATAL_ERROR "Portable packaging requires the built Playnite plugin path.")
endif()

set(SUNSHINE_PORTABLE_REQUIRED_FILES
    ${SUNSHINE_PORTABLE_EXECUTABLES}
    ${SUNSHINE_PORTABLE_LIBRARIES}
    assets/apps.json
    assets/desktop.png
    assets/steam.png
    assets/web/index.html
    assets/web/v2/index.html
    assets/shaders/directx/cursor_vs.hlsl
    plugins/playnite/SunshinePlaynite/extension.yaml
    plugins/playnite/SunshinePlaynite/${_portable_plugin_filename}
    plugins/playnite/SunshinePlaynite/icon.png)

set(_portable_templates "${CMAKE_CURRENT_LIST_DIR}/../../packaging/windows/portable")
set(SUNSHINE_INSTALLER_CPACK_PROJECT_CONFIG "${CPACK_PROJECT_CONFIG_FILE}")
configure_file("${_portable_templates}/cpack_project_config.cmake.in"
    "${CMAKE_BINARY_DIR}/portable_cpack_project_config.cmake" @ONLY)
configure_file("${_portable_templates}/pre_build.cmake.in"
    "${CMAKE_BINARY_DIR}/portable_pre_build.cmake" @ONLY)
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_BINARY_DIR}/portable_cpack_project_config.cmake")

# The monolithic install also stages the optional driver payload. Prepare it
# exactly as the MSI does, without installing any driver on the build host.
if(TARGET package_portable)
    if(TARGET refresh_sunshine_virtual_display_driver_assets AND SUNSHINE_VDD_REFRESH_BEFORE_MSI)
        add_dependencies(package_portable refresh_sunshine_virtual_display_driver_assets)
    endif()
    if(TARGET refresh_sunshine_virtual_gamepad_driver_assets AND SUNSHINE_VHF_GAMEPAD_REFRESH_BEFORE_MSI)
        add_dependencies(package_portable refresh_sunshine_virtual_gamepad_driver_assets)
    endif()
endif()
