# windows specific target definitions
add_executable(vibepollo_dualsense_haptics "${CMAKE_SOURCE_DIR}/tools/dualsense_haptics/launcher.cpp")
add_library(vibepollo_dualsense_audio SHARED "${CMAKE_SOURCE_DIR}/tools/dualsense_haptics/audio_hook.cpp")
foreach(_haptics_target IN ITEMS vibepollo_dualsense_haptics vibepollo_dualsense_audio)
    target_compile_features(${_haptics_target} PRIVATE cxx_std_20)
    target_compile_definitions(${_haptics_target} PRIVATE UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00 NOMINMAX)
    set_target_properties(${_haptics_target} PROPERTIES PREFIX "" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")
    target_link_options(${_haptics_target} PRIVATE -static)
endforeach()
target_link_options(vibepollo_dualsense_haptics PRIVATE -municode)
target_link_libraries(vibepollo_dualsense_haptics PRIVATE setupapi)
target_link_libraries(vibepollo_dualsense_audio PRIVATE minhook::minhook ole32 propsys uuid shell32)
add_dependencies(sunshine vibepollo_dualsense_haptics vibepollo_dualsense_audio)
if(DEFINED _MINHOOK_DLL)
    add_custom_command(TARGET vibepollo_dualsense_audio POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_MINHOOK_DLL}" "$<TARGET_FILE_DIR:vibepollo_dualsense_audio>")
endif()
if(BUILD_TESTS AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|amd64|x86_64")
    add_executable(test_windows_dualsense_launcher "${CMAKE_SOURCE_DIR}/tools/dualsense_haptics/launcher_probe.cpp")
    target_compile_features(test_windows_dualsense_launcher PRIVATE cxx_std_20)
    target_compile_definitions(test_windows_dualsense_launcher PRIVATE UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00 NOMINMAX)
    target_link_options(test_windows_dualsense_launcher PRIVATE -municode -static)
    target_link_libraries(test_windows_dualsense_launcher PRIVATE ole32 uuid)
    set_target_properties(test_windows_dualsense_launcher PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")
    add_dependencies(test_windows_dualsense_launcher vibepollo_dualsense_audio)
    add_test(NAME windows_dualsense_launcher COMMAND test_windows_dualsense_launcher)
    set_tests_properties(windows_dualsense_launcher PROPERTIES TIMEOUT 60)
    add_executable(test_windows_dualsense_audio "${CMAKE_SOURCE_DIR}/tools/dualsense_haptics/probe.cpp")
    target_compile_features(test_windows_dualsense_audio PRIVATE cxx_std_20)
    target_compile_definitions(test_windows_dualsense_audio PRIVATE UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00 NOMINMAX)
    target_link_options(test_windows_dualsense_audio PRIVATE -municode -static)
    target_link_libraries(test_windows_dualsense_audio PRIVATE ole32 uuid)
    set_target_properties(test_windows_dualsense_audio PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")
    add_dependencies(test_windows_dualsense_audio vibepollo_dualsense_audio)
    add_test(NAME windows_dualsense_audio COMMAND test_windows_dualsense_audio)
    set_tests_properties(windows_dualsense_audio PROPERTIES TIMEOUT 60)
endif()
set_target_properties(sunshine PROPERTIES LINK_SEARCH_START_STATIC 1)
set(CMAKE_FIND_LIBRARY_SUFFIXES ".dll")

# Look for zlib1.dll in Sunshine install directory or Apollo
find_library(ZLIB ZLIB1
    HINTS
        "C:/Program Files (x86)/Sunshine"
        "C:/Program Files/Apollo"
)
list(APPEND SUNSHINE_EXTERNAL_LIBRARIES
        $<TARGET_OBJECTS:sunshine_rc_object>
        Windowsapp.lib
        Wtsapi32.lib
        avrt.lib
        Mscms.lib
        version.lib)

# Build the Playnite 10 .NET plugin into the runtime/package layout. Do not copy
# the SDK or project sources into the installer; Playnite supplies Playnite.SDK.
if(DEFINED SUNSHINE_DOTNET_EXECUTABLE AND SUNSHINE_DOTNET_EXECUTABLE STREQUAL "")
    # Recover configurations created with the former empty cache default.
    unset(SUNSHINE_DOTNET_EXECUTABLE CACHE)
    unset(SUNSHINE_DOTNET_EXECUTABLE)
endif()
find_program(SUNSHINE_DOTNET_EXECUTABLE
        NAMES dotnet dotnet.exe
        HINTS "$ENV{DOTNET_ROOT}" "$ENV{ProgramFiles}/dotnet"
        DOC "Path to dotnet used to build the Playnite plugin")
if(NOT SUNSHINE_DOTNET_EXECUTABLE)
    message(FATAL_ERROR
            "dotnet was not found. Install the .NET SDK or set SUNSHINE_DOTNET_EXECUTABLE.")
endif()
set(SUNSHINE_PLAYNITE_PLUGIN_SOURCE_DIR
        "${CMAKE_SOURCE_DIR}/plugins/playnite/SunshinePlaynite")
set(SUNSHINE_PLAYNITE_PLUGIN_OUTPUT_DIR
        "${CMAKE_BINARY_DIR}/plugins/playnite/SunshinePlaynite")
set(SUNSHINE_PLAYNITE_PLUGIN_DLL
        "${SUNSHINE_PLAYNITE_PLUGIN_OUTPUT_DIR}/VibepolloPlaynite.dll")
set(SUNSHINE_PLAYNITE_PLUGIN_ICON
        "${CMAKE_SOURCE_DIR}/src_assets/common/assets/web/public/images/logo-apollo-45.png")
set(SUNSHINE_PLAYNITE_PLUGIN_OUTPUT_FILES
        "${SUNSHINE_PLAYNITE_PLUGIN_DLL}"
        "${SUNSHINE_PLAYNITE_PLUGIN_OUTPUT_DIR}/extension.yaml"
        "${SUNSHINE_PLAYNITE_PLUGIN_OUTPUT_DIR}/icon.png")
file(GLOB SUNSHINE_PLAYNITE_PLUGIN_SOURCES CONFIGURE_DEPENDS
        "${SUNSHINE_PLAYNITE_PLUGIN_SOURCE_DIR}/*.csproj"
        "${SUNSHINE_PLAYNITE_PLUGIN_SOURCE_DIR}/*.props"
        "${SUNSHINE_PLAYNITE_PLUGIN_SOURCE_DIR}/*.targets"
        "${SUNSHINE_PLAYNITE_PLUGIN_SOURCE_DIR}/extension.yaml")
file(GLOB_RECURSE SUNSHINE_PLAYNITE_PLUGIN_CODE CONFIGURE_DEPENDS
        "${SUNSHINE_PLAYNITE_PLUGIN_SOURCE_DIR}/src/*.cs"
        "${SUNSHINE_PLAYNITE_PLUGIN_SOURCE_DIR}/src/*.resx"
        "${SUNSHINE_PLAYNITE_PLUGIN_SOURCE_DIR}/src/*.xaml")
list(APPEND SUNSHINE_PLAYNITE_PLUGIN_SOURCES
        ${SUNSHINE_PLAYNITE_PLUGIN_CODE})

add_custom_command(
        OUTPUT ${SUNSHINE_PLAYNITE_PLUGIN_OUTPUT_FILES}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SUNSHINE_PLAYNITE_PLUGIN_OUTPUT_DIR}"
        COMMAND ${CMAKE_COMMAND} -E rm -f
                "${SUNSHINE_PLAYNITE_PLUGIN_OUTPUT_DIR}/SunshinePlaynite.dll"
                "${SUNSHINE_PLAYNITE_PLUGIN_OUTPUT_DIR}/SunshinePlaynite.pdb"
                "${SUNSHINE_PLAYNITE_PLUGIN_OUTPUT_DIR}/VibeshinePlaynite.dll"
                "${SUNSHINE_PLAYNITE_PLUGIN_OUTPUT_DIR}/VibeshinePlaynite.pdb"
        COMMAND "${SUNSHINE_DOTNET_EXECUTABLE}" build
                "${SUNSHINE_PLAYNITE_PLUGIN_SOURCE_DIR}/SunshinePlaynite.csproj"
                --configuration Release
                --output "${SUNSHINE_PLAYNITE_PLUGIN_OUTPUT_DIR}"
                --nologo
                -p:ContinuousIntegrationBuild=true
        DEPENDS ${SUNSHINE_PLAYNITE_PLUGIN_SOURCES} "${SUNSHINE_PLAYNITE_PLUGIN_ICON}"
        COMMENT "Building Vibepollo Playnite plugin"
        VERBATIM
)
add_custom_target(build_playnite_plugin DEPENDS ${SUNSHINE_PLAYNITE_PLUGIN_OUTPUT_FILES})
add_dependencies(sunshine build_playnite_plugin)
add_dependencies(sunshine sunshine_end_stream_hotkey)

# Ensure the Windows display helper is built and staged under the Sunshine tools
# directory so the runtime launcher can find it reliably.
if (TARGET sunshine_display_helper)
    # Build helper before sunshine to make the copy step reliable
    add_dependencies(sunshine sunshine_display_helper)

    # Copy helper into the tools directory next to the sunshine executable after build
    add_custom_command(TARGET sunshine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:sunshine>/tools"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:sunshine_display_helper>
                "$<TARGET_FILE_DIR:sunshine>/tools"
        COMMENT "Copying sunshine_display_helper into tools directory")
endif()

# Enable libdisplaydevice logging in the main Sunshine binary only
target_compile_definitions(sunshine PRIVATE SUNSHINE_USE_DISPLAYDEVICE_LOGGING)

# Build lightweight uninstall UI executable (same UX as installer, no embedded MSI payload)
set(SUNSHINE_UNINSTALL_UI_EXE "${CMAKE_BINARY_DIR}/uninstall.exe")
add_custom_command(
    OUTPUT "${SUNSHINE_UNINSTALL_UI_EXE}"
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/packaging/windows/bootstrapper/build_bootstrapper.ps1" -BuildDir "${CMAKE_BINARY_DIR}" -UninstallOnly -OutputName "uninstall.exe" -DisableSignPath
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/cpack_artifacts/uninstall.exe" "${SUNSHINE_UNINSTALL_UI_EXE}"
    DEPENDS "${CMAKE_SOURCE_DIR}/packaging/windows/bootstrapper/build_bootstrapper.ps1"
            "${CMAKE_SOURCE_DIR}/packaging/windows/bootstrapper/VibeshineInstaller.cs"
            "${CMAKE_SOURCE_DIR}/packaging/windows/bootstrapper/app.manifest"
            "${CMAKE_SOURCE_DIR}/LICENSE"
            "${CMAKE_SOURCE_DIR}/apollo.ico"
            "${SUNSHINE_WINDOWS_VERSIONINFO_STAMP}"
            generate_windows_versioninfo
    COMMENT "Building lightweight Vibepollo uninstaller UI"
)
add_custom_target(build_uninstall_ui ALL DEPENDS "${SUNSHINE_UNINSTALL_UI_EXE}")

set(SUNSHINE_WINDOWS_PACKAGED_TARGETS sunshine)
list(APPEND SUNSHINE_WINDOWS_PACKAGED_TARGETS vibepollo_dualsense_haptics vibepollo_dualsense_audio)
foreach(_packaged_target IN ITEMS
        dxgi-info
        audio-info
        sunshinesvc
        playnite-launcher
        sunshine_wgc_capture
        sunshine_end_stream_hotkey
        sunshine_display_helper)
    if(TARGET "${_packaged_target}")
        list(APPEND SUNSHINE_WINDOWS_PACKAGED_TARGETS "${_packaged_target}")
    endif()
endforeach()

# Convenience target to build MSI via CPack (WiX)
add_custom_target(package_msi
    COMMAND "${CMAKE_CPACK_COMMAND}" -G WIX -C "$<IF:$<CONFIG:>,${CMAKE_BUILD_TYPE},$<CONFIG>>"
    DEPENDS ${SUNSHINE_WINDOWS_PACKAGED_TARGETS} build_playnite_plugin build_uninstall_ui web_ui
    COMMENT "Building MSI installer via CPack (WiX)"
)

# Build a standalone ZIP with an as-caller physical-desktop launcher.
add_custom_target(package_portable
    COMMAND "${CMAKE_CPACK_COMMAND}" -G ZIP --config "${CMAKE_BINARY_DIR}/CPackConfig.cmake"
        -C "$<IF:$<CONFIG:>,${CMAKE_BUILD_TYPE},$<CONFIG>>"
    DEPENDS ${SUNSHINE_WINDOWS_PACKAGED_TARGETS} build_playnite_plugin build_uninstall_ui web_ui
    COMMENT "Building portable Windows ZIP via CPack"
    VERBATIM
)

# This target never signs: it always builds an unsigned installer. Release
# signing is done exclusively through SignPath origin verification in CI
# (.github/workflows/ci-windows.yml, docs/signpath/), which invokes
# build_bootstrapper.ps1 directly with the already-signed MSI. A runner-local
# signature could never satisfy origin verification anyway.
set(SUNSHINE_BOOTSTRAPPER_SIGNPATH_ARGS -DisableSignPath)

# Build custom elevated installer EXE that wraps the generated MSI
add_custom_target(package_installer
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/packaging/windows/bootstrapper/build_bootstrapper.ps1" -BuildDir "${CMAKE_BINARY_DIR}" -MsiPath "${CMAKE_BINARY_DIR}/cpack_artifacts/${PROJECT_NAME}.msi" ${SUNSHINE_BOOTSTRAPPER_SIGNPATH_ARGS}
    DEPENDS package_msi
    COMMENT "Building custom installer executable"
)
