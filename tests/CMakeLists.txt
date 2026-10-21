cmake_minimum_required(VERSION 3.20)

# This directory is also usable as a standalone entry point for focused test
# work, and is included by the normal project build when BUILD_TESTS is enabled.
project(VibeshineTests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(SUNSHINE_TEST_REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

set(SUNSHINE_TEST_FFMPEG_PLATFORM_LIBRARIES "")
if(WIN32)
    # FFmpeg's Windows random source calls the BCrypt API directly. Component
    # tests that link the static FFmpeg archives must provide the system import
    # library because they do not inherit the application's platform libraries.
    list(APPEND SUNSHINE_TEST_FFMPEG_PLATFORM_LIBRARIES bcrypt)
endif()

# Test categories make the dependency scope of a target visible at configuration
# time. Every target must declare the smallest sources and libraries that
# preserve the behavior it exercises; tests with former runtime dependencies
# belong in a component target after adding a deterministic seam, not a broad
# runtime or integration target.
option(SUNSHINE_TEST_ENABLE_FAST "Configure fast, self-contained test targets" ON)
option(SUNSHINE_TEST_ENABLE_COMPONENT "Configure narrowly scoped component test targets" ON)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/SunshineTestTargets.cmake")

include("${SUNSHINE_TEST_REPOSITORY_ROOT}/cmake/packaging/windows_virtual_display_contract.cmake")

function(sunshine_contract_cpp_list input_variable output_variable)
    set(_contract_cpp_items "")
    foreach(_contract_item IN LISTS ${input_variable})
        string(REPLACE "\\" "\\\\" _contract_item "${_contract_item}")
        string(REPLACE "\"" "\\\"" _contract_item "${_contract_item}")
        list(APPEND _contract_cpp_items "\"${_contract_item}\"")
    endforeach()
    string(JOIN ", " _contract_cpp_list ${_contract_cpp_items})
    set(${output_variable} "${_contract_cpp_list}" PARENT_SCOPE)
endfunction()

function(sunshine_contract_cpp_bool input_variable output_variable)
    if(${input_variable})
        set(${output_variable} true PARENT_SCOPE)
    else()
        set(${output_variable} false PARENT_SCOPE)
    endif()
endfunction()

function(sunshine_extract_config_source_options output_variable)
    file(READ "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/config.cpp" _config_source)
    set(_patterns
        "(string_f|path_f|string_restricted_f)[ \\t\\r\\n]*\\([ \\t\\r\\n]*vars[ \\t\\r\\n]*,[ \\t\\r\\n]*\\\"([^\\\"]+)\\\""
        "(int_f|int_between_f)[ \\t\\r\\n]*\\([ \\t\\r\\n]*vars[ \\t\\r\\n]*,[ \\t\\r\\n]*\\\"([^\\\"]+)\\\""
        "bool_f[ \\t\\r\\n]*\\([ \\t\\r\\n]*vars[ \\t\\r\\n]*,[ \\t\\r\\n]*\\\"([^\\\"]+)\\\""
        "(double_f|double_between_f)[ \\t\\r\\n]*\\([ \\t\\r\\n]*vars[ \\t\\r\\n]*,[ \\t\\r\\n]*\\\"([^\\\"]+)\\\""
        "generic_f[ \\t\\r\\n]*\\([ \\t\\r\\n]*vars[ \\t\\r\\n]*,[ \\t\\r\\n]*\\\"([^\\\"]+)\\\""
        "list_prep_cmd_f[ \\t\\r\\n]*\\([ \\t\\r\\n]*vars[ \\t\\r\\n]*,[ \\t\\r\\n]*\\\"([^\\\"]+)\\\""
        "map_int_int_f[ \\t\\r\\n]*\\([ \\t\\r\\n]*vars[ \\t\\r\\n]*,[ \\t\\r\\n]*\\\"([^\\\"]+)\\\"")

    set(_options "")
    foreach(_pattern IN LISTS _patterns)
        string(REGEX MATCHALL "${_pattern}" _matches "${_config_source}")
        foreach(_match IN LISTS _matches)
            string(REGEX REPLACE ".*vars[ \\t\\r\\n]*,[ \\t\\r\\n]*\\\"([^\\\"]+)\\\".*" "\\1" _option "${_match}")
            list(APPEND _options "${_option}")
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES _options)
    set(${output_variable} "${_options}" PARENT_SCOPE)
endfunction()

function(sunshine_extract_documented_config_options output_variable)
    file(STRINGS "${SUNSHINE_TEST_REPOSITORY_ROOT}/docs/configuration.md" _lines)
    set(_options "")
    foreach(_line IN LISTS _lines)
        if(NOT _line MATCHES "^### ")
            continue()
        endif()
        # This intentionally mirrors the previous unit parser: only H3
        # headings without an embedded markdown heading marker are options.
        string(SUBSTRING "${_line}" 4 -1 _option)
        string(FIND "${_option}" "#" _heading_marker)
        if(NOT _heading_marker EQUAL -1)
            continue()
        endif()
        string(STRIP "${_option}" _option)
        if(NOT _option STREQUAL "")
            list(APPEND _options "${_option}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _options)
    set(${output_variable} "${_options}" PARENT_SCOPE)
endfunction()

function(sunshine_extract_translated_config_options output_variable)
    file(READ "${SUNSHINE_TEST_REPOSITORY_ROOT}/src_assets/common/assets/web/public/assets/locale/en.json" _locale)
    string(JSON _config_length LENGTH "${_locale}" config)
    set(_options "")
    if(_config_length GREATER 0)
        math(EXPR _config_last_index "${_config_length} - 1")
        foreach(_index RANGE 0 ${_config_last_index})
            string(JSON _option MEMBER "${_locale}" config ${_index})
            list(APPEND _options "${_option}")
        endforeach()
    endif()
    set(${output_variable} "${_options}" PARENT_SCOPE)
endfunction()

foreach(_contract_list IN ITEMS
        SUNSHINE_VDD_DRIVER_REQUIRED_FILES
        SUNSHINE_VDD_DRIVER_OPTIONAL_FILES
        SUNSHINE_VDD_VULKAN_LAYER_FILES
        SUNSHINE_VDD_TRUEHDR_FILES
        SUNSHINE_VDD_CONFLICT_PRODUCT_NAMES
        SUNSHINE_VDD_MSI_INSTALL_OPERATIONS
        SUNSHINE_VDD_DRIVER_REBOOT_MARKERS
        SUNSHINE_VDD_RENDER_ADAPTER_SELECTION_ORDER
        SUNSHINE_VDD_HEALTH_RECOVERY_SEQUENCE
        SUNSHINE_VDD_INSTALL_SEQUENCE
        SUNSHINE_VDD_UNINSTALL_SEQUENCE)
    sunshine_contract_cpp_list(${_contract_list} "${_contract_list}_CPP")
endforeach()

foreach(_contract_bool IN ITEMS
        SUNSHINE_VDD_REFRESH_BEFORE_MSI
        SUNSHINE_VDD_INSTALLER_BEST_EFFORT
        SUNSHINE_VDD_FORCE_KILL_UMDF
        SUNSHINE_VDD_REMOVE_LEGACY_DRIVERS
        SUNSHINE_VDD_REPLACE_ONLY_CHANGED_PACKAGE
        SUNSHINE_VDD_REQUIRE_VALID_CATALOG_SIGNATURE
        SUNSHINE_VDD_MSI_TRANSACTIONAL_REPLACEMENT
        SUNSHINE_VDD_MSI_ALLOW_DOWNGRADES
        SUNSHINE_VDD_CONFLICT_NAMES_EXACT
        SUNSHINE_VDD_FACTORY_RESET_REQUIRES_INSTALL_SENTINEL
        SUNSHINE_VDD_CI_REQUIRES_VALID_RELEASE_SIGNATURES
        SUNSHINE_VDD_CI_SELF_SIGN_WITHOUT_PERSISTENT_SECRET
        SUNSHINE_VDD_UPGRADE_SHOWS_DRIVER_CHOICE
        SUNSHINE_VDD_INSTALL_SELECTION_SEEDS_RUNTIME_FLAG
        SUNSHINE_VDD_CLI_PRESERVES_DRIVER_SELECTION
        SUNSHINE_VDD_UNINSTALL_PASSES_REMOVAL_CHOICE
        SUNSHINE_VDD_FORWARD_QUOTED_CLI_ARGUMENTS
        SUNSHINE_VDD_DIRECT_MSI_CONFLICTS_BLOCK
        SUNSHINE_VDD_ADMIN_INSTALL_PREUNINSTALL
        SUNSHINE_VDD_DRIVER_REBOOT_RETURNS_3010
        SUNSHINE_VDD_RUNTIME_SUDOVDA_FALLBACK_ENABLED)
    sunshine_contract_cpp_bool(${_contract_bool} "${_contract_bool}_CPP")
endforeach()

set(SUNSHINE_TEST_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${SUNSHINE_TEST_GENERATED_INCLUDE_DIR}")

# Resource contracts are captured at configure time.  The resulting test has
# no filesystem, JSON-parser, or production file-handler dependency at runtime.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/config.cpp"
    "${SUNSHINE_TEST_REPOSITORY_ROOT}/docs/configuration.md"
    "${SUNSHINE_TEST_REPOSITORY_ROOT}/src_assets/common/assets/web/public/assets/locale/en.json")
sunshine_extract_config_source_options(SUNSHINE_CONFIG_SOURCE_OPTIONS)
sunshine_extract_documented_config_options(SUNSHINE_DOCUMENTED_CONFIG_OPTIONS)
sunshine_extract_translated_config_options(SUNSHINE_TRANSLATED_CONFIG_OPTIONS)
sunshine_contract_cpp_list(SUNSHINE_CONFIG_SOURCE_OPTIONS SUNSHINE_CONFIG_SOURCE_OPTIONS_CPP)
sunshine_contract_cpp_list(SUNSHINE_DOCUMENTED_CONFIG_OPTIONS SUNSHINE_DOCUMENTED_CONFIG_OPTIONS_CPP)
sunshine_contract_cpp_list(SUNSHINE_TRANSLATED_CONFIG_OPTIONS SUNSHINE_TRANSLATED_CONFIG_OPTIONS_CPP)
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/support/config_catalog_contract.h.in"
    "${SUNSHINE_TEST_GENERATED_INCLUDE_DIR}/config_catalog_contract.generated.h"
    @ONLY)
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/support/virtual_display_package_contract.h.in"
    "${SUNSHINE_TEST_GENERATED_INCLUDE_DIR}/virtual_display_package_contract.generated.h"
    @ONLY)

set(SUNSHINE_TEST_GTEST_SOURCE_DIR "" CACHE PATH
    "Optional GoogleTest source tree for an isolated test worktree")
if(SUNSHINE_TEST_GTEST_SOURCE_DIR)
    set(GTEST_SOURCE_DIR "${SUNSHINE_TEST_GTEST_SOURCE_DIR}")
else()
    set(GTEST_SOURCE_DIR "${SUNSHINE_TEST_REPOSITORY_ROOT}/third-party/googletest")
endif()
if(NOT EXISTS "${GTEST_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "GoogleTest is required for the standalone test configuration. "
        "Initialize third-party/googletest before configuring this entry point.")
endif()

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(INSTALL_GMOCK OFF CACHE BOOL "" FORCE)
if(WIN32)
    # Keep the Windows CRT choice local to the standalone test configuration.
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)  # cmake-lint: disable=C0103
endif()
add_subdirectory("${GTEST_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/googletest")

find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
if(NOT TARGET Boost::algorithm OR
   NOT TARGET Boost::asio OR
   NOT TARGET Boost::crc OR
   NOT TARGET Boost::format OR
   NOT TARGET Boost::function OR
   NOT TARGET Boost::log OR
   NOT TARGET Boost::log_setup OR
   NOT TARGET Boost::property_tree OR
   NOT TARGET Boost::process OR
   NOT TARGET Boost::system)
    find_package(Boost CONFIG REQUIRED COMPONENTS
        algorithm
        asio
        crc
        format
        function
        log
        log_setup
        property_tree
        process
        system)
endif()
find_package(OpenSSL REQUIRED)

set(SUNSHINE_TEST_SOCKET_LIBRARIES "")
if(WIN32)
    set(SUNSHINE_TEST_SOCKET_LIBRARIES ws2_32 mswsock)
endif()

# Header-only SDK boundaries are named targets rather than process-wide include
# paths.  A component explicitly opts in to the one external surface it uses;
# this keeps edits in a vendored SDK from rebuilding unrelated test targets.
add_library(sunshine_test_simple_web_headers INTERFACE)
target_include_directories(sunshine_test_simple_web_headers SYSTEM INTERFACE
    "${SUNSHINE_TEST_REPOSITORY_ROOT}/third-party")
target_link_libraries(sunshine_test_simple_web_headers INTERFACE
    Boost::asio OpenSSL::SSL OpenSSL::Crypto)

add_library(sunshine_test_moonlight_headers INTERFACE)
target_include_directories(sunshine_test_moonlight_headers SYSTEM INTERFACE
    "${SUNSHINE_TEST_REPOSITORY_ROOT}/third-party")

add_library(sunshine_test_enet_headers INTERFACE)
target_include_directories(sunshine_test_enet_headers SYSTEM INTERFACE
    "${SUNSHINE_TEST_REPOSITORY_ROOT}/third-party/moonlight-common-c/enet/include")

add_library(sunshine_test_displaydevice_headers INTERFACE)
target_include_directories(sunshine_test_displaydevice_headers SYSTEM INTERFACE
    "${SUNSHINE_TEST_REPOSITORY_ROOT}/third-party/libdisplaydevice/src/common/include"
    "${SUNSHINE_TEST_REPOSITORY_ROOT}/third-party/libdisplaydevice/src/windows/include")

add_library(sunshine_test_nvapi_headers INTERFACE)
target_include_directories(sunshine_test_nvapi_headers SYSTEM INTERFACE
    "${SUNSHINE_TEST_REPOSITORY_ROOT}/third-party/nvapi")

add_library(sunshine_test_nanors_headers INTERFACE)
target_include_directories(sunshine_test_nanors_headers SYSTEM INTERFACE
    "${SUNSHINE_TEST_REPOSITORY_ROOT}/third-party/nanors"
    "${SUNSHINE_TEST_REPOSITORY_ROOT}/third-party/nanors/deps/obl")

enable_testing()

# Exercise the Windows API-independent desktop/input ownership contract on the
# native test host without requiring a physical Windows desktop.
find_program(SUNSHINE_TEST_PYTHON_EXECUTABLE NAMES python3)
if(SUNSHINE_TEST_PYTHON_EXECUTABLE AND NOT WIN32)
    # Exercise exact production quarantine/admission/destructor bodies with
    # controlled CPU leaves. This does not execute Windows or GPU APIs.
    add_test(NAME test_component_pyrowave_capture_guard
        COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/unit/platform/windows/test_pyrowave_capture_guard.py"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}")
    set_tests_properties(test_component_pyrowave_capture_guard PROPERTIES LABELS "component" TIMEOUT 60)
    # V6 extracts actual source staging, completion, collection, release, and
    # teardown bodies with controlled CPU leaves. The separate conversion
    # matrix covers preparation/copy/producer/consumer failure paths. Neither
    # fixture executes Windows or GPU APIs.
    add_test(NAME test_component_pyrowave_source_lease
        COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/unit/platform/windows/test_pyrowave_source_lease.py"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}")
    set_tests_properties(test_component_pyrowave_source_lease PROPERTIES LABELS "component" TIMEOUT 60)
    add_test(NAME test_component_pyrowave_pre_copy
        COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/unit/platform/windows/test_pyrowave_pre_copy.py"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}")
    set_tests_properties(test_component_pyrowave_pre_copy PROPERTIES LABELS "component" TIMEOUT 60)
    add_test(NAME test_component_windows_input_desktop
        COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/tools/tests/test_input_desktop.py"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}"
            "${CMAKE_CXX_COMPILER}")
    set_tests_properties(test_component_windows_input_desktop PROPERTIES LABELS "component" TIMEOUT 30)
    add_test(NAME test_component_control_peer_timeout
        COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/unit/test_control_peer_timeout.py"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}"
            "${CMAKE_CXX_COMPILER}")
    set_tests_properties(test_component_control_peer_timeout PROPERTIES LABELS "component" TIMEOUT 30)
    foreach(_metrics_boundary IN ITEMS config windows_vram)
        add_test(NAME test_component_hardware_metrics_${_metrics_boundary}
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/test_hardware_metrics_${_metrics_boundary}.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}"
                "${CMAKE_CXX_COMPILER}")
        set_tests_properties(test_component_hardware_metrics_${_metrics_boundary} PROPERTIES LABELS "component" TIMEOUT 30)
    endforeach()
endif()

if(WIN32)
    find_program(SUNSHINE_TEST_POWERSHELL_EXECUTABLE NAMES pwsh powershell)
    if(SUNSHINE_TEST_POWERSHELL_EXECUTABLE)
        add_test(NAME test_script_playnite_waveform_bridge
            COMMAND "${SUNSHINE_TEST_POWERSHELL_EXECUTABLE}" -NoLogo -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_LIST_DIR}/unit/platform/windows/test_playnite_waveform_launch.ps1" -CompileBridge)
        set_tests_properties(test_script_playnite_waveform_bridge PROPERTIES LABELS "component;script")
        if(TARGET vibepollo_dualsense_haptics)
            add_test(NAME test_script_playnite_waveform_helper
                COMMAND "${SUNSHINE_TEST_POWERSHELL_EXECUTABLE}" -NoLogo -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_CURRENT_LIST_DIR}/unit/platform/windows/test_playnite_waveform_launch.ps1"
                    -CompileBridge -NativeHelperDirectory "$<TARGET_FILE_DIR:vibepollo_dualsense_haptics>")
            set_tests_properties(test_script_playnite_waveform_helper PROPERTIES TIMEOUT 35 LABELS "component;script")
            set(_playnite_x86_powershell "$ENV{WINDIR}/SysWOW64/WindowsPowerShell/v1.0/powershell.exe")
            if(EXISTS "${_playnite_x86_powershell}")
                add_test(NAME test_script_playnite_waveform_helper_x86
                    COMMAND "${_playnite_x86_powershell}" -NoLogo -NoProfile -ExecutionPolicy Bypass
                        -File "${CMAKE_CURRENT_LIST_DIR}/unit/platform/windows/test_playnite_waveform_launch.ps1"
                        -CompileBridge -NativeHelperDirectory "$<TARGET_FILE_DIR:vibepollo_dualsense_haptics>")
                set_tests_properties(test_script_playnite_waveform_helper_x86 PROPERTIES TIMEOUT 35 LABELS "component;script")
            endif()
        endif()
        add_test(
            NAME test_script_webrtc_build_cleanup
            COMMAND "${SUNSHINE_TEST_POWERSHELL_EXECUTABLE}"
                -NoLogo -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_LIST_DIR}/scripts/test_cleanup_webrtc_build_sources.ps1"
                -RepositoryRoot "${SUNSHINE_TEST_REPOSITORY_ROOT}")
        set_tests_properties(test_script_webrtc_build_cleanup PROPERTIES LABELS "fast;script")
    endif()
endif()

# The foundation target proves that the fast path needs only GTest and the
# universal test entry. Registrations use the test_fast_ and test_component_
# naming convention and must declare their exact sources, definitions, and link
# libraries. None may link the full application source inventory.
set(SUNSHINE_TEST_FAST_FOUNDATION_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/tests_main.cpp"
)

sunshine_add_test_target(
    NAME test_fast_foundation
    CATEGORY fast
    TEST_SOURCES ${SUNSHINE_TEST_FAST_FOUNDATION_SOURCES}
    PRODUCT_SOURCES
    DEFINITIONS
    LINK_LIBRARIES GTest::gtest
    DEPENDENCIES
)

sunshine_add_test_target(
    NAME test_fast_thread_safe
    CATEGORY fast
    TEST_SOURCES
        "${CMAKE_CURRENT_LIST_DIR}/tests_main.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/unit/test_thread_safe.cpp"
    PRODUCT_SOURCES
    DEFINITIONS
    LINK_LIBRARIES GTest::gtest nlohmann_json::nlohmann_json
    DEPENDENCIES
)

if(SUNSHINE_TEST_ENABLE_COMPONENT)
    find_package(Threads REQUIRED)
    sunshine_add_test_target(
        NAME test_component_broadcast_runtime
        CATEGORY component
        TEST_SOURCES
            "${CMAKE_CURRENT_LIST_DIR}/tests_main.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/unit/test_broadcast_runtime.cpp"
        PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/stream_broadcast_runtime.cpp"
        LINK_LIBRARIES GTest::gtest Threads::Threads nlohmann_json::nlohmann_json
    )
    set_tests_properties(test_component_broadcast_runtime PROPERTIES TIMEOUT 30)
    sunshine_add_test_target(
        NAME test_component_nvhttp_dispatch
        CATEGORY component
        TEST_SOURCES
            "${CMAKE_CURRENT_LIST_DIR}/tests_main.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/unit/test_nvhttp_dispatch.cpp"
        PRODUCT_SOURCES
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/stream_broadcast_runtime.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/http_pairing_policy.cpp"
        LINK_LIBRARIES GTest::gtest Threads::Threads nlohmann_json::nlohmann_json Boost::property_tree
    )
    set_tests_properties(test_component_nvhttp_dispatch PROPERTIES TIMEOUT 30)
    sunshine_add_test_target(
        NAME test_component_nvhttp_bitrate_policy
        CATEGORY component
        TEST_SOURCES
            "${CMAKE_CURRENT_LIST_DIR}/tests_main.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/unit/test_nvhttp_bitrate_policy.cpp"
        PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/stream_broadcast_runtime.cpp"
        LINK_LIBRARIES GTest::gtest Threads::Threads nlohmann_json::nlohmann_json Boost::property_tree OpenSSL::Crypto
    )
    set_tests_properties(test_component_nvhttp_bitrate_policy PROPERTIES TIMEOUT 30)
endif()

sunshine_add_test_target(
    NAME test_fast_update_release
    CATEGORY fast
    TEST_SOURCES unit/test_update_release_standalone.cpp
    LINK_LIBRARIES nlohmann_json::nlohmann_json)

sunshine_add_test_target(
    NAME test_component_update_notification_state
    CATEGORY component
    TEST_SOURCES unit/test_update_notification_state_standalone.cpp
    LINK_LIBRARIES Boost::property_tree nlohmann_json::nlohmann_json)

# The source standalone harnesses use assert. Preserve their checks when the
# application and CI test targets are configured in Release mode.
foreach(_update_test IN ITEMS test_fast_update_release test_component_update_notification_state)
    if(TARGET ${_update_test})
        if(MSVC)
            target_compile_options(${_update_test} PRIVATE /UNDEBUG)
        else()
            target_compile_options(${_update_test} PRIVATE -UNDEBUG)
        endif()
    endif()
endforeach()

sunshine_add_test_target(
    NAME test_fast_version_compare
    CATEGORY fast
    TEST_SOURCES
        "${CMAKE_CURRENT_LIST_DIR}/tests_main.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/unit/test_version_compare.cpp"
    PRODUCT_SOURCES
    DEFINITIONS
    LINK_LIBRARIES GTest::gtest
    DEPENDENCIES
)

sunshine_add_test_target(
    NAME test_fast_nvenc_api
    CATEGORY fast
    TEST_SOURCES
        "${CMAKE_CURRENT_LIST_DIR}/tests_main.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/unit/test_nvenc_api.cpp"
    PRODUCT_SOURCES
    DEFINITIONS
    INCLUDE_DIRECTORIES
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/third-party/nv-codec-headers/include"
    LINK_LIBRARIES GTest::gmock GTest::gtest
    DEPENDENCIES
)

sunshine_add_test_target(
    NAME test_component_nvenc_event_wait
    CATEGORY component
    TEST_SOURCES
        "${CMAKE_CURRENT_LIST_DIR}/tests_main.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/unit/test_nvenc_event_wait.cpp"
    PRODUCT_SOURCES
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/nvenc/nvenc_event_wait.cpp"
    LINK_LIBRARIES GTest::gmock GTest::gtest
)

if(NOT WIN32)
    sunshine_add_test_target(
        NAME test_component_end_stream_hotkey_helper
        CATEGORY component
        TEST_SOURCES
            "${CMAKE_CURRENT_LIST_DIR}/tests_main.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/unit/test_end_stream_hotkey_helper.cpp"
        SUPPORT_SOURCES "${CMAKE_CURRENT_LIST_DIR}/support/end_stream_hotkey/entry.cpp"
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/support/end_stream_hotkey"
        LINK_LIBRARIES GTest::gtest
    )
endif()

sunshine_add_test_target(
    NAME test_fast_host_stream_control
    CATEGORY fast
    TEST_SOURCES
        "${CMAKE_CURRENT_LIST_DIR}/tests_main.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/unit/test_host_stream_control.cpp"
    LINK_LIBRARIES GTest::gtest Threads::Threads
)

sunshine_add_test_target(
    NAME test_component_stream_start_dispatch
    CATEGORY component
    TEST_SOURCES
        "${CMAKE_CURRENT_LIST_DIR}/tests_main.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/unit/test_stream_start_dispatch.cpp"
    LINK_LIBRARIES GTest::gtest Threads::Threads Boost::property_tree
)

sunshine_add_test_target(
    NAME test_fast_framegen_policy
    CATEGORY fast
    TEST_SOURCES
        "${CMAKE_CURRENT_LIST_DIR}/tests_main.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/unit/test_framegen_policy.cpp"
    PRODUCT_SOURCES
    DEFINITIONS
    LINK_LIBRARIES GTest::gtest
    DEPENDENCIES
)

sunshine_add_test_target(
    NAME test_fast_display_snapshot_restore
    CATEGORY fast
    TEST_SOURCES
        "${CMAKE_CURRENT_LIST_DIR}/tests_main.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/unit/test_display_snapshot_restore.cpp"
    PRODUCT_SOURCES
    INCLUDE_DIRECTORIES
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/third-party/libdisplaydevice/src/common/include"
    LINK_LIBRARIES GTest::gtest
    DEPENDENCIES
)

# rswrapper.c includes the vendored nanors Reed-Solomon implementation directly,
# so no separate library or server source is required by this target.
set_source_files_properties(
    "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/rswrapper.c"
    PROPERTIES COMPILE_OPTIONS "-ftree-vectorize;-funroll-loops"
)
sunshine_add_test_target(
    NAME test_fast_rswrapper
    CATEGORY fast
    TEST_SOURCES
        "${CMAKE_CURRENT_LIST_DIR}/tests_main.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/unit/test_rswrapper.cpp"
    PRODUCT_SOURCES
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/rswrapper.c"
    DEFINITIONS
    LINK_LIBRARIES GTest::gtest sunshine_test_nanors_headers
    DEPENDENCIES
)

# Every remaining translation unit is registered independently.  This wrapper
# adds only the universal GTest main; callers still spell out every production
# source and library so there is no hidden component bundle.
function(sunshine_register_component)
    cmake_parse_arguments(COMPONENT "" "NAME;TEST_SOURCE" "EXTRA_TEST_SOURCES;SUPPORT_SOURCES;PRODUCT_SOURCES;INCLUDE_DIRECTORIES;LINK_LIBRARIES;DEFINITIONS" ${ARGN})
    sunshine_add_test_target(
        NAME "${COMPONENT_NAME}"
        CATEGORY component
        TEST_SOURCES
            "${CMAKE_CURRENT_LIST_DIR}/tests_main.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/${COMPONENT_TEST_SOURCE}"
            ${COMPONENT_EXTRA_TEST_SOURCES}
        SUPPORT_SOURCES ${COMPONENT_SUPPORT_SOURCES}
        PRODUCT_SOURCES ${COMPONENT_PRODUCT_SOURCES}
        INCLUDE_DIRECTORIES ${COMPONENT_INCLUDE_DIRECTORIES}
        DEFINITIONS SUNSHINE_TESTS ${COMPONENT_DEFINITIONS}
        LINK_LIBRARIES GTest::gtest ${COMPONENT_LINK_LIBRARIES}
        DEPENDENCIES
    )
endfunction()

if(SUNSHINE_TEST_ENABLE_COMPONENT)
    add_subdirectory(process_catalog)
endif()

find_package(Threads REQUIRED)
sunshine_add_test_target(NAME test_component_audio_snapshot CATEGORY component
    TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/unit/test_audio_context.cpp"
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/audio_context.cpp"
    DEFINITIONS SUNSHINE_ENABLE_WEBRTC
    LINK_LIBRARIES Threads::Threads nlohmann_json::nlohmann_json)
if(TARGET test_component_audio_snapshot)
    add_test(NAME test_component_windows_audio_role_restore COMMAND test_component_audio_snapshot roles.)
    add_test(NAME test_component_audio_display_snapshot COMMAND test_component_audio_snapshot context.)
    add_test(NAME test_component_audio_display_transition COMMAND test_component_audio_snapshot transition.)
    add_test(NAME test_component_webrtc_video_owner_unwind COMMAND test_component_audio_snapshot --owner-unwind-video)
    add_test(NAME test_component_webrtc_audio_owner_unwind COMMAND test_component_audio_snapshot --owner-unwind-audio)
    set_tests_properties(test_component_windows_audio_role_restore test_component_audio_display_snapshot
        test_component_audio_display_transition
        test_component_webrtc_video_owner_unwind test_component_webrtc_audio_owner_unwind
        PROPERTIES LABELS "component" TIMEOUT 30)
endif()

sunshine_register_component(NAME test_component_stream_protocol TEST_SOURCE unit/test_stream.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/stream_protocol.cpp")
sunshine_register_component(NAME test_component_video_pacing TEST_SOURCE unit/test_video_pacing.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/stream_protocol.cpp")
if(SUNSHINE_TEST_ENABLE_COMPONENT)
    find_program(SUNSHINE_TEST_PYTHON_EXECUTABLE NAMES python3 python)
    if(SUNSHINE_TEST_PYTHON_EXECUTABLE)
        set(_pacing_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/video-pacing")
        add_custom_command(
            OUTPUT "${_pacing_fixture_dir}/video_pacing_integration.cpp" "${_pacing_fixture_dir}/stream_protocol.h"
            BYPRODUCTS "${_pacing_fixture_dir}/stream_protocol.cpp"
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/test_video_pacing_integration.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}"
                --generate-only --output-dir "${_pacing_fixture_dir}"
            DEPENDS unit/test_video_pacing_integration.py unit/test_video_pacing_integration.cpp.in
                "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/stream.cpp"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/stream.h"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/stream_broadcast_runtime.h"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/stream_protocol.cpp"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/stream_protocol.h"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/pyrowave_policy.h"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/nvhttp.cpp"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/rtsp.cpp"
            VERBATIM)
        sunshine_add_test_target(NAME test_component_video_pacing_admission CATEGORY component
            TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/tests_main.cpp" "${_pacing_fixture_dir}/video_pacing_integration.cpp"
            PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/stream_protocol.cpp"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/stream_broadcast_runtime.cpp"
            INCLUDE_DIRECTORIES "${_pacing_fixture_dir}"
            DEFINITIONS SUNSHINE_TESTS
            LINK_LIBRARIES GTest::gtest Threads::Threads nlohmann_json::nlohmann_json)
        set_tests_properties(test_component_video_pacing_admission PROPERTIES
            LABELS "component;supplementary" TIMEOUT 30)
    endif()
endif()
sunshine_register_component(NAME test_component_remote_session TEST_SOURCE unit/test_remote_session.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/remote_session.cpp"
    LINK_LIBRARIES nlohmann_json::nlohmann_json)
sunshine_register_component(NAME test_component_rtsp_pending_policy TEST_SOURCE unit/test_rtsp_pending_policy.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/rtsp_pending_policy.cpp")
sunshine_register_component(NAME test_component_pyrowave_udp_probe TEST_SOURCE unit/test_pyrowave_udp_probe.cpp
    LINK_LIBRARIES Boost::asio Boost::system ${SUNSHINE_TEST_SOCKET_LIBRARIES})
sunshine_register_component(NAME test_component_pyrowave_policy TEST_SOURCE unit/test_pyrowave_policy.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/pyrowave_policy.cpp")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    sunshine_register_component(NAME test_component_linux_routed_link
        TEST_SOURCE unit/platform/linux/test_routed_link.cpp
        PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/linux/routed_link.cpp"
        LINK_LIBRARIES Boost::asio)
endif()
sunshine_register_component(NAME test_component_hdr_request_policy TEST_SOURCE unit/test_hdr_request_policy.cpp)
sunshine_register_component(NAME test_component_single_flight TEST_SOURCE unit/test_single_flight.cpp)
sunshine_register_component(NAME test_component_display_helper_request_policy TEST_SOURCE unit/test_display_helper_request_policy.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_request_policy.cpp")
sunshine_register_component(NAME test_component_remote_display_topology TEST_SOURCE unit/test_remote_display_topology.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/remote_display_topology.cpp"
    LINK_LIBRARIES nlohmann_json::nlohmann_json)
sunshine_register_component(NAME test_component_config_parse TEST_SOURCE unit/test_config_parse.cpp
    PRODUCT_SOURCES)
sunshine_register_component(NAME test_component_managed_app_focus TEST_SOURCE unit/test_managed_app_focus.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/managed_app_focus.cpp"
                    "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/steam_process_tracker.cpp")
sunshine_register_component(NAME test_component_config_playnite TEST_SOURCE test_config_playnite.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/config_playnite_parse.cpp"
    LINK_LIBRARIES nlohmann_json::nlohmann_json)
set(STEAM_DISCOVERY_TEST_LIBRARIES "")
if(WIN32)
    set(STEAM_DISCOVERY_TEST_LIBRARIES advapi32 shell32 wtsapi32)
endif()
sunshine_register_component(NAME test_component_steam_integration TEST_SOURCE test_steam_integration.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/steam_integration.cpp"
                    "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/provider_scan_protocol.cpp"
    LINK_LIBRARIES nlohmann_json::nlohmann_json ${STEAM_DISCOVERY_TEST_LIBRARIES})
if(NOT WIN32)
    # Run the production Windows token/session/registry adapter against a narrow
    # API shim, without touching host identities, sessions or registry hives.
    sunshine_register_component(NAME test_component_windows_steam_discovery
        TEST_SOURCE unit/platform/windows/test_steam_discovery.cpp
        PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/steam_integration.cpp"
                        "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/provider_scan_protocol.cpp"
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/support/windows_steam_api"
        LINK_LIBRARIES nlohmann_json::nlohmann_json)
endif()
if(WIN32)
    # The static FFmpeg build uses BCrypt for its random seed provider.
    list(APPEND STEAM_ARTWORK_TEST_LIBRARIES bcrypt)
endif()
sunshine_register_component(NAME test_component_steam_artwork TEST_SOURCE test_steam_artwork.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/steam_artwork.cpp"
                    "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/steam_integration.cpp"
                    "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/provider_scan_protocol.cpp"
    INCLUDE_DIRECTORIES ${FFMPEG_INCLUDE_DIRS}
    LINK_LIBRARIES nlohmann_json::nlohmann_json ${FFMPEG_LIBRARIES} ${STEAM_ARTWORK_TEST_LIBRARIES}
                   ${SUNSHINE_TEST_FFMPEG_PLATFORM_LIBRARIES} ${STEAM_DISCOVERY_TEST_LIBRARIES}
    DEFINITIONS ${STEAM_ARTWORK_TEST_DEFINITIONS})
sunshine_register_component(NAME test_component_config_steam TEST_SOURCE test_config_steam.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/config_steam.cpp"
    LINK_LIBRARIES nlohmann_json::nlohmann_json)
sunshine_register_component(NAME test_component_steam_sync TEST_SOURCE test_steam_sync.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/steam_sync_policy.cpp"
                    "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/steam_integration.cpp"
                    "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/provider_scan_protocol.cpp"
    LINK_LIBRARIES nlohmann_json::nlohmann_json ${STEAM_DISCOVERY_TEST_LIBRARIES})
sunshine_register_component(NAME test_component_steam_process_tracker TEST_SOURCE test_steam_process_tracker.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/steam_process_tracker.cpp")
sunshine_register_component(NAME test_component_steam_auto_sync TEST_SOURCE test_steam_auto_sync.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/steam_auto_sync_policy.cpp"
                    "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/steam_integration.cpp"
                    "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/provider_scan_protocol.cpp"
    LINK_LIBRARIES nlohmann_json::nlohmann_json ${STEAM_DISCOVERY_TEST_LIBRARIES})
sunshine_register_component(NAME test_component_config_lutris TEST_SOURCE test_config_lutris.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/config_lutris.cpp"
    LINK_LIBRARIES nlohmann_json::nlohmann_json)
sunshine_register_component(NAME test_component_lutris_integration TEST_SOURCE test_lutris_integration.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/lutris_integration.cpp"
                    "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/provider_scan_protocol.cpp"
                    "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/lutris_artwork.cpp"
                    "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/steam_artwork.cpp"
    INCLUDE_DIRECTORIES ${FFMPEG_INCLUDE_DIRS}
    LINK_LIBRARIES nlohmann_json::nlohmann_json ${SQLITE3_LIBRARIES} ${FFMPEG_LIBRARIES} ${STEAM_ARTWORK_TEST_LIBRARIES}
                   ${SUNSHINE_TEST_FFMPEG_PLATFORM_LIBRARIES} ${STEAM_DISCOVERY_TEST_LIBRARIES}
    DEFINITIONS ${STEAM_ARTWORK_TEST_DEFINITIONS})
sunshine_register_component(NAME test_component_lutris_sync TEST_SOURCE test_lutris_sync.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/lutris_sync_policy.cpp"
                    "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/lutris_integration.cpp"
                    "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/provider_scan_protocol.cpp"
    LINK_LIBRARIES nlohmann_json::nlohmann_json ${SQLITE3_LIBRARIES})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND SUNSHINE_TEST_ENABLE_COMPONENT)
    pkg_check_modules(AUDIO_STATUS_GIO REQUIRED IMPORTED_TARGET gio-2.0)
    sunshine_register_component(NAME test_component_linux_audio_sink_status
        TEST_SOURCE unit/test_linux_audio_sink_status.cpp
        LINK_LIBRARIES nlohmann_json::nlohmann_json PkgConfig::AUDIO_STATUS_GIO)
endif()
sunshine_register_component(NAME test_component_provider_scan_protocol TEST_SOURCE test_provider_scan_protocol.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/provider_scan_protocol.cpp"
    LINK_LIBRARIES nlohmann_json::nlohmann_json)
sunshine_register_component(NAME test_component_file_handler TEST_SOURCE unit/test_file_handler.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/file_handler_core.cpp")
sunshine_register_component(NAME test_component_state_storage TEST_SOURCE unit/test_state_storage.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/state_storage_policy.cpp"
    LINK_LIBRARIES Boost::property_tree nlohmann_json::nlohmann_json)
sunshine_register_component(NAME test_component_pairing_recovery TEST_SOURCE unit/test_pairing_recovery.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/pairing_recovery.cpp"
                    "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/state_storage_policy.cpp"
                    "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/crypto.cpp"
                    "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/http_pairing_policy.cpp"
    LINK_LIBRARIES Boost::property_tree nlohmann_json::nlohmann_json OpenSSL::Crypto
                   $<$<PLATFORM_ID:Windows>:advapi32>)
sunshine_register_component(NAME test_component_virtual_display_scale TEST_SOURCE unit/test_virtual_display_scale.cpp)

# The additive host policy deliberately compiles against the pinned 3.7 control
# headers. A newer installed producer is authorized only by its queried ABI.
set(SUNSHINE_TEST_VDD_DRIVER_ROOT "${SUNSHINE_TEST_REPOSITORY_ROOT}/third-party/libvirtualdisplay/src/driver" CACHE PATH "libvirtualdisplay driver source for catalog consumer tests")
if(EXISTS "${SUNSHINE_TEST_VDD_DRIVER_ROOT}/control_client.cpp")
    sunshine_register_component(NAME test_component_virtual_display_catalog_policy
        TEST_SOURCE unit/platform/windows/test_virtual_display_catalog_policy.cpp
        PRODUCT_SOURCES "${SUNSHINE_TEST_VDD_DRIVER_ROOT}/control_client.cpp" "${SUNSHINE_TEST_VDD_DRIVER_ROOT}/control_protocol.cpp"
        INCLUDE_DIRECTORIES "${SUNSHINE_TEST_VDD_DRIVER_ROOT}/include")
endif()

sunshine_register_component(NAME test_component_color_profile_policy TEST_SOURCE unit/test_color_profile_policy.cpp)
sunshine_register_component(NAME test_component_legacy_restore_event_policy TEST_SOURCE unit/test_legacy_restore_event_policy.cpp)
sunshine_register_component(NAME test_component_recovery_status_policy TEST_SOURCE unit/test_recovery_status_policy.cpp)
sunshine_register_component(NAME test_component_wake_recovery_cleanup_policy
    TEST_SOURCE unit/platform/windows/test_wake_recovery_cleanup_policy.cpp)
sunshine_register_component(NAME test_component_http_auth TEST_SOURCE unit/test_http_auth.cpp
    EXTRA_TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/unit/test_confighttp.cpp"
    PRODUCT_SOURCES
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/http_auth_policy.cpp"
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/http_auth_request_policy.cpp"
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/config_http_policy.cpp"
    LINK_LIBRARIES GTest::gmock nlohmann_json::nlohmann_json sunshine_test_simple_web_headers
        Boost::algorithm Boost::function Boost::property_tree ${SUNSHINE_TEST_SOCKET_LIBRARIES})
sunshine_register_component(NAME test_component_http_policy TEST_SOURCE unit/test_httpcommon.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/http_policy.cpp"
    LINK_LIBRARIES Boost::property_tree)
sunshine_register_component(NAME test_component_https_listener_diagnostics
    TEST_SOURCE unit/test_https_listener_diagnostics.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/http_auth_request_policy.cpp"
    LINK_LIBRARIES nlohmann_json::nlohmann_json sunshine_test_simple_web_headers
        Boost::algorithm Boost::function Boost::property_tree ${SUNSHINE_TEST_SOCKET_LIBRARIES})
if(TARGET test_component_https_listener_diagnostics)
    set_tests_properties(test_component_https_listener_diagnostics PROPERTIES TIMEOUT 30)
endif()
sunshine_register_component(NAME test_component_network_policy TEST_SOURCE unit/test_network.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/network_policy.cpp"
    LINK_LIBRARIES Boost::algorithm Boost::asio ${SUNSHINE_TEST_SOCKET_LIBRARIES})
sunshine_register_component(NAME test_component_input_validation TEST_SOURCE unit/test_input.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/input_validation_policy.cpp"
    LINK_LIBRARIES sunshine_test_moonlight_headers)
sunshine_register_component(NAME test_component_display_device_policy TEST_SOURCE unit/test_display_device.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/display_device_policy.cpp"
    INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
    LINK_LIBRARIES nlohmann_json::nlohmann_json)
sunshine_register_component(NAME test_component_process_policy TEST_SOURCE unit/test_process.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/app_catalog_policy.cpp"
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/remote_session.cpp"
    LINK_LIBRARIES Boost::crc nlohmann_json::nlohmann_json)
sunshine_register_component(NAME test_component_builtin_virtual_display TEST_SOURCE unit/test_builtin_virtual_display.cpp
    INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
    LINK_LIBRARIES nlohmann_json::nlohmann_json Boost::process Boost::log Boost::format sunshine_test_moonlight_headers)
sunshine_register_component(NAME test_component_process_framegen_json TEST_SOURCE unit/test_process_framegen_json.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/app_framegen_config.cpp"
    LINK_LIBRARIES nlohmann_json::nlohmann_json)
sunshine_register_component(NAME test_component_session_history TEST_SOURCE unit/test_session_history.cpp
    PRODUCT_SOURCES
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/session_history_storage.cpp"
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/session_history_storage_diagnostics.cpp"
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/session_history_policy.cpp"
    LINK_LIBRARIES PkgConfig::SQLITE3 nlohmann_json::nlohmann_json)
sunshine_register_component(NAME test_component_hardware_metrics TEST_SOURCE unit/test_hardware_metrics.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/hardware_metrics.cpp"
    LINK_LIBRARIES nlohmann_json::nlohmann_json)
sunshine_register_component(NAME test_component_host_stats TEST_SOURCE unit/test_host_stats.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/host_stats_service.cpp")
sunshine_register_component(NAME test_component_audio_policy TEST_SOURCE unit/test_audio.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/audio_policy.cpp")
sunshine_register_component(NAME test_component_mouse_policy TEST_SOURCE unit/test_mouse.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/mouse_input.cpp")
sunshine_register_component(NAME test_component_platform_common TEST_SOURCE unit/platform/test_common.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/common_services.cpp")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(NOT TARGET sunshine_libvirtualdisplay_uapi)
        if(SUNSHINE_LIBVIRTUALDISPLAY_SOURCE_DIR)
            set(_sunshine_test_libvirtualdisplay_source "${SUNSHINE_LIBVIRTUALDISPLAY_SOURCE_DIR}")
        else()
            set(_sunshine_test_libvirtualdisplay_source
                    "${SUNSHINE_TEST_REPOSITORY_ROOT}/third-party/libvirtualdisplay")
        endif()
        set(_sunshine_test_vibeshine_drm_uapi_dir
                "${_sunshine_test_libvirtualdisplay_source}/linux/vibeshine-drm")
        if(NOT EXISTS "${_sunshine_test_vibeshine_drm_uapi_dir}/vibeshine_drm_uapi.h")
            message(FATAL_ERROR
                    "libvirtualdisplay DRM UAPI header not found: "
                    "${_sunshine_test_vibeshine_drm_uapi_dir}/vibeshine_drm_uapi.h")
        endif()
        add_library(sunshine_libvirtualdisplay_uapi INTERFACE)
        target_include_directories(sunshine_libvirtualdisplay_uapi SYSTEM INTERFACE
                "${_sunshine_test_vibeshine_drm_uapi_dir}")
    endif()
    sunshine_add_test_target(
        NAME test_fast_vibeshine_present_policy
        CATEGORY fast
        TEST_SOURCES
            "${CMAKE_CURRENT_LIST_DIR}/tests_main.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_vibeshine_present_policy.cpp"
        LINK_LIBRARIES GTest::gtest sunshine_libvirtualdisplay_uapi)
    sunshine_register_component(NAME test_component_linux_boost_process_shim TEST_SOURCE unit/platform/linux/test_boost_process_shim.cpp
        LINK_LIBRARIES Boost::process)
    sunshine_register_component(NAME test_component_linux_kmsgrab_selection TEST_SOURCE unit/platform/linux/test_kmsgrab_selection.cpp
        LINK_LIBRARIES sunshine_libvirtualdisplay_uapi)
    sunshine_register_component(NAME test_component_linux_kmsgrab_framebuffer TEST_SOURCE unit/platform/linux/test_kmsgrab_framebuffer.cpp)
    sunshine_register_component(NAME test_component_linux_private_display_cleanup_policy TEST_SOURCE unit/platform/linux/test_private_display_cleanup_policy.cpp
        LINK_LIBRARIES nlohmann_json::nlohmann_json)
    sunshine_register_component(NAME test_component_linux_private_display_recovery_policy TEST_SOURCE unit/platform/linux/test_private_display_recovery_policy.cpp)
    sunshine_register_component(NAME test_component_linux_display_restore_dispatcher TEST_SOURCE unit/platform/linux/test_display_restore_dispatcher.cpp
        LINK_LIBRARIES Threads::Threads)
    sunshine_register_component(NAME test_component_linux_render_device TEST_SOURCE unit/platform/linux/test_render_device.cpp)
    sunshine_register_component(NAME test_component_linux_dmabuf_surface TEST_SOURCE unit/platform/linux/test_dmabuf_surface.cpp)
    pkg_check_modules(TEST_PYROWAVE_IMPORT_DRM QUIET IMPORTED_TARGET libdrm)
    if(TEST_PYROWAVE_IMPORT_DRM_FOUND)
        sunshine_register_component(NAME test_component_linux_kmsgrab_hdr
            TEST_SOURCE unit/platform/linux/test_kmsgrab_hdr.cpp
            LINK_LIBRARIES PkgConfig::TEST_PYROWAVE_IMPORT_DRM Threads::Threads)
    endif()
    find_path(TEST_PYROWAVE_IMPORT_VULKAN_HEADERS vulkan/vulkan_core.h
        HINTS "${SUNSHINE_TEST_REPOSITORY_ROOT}/third-party/build-deps/third-party/FFmpeg/Vulkan-Headers/include"
              "${SUNSHINE_TEST_REPOSITORY_ROOT}/third-party/pyrowave/pyrowave/Granite/third_party/khronos/vulkan-headers/include")
    if(TEST_PYROWAVE_IMPORT_DRM_FOUND AND TEST_PYROWAVE_IMPORT_VULKAN_HEADERS)
        sunshine_register_component(NAME test_component_linux_pyrowave_import_diagnostics
            TEST_SOURCE unit/platform/linux/test_pyrowave_import_diagnostics.cpp
            INCLUDE_DIRECTORIES "${TEST_PYROWAVE_IMPORT_VULKAN_HEADERS}"
            LINK_LIBRARIES PkgConfig::TEST_PYROWAVE_IMPORT_DRM)
    endif()
    sunshine_register_component(NAME test_component_linux_hdr_policy TEST_SOURCE unit/platform/linux/test_hdr_policy.cpp)
    sunshine_register_component(NAME test_component_linux_wayland_hdr_compatibility TEST_SOURCE unit/platform/linux/test_wayland_hdr_compatibility.cpp)
    sunshine_register_component(NAME test_component_linux_private_display_mode_policy TEST_SOURCE unit/platform/linux/test_private_display_mode_policy.cpp)
    sunshine_register_component(NAME test_component_linux_private_display_mode_client
        TEST_SOURCE unit/platform/linux/test_private_display_mode_client.cpp
        PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/linux/private_display_mode_client.cpp")
    sunshine_register_component(NAME test_component_linux_mangohud_policy TEST_SOURCE unit/platform/linux/test_mangohud_policy.cpp)
    add_test(
        NAME test_component_linux_mangohud_wrapper
        COMMAND sh
            "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_mangohud_wrapper.sh"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src_assets/linux/misc/vibepollo-mangohud")
    set_tests_properties(test_component_linux_mangohud_wrapper PROPERTIES LABELS "component")
    sunshine_register_component(NAME test_component_linux_smooth_motion_policy TEST_SOURCE unit/platform/linux/test_smooth_motion_policy.cpp)
    sunshine_register_component(NAME test_component_linux_gamescope_session TEST_SOURCE unit/platform/linux/test_gamescope_session.cpp
        PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/linux/gamescope_session.cpp")
    sunshine_register_component(NAME test_component_linux_capture_fallback TEST_SOURCE unit/platform/linux/test_capture_fallback.cpp)
    pkg_check_modules(TEST_WAYLAND QUIET IMPORTED_TARGET wayland-client)
    if(TEST_WAYLAND_FOUND)
        sunshine_register_component(NAME test_component_linux_wayland_roundtrip
            TEST_SOURCE unit/platform/linux/test_wayland_roundtrip.cpp
            LINK_LIBRARIES PkgConfig::TEST_WAYLAND)
        sunshine_register_component(NAME test_component_linux_wayland_observation
            TEST_SOURCE unit/platform/linux/test_wayland_observation.cpp
            LINK_LIBRARIES PkgConfig::TEST_WAYLAND)
    endif()
    add_test(NAME test_component_linux_steamos_install
        COMMAND bash "${SUNSHINE_TEST_REPOSITORY_ROOT}/packaging/linux/steamos/tests/test-user-install.sh")
    set_tests_properties(test_component_linux_steamos_install PROPERTIES LABELS "component")
    add_test(NAME test_component_linux_steamos_profile
        COMMAND bash "${SUNSHINE_TEST_REPOSITORY_ROOT}/packaging/linux/steamos/tests/test-profile.sh")
    set_tests_properties(test_component_linux_steamos_profile PROPERTIES LABELS "component")
    add_test(NAME test_component_linux_steamos_replace_sunshine
        COMMAND bash "${SUNSHINE_TEST_REPOSITORY_ROOT}/packaging/linux/steamos/tests/test-replace-sunshine.sh")
    set_tests_properties(test_component_linux_steamos_replace_sunshine PROPERTIES LABELS "component")
    find_program(SUNSHINE_TEST_PYTHON_EXECUTABLE NAMES python3)
    if(SUNSHINE_TEST_PYTHON_EXECUTABLE)
        if(TEST_PYROWAVE_IMPORT_DRM_FOUND)
            add_test(NAME test_component_linux_kmsgrab_hdr_worker
                COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                    "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_kmsgrab_hdr_worker.py"
                    "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}")
            set_tests_properties(test_component_linux_kmsgrab_hdr_worker PROPERTIES LABELS "component" TIMEOUT 45)
            add_test(NAME test_component_linux_kmsgrab_hdr_event
                COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                    "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_kmsgrab_hdr_event.py"
                    "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}")
            set_tests_properties(test_component_linux_kmsgrab_hdr_event PROPERTIES LABELS "component" TIMEOUT 45)
        endif()
        add_test(NAME test_component_host_stream_route_composition
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/scripts/test_host_stream_route_composition.py"
                --source "${SUNSHINE_TEST_REPOSITORY_ROOT}"
                --build-dir "${CMAKE_CURRENT_BINARY_DIR}/host-stream-route-composition"
                --compiler "${CMAKE_CXX_COMPILER}"
                --json-include "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
                --boost-include "$<TARGET_PROPERTY:Boost::property_tree,INTERFACE_INCLUDE_DIRECTORIES>")
        set_tests_properties(test_component_host_stream_route_composition PROPERTIES LABELS "component" TIMEOUT 60)
        add_test(NAME test_component_host_stream_terminal_composition
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/scripts/test_host_stream_terminal_composition.py"
                --source "${SUNSHINE_TEST_REPOSITORY_ROOT}"
                --build-dir "${CMAKE_CURRENT_BINARY_DIR}/host-stream-terminal-composition"
                --compiler "${CMAKE_CXX_COMPILER}"
                --json-include "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
                --boost-include "$<TARGET_PROPERTY:Boost::property_tree,INTERFACE_INCLUDE_DIRECTORIES>")
        set_tests_properties(test_component_host_stream_terminal_composition PROPERTIES LABELS "component" TIMEOUT 60)
        add_test(NAME test_component_host_app_start_composition
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/scripts/test_host_app_start_composition.py"
                --source "${SUNSHINE_TEST_REPOSITORY_ROOT}"
                --build-dir "${CMAKE_CURRENT_BINARY_DIR}/host-app-start-composition"
                --compiler "${CMAKE_CXX_COMPILER}"
                --json-include "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
                --boost-include "$<TARGET_PROPERTY:Boost::property_tree,INTERFACE_INCLUDE_DIRECTORIES>")
        set_tests_properties(test_component_host_app_start_composition PROPERTIES LABELS "component" TIMEOUT 60)
        add_library(sunshine_test_host_power_terminal_dependencies INTERFACE)
        target_link_libraries(sunshine_test_host_power_terminal_dependencies INTERFACE Boost::asio Boost::property_tree)
        add_test(NAME test_component_host_power_terminal_composition
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/scripts/test_host_power_terminal_composition.py"
                --source "${SUNSHINE_TEST_REPOSITORY_ROOT}"
                --build-dir "${CMAKE_CURRENT_BINARY_DIR}/host-power-terminal-composition"
                --compiler "${CMAKE_CXX_COMPILER}"
                --json-include "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
                --boost-include "$<TARGET_PROPERTY:sunshine_test_host_power_terminal_dependencies,INTERFACE_INCLUDE_DIRECTORIES>")
        set_tests_properties(test_component_host_power_terminal_composition PROPERTIES LABELS "component" TIMEOUT 60)
        add_test(NAME test_component_webrtc_peer_lifetime_composition
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/scripts/test_webrtc_peer_lifetime_composition.py"
                --source "${SUNSHINE_TEST_REPOSITORY_ROOT}"
                --build-dir "${CMAKE_CURRENT_BINARY_DIR}/webrtc-peer-lifetime-composition"
                --compiler "${CMAKE_CXX_COMPILER}")
        set_tests_properties(test_component_webrtc_peer_lifetime_composition PROPERTIES LABELS "component" TIMEOUT 60)
        add_test(NAME test_component_linux_kmsgrab_initialization
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_kmsgrab_initialization.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}")
        set_tests_properties(test_component_linux_kmsgrab_initialization PROPERTIES LABELS "component" TIMEOUT 30)
        add_test(NAME test_component_virtual_display_startup_reconnect
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/test_virtual_display_startup_reconnect.py"
                --repo "${SUNSHINE_TEST_REPOSITORY_ROOT}" --compiler "${CMAKE_CXX_COMPILER}")
        set_tests_properties(test_component_virtual_display_startup_reconnect PROPERTIES LABELS "component" TIMEOUT 30)
        add_test(NAME test_component_display_capture_admission
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/test_display_capture_admission.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}")
        set_tests_properties(test_component_display_capture_admission PROPERTIES LABELS "component")
        add_test(NAME test_component_capture_restart_diagnostics
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/test_capture_restart_diagnostics.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}")
        set_tests_properties(test_component_capture_restart_diagnostics PROPERTIES LABELS "component")
        add_test(NAME test_component_display_restore_task
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/test_display_restore_task.py" "${CMAKE_CXX_COMPILER}")
        set_tests_properties(test_component_display_restore_task PROPERTIES LABELS "component")
        # Build the extracted production fixture with the same modular Boost
        # usage requirements as other targets; CTest only runs the built binary.
        set(_restore_policy_fixture "${CMAKE_CURRENT_BINARY_DIR}/display-restore-policy/test.cpp")
        add_custom_command(
            OUTPUT "${_restore_policy_fixture}"
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/test_display_restore_policy.py"
                --generate-only --output "${_restore_policy_fixture}"
            DEPENDS unit/test_display_restore_policy.py
                "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/state_storage.cpp"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}/tools/display_helper_paths.h"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}/tools/display_settings_helper.cpp"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}/tools/display_settings_helper_v2.cpp"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/config.cpp"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_integration.cpp"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/win_scheduled_task_manager.cpp"
            VERBATIM)
        add_executable(test_component_display_restore_policy
            "${_restore_policy_fixture}"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/state_storage_policy.cpp")
        target_include_directories(test_component_display_restore_policy PRIVATE "${SUNSHINE_TEST_REPOSITORY_ROOT}")
        target_link_libraries(test_component_display_restore_policy PRIVATE Boost::property_tree)
        # The previous direct compiler invocation kept every assert enabled.
        target_compile_options(test_component_display_restore_policy PRIVATE -Wall -Wextra -Werror -UNDEBUG)
        set_target_properties(test_component_display_restore_policy PROPERTIES
            CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF FOLDER "tests/component")
        add_test(NAME test_component_display_restore_policy
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/test_display_restore_policy.py"
                --binary "$<TARGET_FILE:test_component_display_restore_policy>")
        set_tests_properties(test_component_display_restore_policy PROPERTIES LABELS "component")
        add_test(NAME test_component_legacy_boot_primary_restore
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/test_legacy_boot_primary_restore.py"
                --repo "${SUNSHINE_TEST_REPOSITORY_ROOT}" --compiler "${CMAKE_CXX_COMPILER}")
        set_tests_properties(test_component_legacy_boot_primary_restore PROPERTIES LABELS "component")
        add_test(NAME test_component_display_snapshot_backend
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/test_display_snapshot_backend.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}" --compiler "${CMAKE_CXX_COMPILER}")
        set_tests_properties(test_component_display_snapshot_backend PROPERTIES LABELS "component" TIMEOUT 30)
        add_test(NAME test_component_paused_display_restore
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/test_paused_display_restore.py"
                --compiler "${CMAKE_CXX_COMPILER}")
        set_tests_properties(test_component_paused_display_restore PROPERTIES LABELS "component")

        add_test(NAME test_component_display_helper_recovery
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/test_display_helper_recovery.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}" --compiler "${CMAKE_CXX_COMPILER}")
        set_tests_properties(test_component_display_helper_recovery PROPERTIES LABELS "component")
        add_test(NAME test_component_recovery_status_integration
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/test_recovery_status_integration.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}")
        set_tests_properties(test_component_recovery_status_integration PROPERTIES LABELS "component")
        add_test(NAME test_component_recovery_status_legacy_protocol
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/test_recovery_status_legacy_protocol.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}"
                --json-include "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>")
        set_tests_properties(test_component_recovery_status_legacy_protocol PROPERTIES LABELS "component")
        add_test(NAME test_component_recovery_status_legacy_lifecycle
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/test_recovery_status_legacy_lifecycle.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}")
        set_tests_properties(test_component_recovery_status_legacy_lifecycle PROPERTIES LABELS "component")
        add_test(NAME test_component_recovery_status_liveness
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/test_recovery_status_liveness.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}")
        set_tests_properties(test_component_recovery_status_liveness PROPERTIES LABELS "component")
        add_test(NAME test_component_system_tray_shutdown
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/test_system_tray_shutdown.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}")
        set_tests_properties(test_component_system_tray_shutdown PROPERTIES LABELS "component" TIMEOUT 45)
        add_test(NAME test_component_process_undo_shutdown
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/test_process_undo_shutdown.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}"
                "-I$<JOIN:$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>,;-I>"
            COMMAND_EXPAND_LISTS)
        set_tests_properties(test_component_process_undo_shutdown PROPERTIES LABELS "component" TIMEOUT 45)
        add_test(NAME test_component_nvprefs_shutdown
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/test_nvprefs_shutdown.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}")
        set_tests_properties(test_component_nvprefs_shutdown PROPERTIES LABELS "component" TIMEOUT 45)
        add_test(NAME test_component_nvprefs_cleanup
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/test_nvprefs_cleanup.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}")
        set_tests_properties(test_component_nvprefs_cleanup PROPERTIES LABELS "component" TIMEOUT 45)
        add_test(NAME test_component_windows_streaming_scheduling
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/platform/windows/test_streaming_scheduling.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}")
        set_tests_properties(test_component_windows_streaming_scheduling PROPERTIES LABELS "component" TIMEOUT 60)
        add_test(NAME test_component_recovery_event_notifications
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/test_recovery_event_notifications.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}")
        set_tests_properties(test_component_recovery_event_notifications PROPERTIES LABELS "component")
        if(WAYLAND_FOUND)
            add_test(NAME test_component_linux_display_event_observer
                COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                    "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_display_event_observer.py"
                    "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_BINARY_DIR}/generated-src"
                    "${CMAKE_C_COMPILER}" "${CMAKE_CXX_COMPILER}")
            set_tests_properties(test_component_linux_display_event_observer PROPERTIES LABELS "component")
        endif()
        # The Python fixture invokes the compiler itself, so export the resolved
        # imported-target include closure instead of relying on global headers.
        add_library(sunshine_test_wake_observer_dependencies INTERFACE)
        target_link_libraries(sunshine_test_wake_observer_dependencies INTERFACE Boost::algorithm)
        set(_wake_observer_include_file "${CMAKE_CURRENT_BINARY_DIR}/wake-observer-includes-$<CONFIG>.txt")
        file(GENERATE OUTPUT "${_wake_observer_include_file}"
            CONTENT "$<JOIN:$<TARGET_PROPERTY:sunshine_test_wake_observer_dependencies,INTERFACE_INCLUDE_DIRECTORIES>,\n>\n")
        add_test(NAME test_component_wake_recovery_observer
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/test_wake_recovery_observer.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}" "${_wake_observer_include_file}")
        set_tests_properties(test_component_wake_recovery_observer PROPERTIES LABELS "component")
        add_test(NAME test_component_recovery_status_ipc
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/test_recovery_status_ipc.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}")
        set_tests_properties(test_component_recovery_status_ipc PROPERTIES LABELS "component")
        add_test(NAME test_component_linux_global_limiter
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_global_limiter.py")
        set_tests_properties(test_component_linux_global_limiter PROPERTIES LABELS "component")
        add_test(NAME test_component_linux_steamos_local_hdr
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}/packaging/linux/steamos/tests/test-local-hdr.py")
        set_tests_properties(test_component_linux_steamos_local_hdr PROPERTIES LABELS "component")
        add_test(NAME test_component_linux_steamos_pairing_migration
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}/packaging/linux/steamos/tests/test-pairing-migration.py")
        set_tests_properties(test_component_linux_steamos_pairing_migration PROPERTIES LABELS "component")
    endif()
    sunshine_register_component(NAME test_component_linux_pipewire_cuda_policy TEST_SOURCE unit/platform/linux/test_pipewire_cuda_policy.cpp)
    sunshine_register_component(NAME test_component_linux_cuda_interop TEST_SOURCE unit/platform/linux/test_cuda_interop.cpp)
    sunshine_register_component(NAME test_component_linux_cuda_render_device TEST_SOURCE unit/platform/linux/test_cuda_render_device.cpp)
    if(TARGET test_component_linux_cuda_interop)
        target_include_directories(test_component_linux_cuda_interop SYSTEM PRIVATE
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/third-party/nv-codec-headers/include")
    endif()
    sunshine_register_component(NAME test_component_linux_vulkan_encode_policy TEST_SOURCE unit/platform/linux/test_vulkan_encode_policy.cpp)
    sunshine_register_component(NAME test_component_linux_capture_status TEST_SOURCE unit/platform/linux/test_capture_status.cpp)
    sunshine_register_component(NAME test_component_linux_private_display_capture_policy TEST_SOURCE unit/platform/linux/test_private_display_capture_policy.cpp)
    sunshine_register_component(NAME test_component_linux_private_display_restore_policy TEST_SOURCE unit/platform/linux/test_private_display_restore_policy.cpp
        LINK_LIBRARIES nlohmann_json::nlohmann_json)
    sunshine_register_component(NAME test_component_linux_private_display_restore_transaction TEST_SOURCE unit/platform/linux/test_private_display_restore_transaction.cpp
        LINK_LIBRARIES nlohmann_json::nlohmann_json)
    sunshine_register_component(NAME test_component_linux_private_display_resume_policy TEST_SOURCE unit/platform/linux/test_private_display_resume_policy.cpp)
    pkg_check_modules(TEST_PIPEWIRE QUIET IMPORTED_TARGET libpipewire-0.3)
    if(TEST_PIPEWIRE_FOUND)
        sunshine_register_component(NAME test_component_linux_pipewire_format
            TEST_SOURCE unit/platform/linux/test_pipewire_format.cpp
            LINK_LIBRARIES PkgConfig::TEST_PIPEWIRE)
    endif()
endif()
find_package(ZLIB REQUIRED)
sunshine_register_component(NAME test_component_log_export_reader TEST_SOURCE unit/test_log_export_reader.cpp
    LINK_LIBRARIES Boost::asio Boost::crc ZLIB::ZLIB ${SUNSHINE_TEST_SOCKET_LIBRARIES})
sunshine_register_component(NAME test_component_log_sessions TEST_SOURCE unit/test_log_sessions.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/logging.cpp" "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/logging_policy.cpp"
    DEFINITIONS SUNSHINE_TESTS
    LINK_LIBRARIES Boost::log Boost::asio Boost::crc Boost::format ZLIB::ZLIB ${SUNSHINE_TEST_SOCKET_LIBRARIES})
sunshine_register_component(NAME test_component_logging_policy TEST_SOURCE unit/test_logging.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/logging_policy.cpp")
# Exercise the Windows SCM calls with a narrow API shim on non-Windows hosts.
if(NOT WIN32)
    sunshine_register_component(NAME test_component_windows_service_startup
        TEST_SOURCE unit/platform/windows/test_service_startup.cpp
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/support/windows_service_api")
endif()
sunshine_register_component(NAME test_component_entry_metadata TEST_SOURCE unit/test_entry_handler.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/entry_metadata.cpp")
sunshine_register_component(NAME test_component_config_http_policy TEST_SOURCE unit/test_config_http_policy.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/config_http_policy.cpp"
    LINK_LIBRARIES Boost::asio Boost::system ${SUNSHINE_TEST_SOCKET_LIBRARIES})
sunshine_register_component(NAME test_component_http_pairing_policy TEST_SOURCE unit/test_http_pairing.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/http_pairing_policy.cpp"
                    "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/state_storage_policy.cpp"
    LINK_LIBRARIES nlohmann_json::nlohmann_json Boost::property_tree)
sunshine_register_component(NAME test_component_tls_client_identity TEST_SOURCE unit/test_tls_client_identity.cpp
    PRODUCT_SOURCES
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/tls_client_identity.cpp"
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/tls_client_identity_native.cpp"
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/http_pairing_policy.cpp"
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/crypto.cpp"
    LINK_LIBRARIES nlohmann_json::nlohmann_json sunshine_test_simple_web_headers ${SUNSHINE_TEST_SOCKET_LIBRARIES})
sunshine_register_component(NAME test_component_power_action TEST_SOURCE unit/test_power_action.cpp
    PRODUCT_SOURCES
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/power_action.cpp"
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/tls_client_identity.cpp"
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/http_pairing_policy.cpp"
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/crypto.cpp"
    LINK_LIBRARIES nlohmann_json::nlohmann_json sunshine_test_simple_web_headers Boost::property_tree ${SUNSHINE_TEST_SOCKET_LIBRARIES})
if(TARGET test_component_power_action)
    set_tests_properties(test_component_power_action PROPERTIES TIMEOUT 30)
endif()
if(NOT WIN32)
    sunshine_register_component(NAME test_component_power_action_native
        TEST_SOURCE unit/test_power_action_native.cpp
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/support/power_api")
    sunshine_register_component(NAME test_component_power_action_native_legacy_headers
        TEST_SOURCE unit/test_power_action_native.cpp
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/support/power_api"
        DEFINITIONS SUNSHINE_TEST_LEGACY_POWER_CAPABILITIES)
endif()
if(TARGET test_component_tls_client_identity)
    set_tests_properties(test_component_tls_client_identity PROPERTIES TIMEOUT 90)
endif()
sunshine_register_component(NAME test_component_video_policy TEST_SOURCE unit/test_video.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/video_policy.cpp"
    LINK_LIBRARIES nlohmann_json::nlohmann_json)
sunshine_register_component(NAME test_component_video_capture_source TEST_SOURCE unit/test_video_capture_source.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/video_policy.cpp"
    LINK_LIBRARIES nlohmann_json::nlohmann_json)
sunshine_register_component(NAME test_component_video_colorspace TEST_SOURCE unit/test_video_colorspace.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/video_colorspace.cpp"
                    "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/logging.cpp"
                    "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/logging_policy.cpp"
    INCLUDE_DIRECTORIES ${FFMPEG_INCLUDE_DIRS}
    LINK_LIBRARIES nlohmann_json::nlohmann_json Boost::log Boost::asio Boost::crc Boost::format Boost::process ZLIB::ZLIB ${FFMPEG_LIBRARIES}
                   ${SUNSHINE_TEST_FFMPEG_PLATFORM_LIBRARIES} ${SUNSHINE_TEST_SOCKET_LIBRARIES})
sunshine_register_component(NAME test_component_wgc_monitor_selection TEST_SOURCE unit/test_wgc_monitor_selection.cpp)
sunshine_register_component(NAME test_component_screen_saver_state TEST_SOURCE unit/test_screen_saver_state.cpp
    LINK_LIBRARIES nlohmann_json::nlohmann_json)
if(SUNSHINE_TEST_ENABLE_COMPONENT AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    find_program(SUNSHINE_TEST_WGC_PYTHON_EXECUTABLE NAMES python3)
    if(SUNSHINE_TEST_WGC_PYTHON_EXECUTABLE)
        add_test(NAME test_component_video_capture_demand
            COMMAND "${SUNSHINE_TEST_WGC_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/test_video_capture_demand.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}"
                --include-directory "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>")
        set_tests_properties(test_component_video_capture_demand PROPERTIES LABELS "component" TIMEOUT 60)
        add_test(NAME test_component_wgc_source_admission
            COMMAND "${SUNSHINE_TEST_WGC_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/platform/windows/test_wgc_source_admission.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}")
        set_tests_properties(test_component_wgc_source_admission PROPERTIES LABELS "component" TIMEOUT 30)
        add_test(NAME test_component_wgc_gpu_priority
            COMMAND "${SUNSHINE_TEST_WGC_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/platform/windows/test_wgc_gpu_priority.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}")
        set_tests_properties(test_component_wgc_gpu_priority PROPERTIES LABELS "component" TIMEOUT 30)
        add_test(NAME test_component_wgc_capture_stop
            COMMAND "${SUNSHINE_TEST_WGC_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/platform/windows/test_wgc_capture_stop.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}"
                --include-directory "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>")
        set_tests_properties(test_component_wgc_capture_stop PROPERTIES LABELS "component" TIMEOUT 30)
        add_test(NAME test_component_wgc_anonymous_stop
            COMMAND "${SUNSHINE_TEST_WGC_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/platform/windows/test_wgc_anonymous_stop.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}")
        set_tests_properties(test_component_wgc_anonymous_stop PROPERTIES LABELS "component" TIMEOUT 30)
        add_test(NAME test_component_wgc_cursor_visibility
            COMMAND "${SUNSHINE_TEST_WGC_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/platform/windows/test_wgc_cursor_visibility.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}")
        set_tests_properties(test_component_wgc_cursor_visibility PROPERTIES LABELS "component" TIMEOUT 30)
    endif()
endif()
sunshine_register_component(NAME test_component_present_timing_policy TEST_SOURCE unit/test_present_timing_policy.cpp)
sunshine_register_component(NAME test_component_encoder_probe_policy TEST_SOURCE unit/test_encoder_probe_policy.cpp)
sunshine_register_component(NAME test_component_resource_config_catalog TEST_SOURCE unit/test_config_catalog_contract.cpp
    INCLUDE_DIRECTORIES "${SUNSHINE_TEST_GENERATED_INCLUDE_DIR}")
sunshine_register_component(NAME test_component_resource_locale_catalog TEST_SOURCE integration/test_locale_consistency.cpp
    SUPPORT_SOURCES "${CMAKE_CURRENT_LIST_DIR}/support/file_handler_read_only.cpp"
    LINK_LIBRARIES nlohmann_json::nlohmann_json)
set_tests_properties(test_component_resource_locale_catalog PROPERTIES
    WORKING_DIRECTORY "${SUNSHINE_TEST_REPOSITORY_ROOT}")
sunshine_register_component(NAME test_component_external_command_policy TEST_SOURCE integration/test_external_commands.cpp)
sunshine_register_component(NAME test_component_virtual_display_manifest TEST_SOURCE integration/test_virtual_display_packaging.cpp
    INCLUDE_DIRECTORIES "${SUNSHINE_TEST_GENERATED_INCLUDE_DIR}")

find_program(SUNSHINE_TEST_POWERSHELL NAMES pwsh powershell)
if(SUNSHINE_TEST_POWERSHELL AND SUNSHINE_TEST_ENABLE_COMPONENT)
    add_test(NAME test_windows_vulkan_hdr_layer_registration
        COMMAND "${SUNSHINE_TEST_POWERSHELL}" -NoLogo -NoProfile -NonInteractive
            -File "${CMAKE_CURRENT_LIST_DIR}/integration/test_windows_vulkan_hdr_layer.ps1"
            -RepositoryRoot "${SUNSHINE_TEST_REPOSITORY_ROOT}")
    set_tests_properties(test_windows_vulkan_hdr_layer_registration PROPERTIES LABELS COMPONENT)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND SUNSHINE_TEST_ENABLE_COMPONENT)
    sunshine_register_component(NAME test_component_linux_secure_open
        TEST_SOURCE unit/platform/linux/test_secure_open.cpp
        PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/linux/secure_open.cpp")

    add_executable(test_linux_capability_sanitizer
        "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_capability_sanitizer.cpp")
    target_include_directories(test_linux_capability_sanitizer PRIVATE "${LIBCAP_INCLUDE_DIRS}")
    target_link_libraries(test_linux_capability_sanitizer PRIVATE "${LIBCAP_LIBRARIES}")
    if(SUNSHINE_BUILD_STEAMOS)
        target_compile_definitions(test_linux_capability_sanitizer PRIVATE SUNSHINE_BUILD_STEAMOS=1)
    endif()
    add_test(NAME test_component_linux_capability_sanitizer COMMAND test_linux_capability_sanitizer)
    set_tests_properties(test_component_linux_capability_sanitizer PROPERTIES LABELS "component")

    add_executable(test_linux_maintenance_cli
        "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_maintenance_cli.cpp")
    add_test(NAME test_component_linux_maintenance_cli COMMAND test_linux_maintenance_cli)
    set_tests_properties(test_component_linux_maintenance_cli PROPERTIES LABELS "component")

    add_executable(test_linux_session_exec_policy
        "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_session_exec_policy.c")
    pkg_check_modules(TEST_GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)
    sunshine_register_component(NAME test_component_linux_display_helper_process TEST_SOURCE unit/platform/linux/test_display_helper_process.cpp
        LINK_LIBRARIES PkgConfig::TEST_GIO Threads::Threads)
    target_include_directories(test_linux_session_exec_policy PRIVATE "${LIBCAP_INCLUDE_DIRS}")
    target_link_libraries(test_linux_session_exec_policy PRIVATE "${LIBCAP_LIBRARIES}")
    add_test(NAME test_component_linux_session_exec_policy COMMAND test_linux_session_exec_policy)
    set_tests_properties(test_component_linux_session_exec_policy PROPERTIES LABELS "component")

    add_executable(test_linux_display_borrow_fence
        "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_display_borrow_fence.c")
    target_include_directories(test_linux_display_borrow_fence PRIVATE "${SUNSHINE_TEST_REPOSITORY_ROOT}")
    add_test(NAME test_component_linux_display_borrow_fence COMMAND test_linux_display_borrow_fence)
    set_tests_properties(test_component_linux_display_borrow_fence PROPERTIES LABELS "component" TIMEOUT 5)

    add_executable(test_linux_touch_map_wait
        "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_touch_map_wait.cpp")
    target_link_libraries(test_linux_touch_map_wait PRIVATE PkgConfig::TEST_GIO Threads::Threads)
    add_test(NAME test_component_linux_touch_map_wait COMMAND test_linux_touch_map_wait)
    set_tests_properties(test_component_linux_touch_map_wait PROPERTIES LABELS "component" TIMEOUT 5)

    pkg_check_modules(TEST_TOUCH_WAYLAND IMPORTED_TARGET wayland-client wayland-server)
    if(TEST_TOUCH_WAYLAND_FOUND)
        add_executable(test_linux_touch_output
            "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_touch_output.c")
        target_link_libraries(test_linux_touch_output PRIVATE PkgConfig::TEST_TOUCH_WAYLAND PkgConfig::TEST_GIO Threads::Threads)
        add_test(NAME test_component_linux_touch_output COMMAND test_linux_touch_output)
        set_tests_properties(test_component_linux_touch_output PROPERTIES LABELS "component" TIMEOUT 5)
    endif()

    add_executable(test_linux_touch_map
        "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_touch_map.c")
    target_link_libraries(test_linux_touch_map PRIVATE PkgConfig::TEST_GIO)
    add_test(NAME test_component_linux_touch_map COMMAND test_linux_touch_map)
    set_tests_properties(test_component_linux_touch_map PROPERTIES LABELS "component" TIMEOUT 15)

    add_executable(test_linux_display_power
        "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_display_power.c")
    target_link_libraries(test_linux_display_power PRIVATE PkgConfig::TEST_GIO)
    add_test(NAME test_component_linux_display_power COMMAND test_linux_display_power)
    set_tests_properties(test_component_linux_display_power PROPERTIES LABELS "component" TIMEOUT 30)

    add_executable(test_linux_display_power_command
        "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_display_power_command.c")
    target_link_libraries(test_linux_display_power_command PRIVATE PkgConfig::TEST_GIO)
    add_test(NAME test_component_linux_display_power_command COMMAND test_linux_display_power_command)
    set_tests_properties(test_component_linux_display_power_command PROPERTIES LABELS "component" TIMEOUT 5)

    add_executable(test_linux_display_power_lease
        "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_display_power_lease.cpp")
    target_link_libraries(test_linux_display_power_lease PRIVATE
        PkgConfig::TEST_GIO Threads::Threads Boost::log Boost::format)
    add_test(NAME test_component_linux_display_power_lease COMMAND test_linux_display_power_lease)
    set_tests_properties(test_component_linux_display_power_lease PROPERTIES LABELS "component" TIMEOUT 15)

    add_executable(test_linux_app_supervisor
        "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_app_supervisor.c")
    target_include_directories(test_linux_app_supervisor PRIVATE "${LIBCAP_INCLUDE_DIRS}")
    target_link_libraries(test_linux_app_supervisor PRIVATE "${LIBCAP_LIBRARIES}")
    if(TARGET vibepollo_app_supervisor)
        add_test(
            NAME test_component_linux_app_supervisor
            COMMAND test_linux_app_supervisor "$<TARGET_FILE:vibepollo_app_supervisor>")
        set_tests_properties(test_component_linux_app_supervisor PROPERTIES LABELS "component")
    endif()

    add_executable(test_linux_session_exec_client
        "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_session_exec_client.c")
    target_include_directories(test_linux_session_exec_client PRIVATE "${LIBCAP_INCLUDE_DIRS}")
    target_link_libraries(test_linux_session_exec_client PRIVATE "${LIBCAP_LIBRARIES}")
    add_test(NAME test_component_linux_session_exec_client COMMAND test_linux_session_exec_client)
    set_tests_properties(test_component_linux_session_exec_client PROPERTIES LABELS "component")

    add_executable(test_linux_hyprland_broker
        "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_hyprland_broker.c")
    target_include_directories(test_linux_hyprland_broker PRIVATE "${LIBCAP_INCLUDE_DIRS}")
    target_link_libraries(test_linux_hyprland_broker PRIVATE "${LIBCAP_LIBRARIES}")
    add_test(NAME test_component_linux_hyprland_broker COMMAND test_linux_hyprland_broker)
    set_tests_properties(test_component_linux_hyprland_broker PROPERTIES LABELS "component")
    if(SUNSHINE_TEST_PYTHON_EXECUTABLE)
        add_test(NAME test_component_linux_hyprland_backend
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_hyprland_backend.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}")
        set_tests_properties(test_component_linux_hyprland_backend PROPERTIES LABELS "component")
        add_test(NAME test_component_linux_hyprland_sdr
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_hyprland_sdr.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}")
        set_tests_properties(test_component_linux_hyprland_sdr PROPERTIES LABELS "component")
        add_test(NAME test_component_linux_hyprland_property_failure
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_hyprland_property_failure.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}")
        set_tests_properties(test_component_linux_hyprland_property_failure PROPERTIES LABELS "component")
        add_test(NAME test_component_linux_kmsgrab_startup
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_kmsgrab_startup.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}")
        set_tests_properties(test_component_linux_kmsgrab_startup PROPERTIES LABELS "component" TIMEOUT 45)
        add_test(NAME test_component_linux_hyprland_socket_peer
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_hyprland_socket_peer.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}")
        set_tests_properties(test_component_linux_hyprland_socket_peer PROPERTIES LABELS "component")
    endif()

    add_executable(test_linux_kwin_session_environment
        "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_kwin_session_environment.c")
    add_test(NAME test_component_linux_kwin_session_environment
        COMMAND test_linux_kwin_session_environment)
    set_tests_properties(test_component_linux_kwin_session_environment PROPERTIES LABELS "component")

    find_program(SUNSHINE_TEST_BASH_EXECUTABLE bash)
    if(SUNSHINE_TEST_BASH_EXECUTABLE)
        add_test(NAME test_component_linux_hyprland_session
            COMMAND "${SUNSHINE_TEST_BASH_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_hyprland_session.sh"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}")
        set_tests_properties(test_component_linux_hyprland_session PROPERTIES LABELS "component")
        add_test(NAME test_component_linux_hyprland_lifecycle
            COMMAND "${SUNSHINE_TEST_BASH_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_hyprland_lifecycle.sh"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}")
        set_tests_properties(test_component_linux_hyprland_lifecycle PROPERTIES LABELS "component")
        add_test(NAME test_component_linux_vibeshine_ds5_install
            COMMAND "${SUNSHINE_TEST_BASH_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_ds5_install.sh"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}/packaging/linux/vibeshine-ds5-install.in")
        set_tests_properties(test_component_linux_vibeshine_ds5_install PROPERTIES LABELS "component")
        add_test(
            NAME test_component_linux_vkms_setup
            COMMAND "${SUNSHINE_TEST_BASH_EXECUTABLE}"
                "${SUNSHINE_LIBVIRTUALDISPLAY_SOURCE_DIR}/tests/linux/test-vibeshine-vkms.sh"
                "${LIBVIRTUALDISPLAY_LINUX_ROOT}/packaging/vibeshine-vkms")
        set_tests_properties(test_component_linux_vkms_setup PROPERTIES LABELS "component")
        add_test(
            NAME test_component_linux_vibepollo_drm_install
            COMMAND "${SUNSHINE_TEST_BASH_EXECUTABLE}"
                "${SUNSHINE_LIBVIRTUALDISPLAY_SOURCE_DIR}/tests/linux/test-vibeshine-drm-install.sh"
                "${LIBVIRTUALDISPLAY_LINUX_ROOT}/packaging/vibeshine-drm-install.in")
        set_tests_properties(test_component_linux_vibepollo_drm_install PROPERTIES LABELS "component")
        add_test(
            NAME test_component_linux_vibepollo_drm_build_module
            COMMAND "${SUNSHINE_TEST_BASH_EXECUTABLE}"
                "${SUNSHINE_LIBVIRTUALDISPLAY_SOURCE_DIR}/tests/linux/test-vibeshine-drm-build-module.sh"
                "${LIBVIRTUALDISPLAY_LINUX_ROOT}/vibeshine-drm/build-module"
                "${LIBVIRTUALDISPLAY_LINUX_ROOT}/vibeshine-drm/dkms.conf.in")
        set_tests_properties(test_component_linux_vibepollo_drm_build_module PROPERTIES LABELS "component")
        add_test(
            NAME test_component_linux_session_controller
            COMMAND "${SUNSHINE_TEST_BASH_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_session_controller.sh"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}/packaging/linux/vibepollo-session-controller")
        set_tests_properties(test_component_linux_session_controller PROPERTIES LABELS "component")
        add_test(NAME test_component_linux_session_classifier
            COMMAND bash "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_session_classifier.sh"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}/packaging/linux/vibepollo-session-controller")
        set_tests_properties(test_component_linux_session_classifier PROPERTIES LABELS "component")
        add_test(
            NAME test_component_linux_session_command_timeout
            COMMAND "${SUNSHINE_TEST_BASH_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_session_command_timeout.sh"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}/packaging/linux/vibepollo-session-controller")
        set_tests_properties(test_component_linux_session_command_timeout PROPERTIES LABELS "component" TIMEOUT 15)
        add_test(
            NAME test_component_linux_machine_host_manifest
            COMMAND "${SUNSHINE_TEST_BASH_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_machine_host_manifest.sh"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}/packaging/linux/vibepollo-machine-host")
        set_tests_properties(test_component_linux_machine_host_manifest PROPERTIES LABELS "component")
    else()
        message(WARNING "bash was not found; skipping the Linux VKMS setup shell test")
    endif()

    find_program(SUNSHINE_TEST_PYTHON_EXECUTABLE NAMES python3)
    if(SUNSHINE_TEST_PYTHON_EXECUTABLE)
        if(SUNSHINE_TEST_BASH_EXECUTABLE)
            add_test(NAME test_component_linux_machine_locale
                COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                    "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_machine_locale.py"
                    "${SUNSHINE_TEST_REPOSITORY_ROOT}")
            set_tests_properties(test_component_linux_machine_locale PROPERTIES LABELS "component" TIMEOUT 30)
            foreach(package_fixture IN ITEMS
                    package_kill_mode package_metadata package_host_stop package_host_scope package_cgroup_absence package_fuse_identity package_quiesce_diagnostics package_transaction_recovery package_postconfigure
                    package_removal package_masked_host package_mask_ownership package_arch_guard package_force_backup installer_compat installer_fence_failure installer_fuse_bootstrap installer_ownership installer_real_guard installer_2_0_1 installer_hookdirs)
                add_test(NAME "test_component_linux_${package_fixture}"
                    COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                        "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_${package_fixture}.py"
                        "${SUNSHINE_TEST_REPOSITORY_ROOT}" "vibepollo")
                set_tests_properties("test_component_linux_${package_fixture}"
                    PROPERTIES LABELS "component" TIMEOUT 240)
            endforeach()
            # Recovery exercises real subprocess publication/crash checkpoints.
            set_tests_properties(test_component_linux_package_transaction_recovery
                PROPERTIES TIMEOUT 600)
            add_test(NAME test_component_linux_profile_upgrade
                COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                    "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_profile_upgrade.py"
                    "${SUNSHINE_TEST_REPOSITORY_ROOT}")
            set_tests_properties(test_component_linux_profile_upgrade PROPERTIES LABELS "component")
        endif()
        add_test(
            NAME test_component_linux_machine_service_contract
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_machine_service_contract.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}")
        set_tests_properties(test_component_linux_machine_service_contract PROPERTIES LABELS "component")
        add_test(NAME test_component_machine_status_polling_behavior
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/test_machine_status_polling_behavior.py"
                "${SUNSHINE_TEST_REPOSITORY_ROOT}" --compiler "${CMAKE_CXX_COMPILER}"
                --json-include "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>")
        set_tests_properties(test_component_machine_status_polling_behavior PROPERTIES LABELS "component")
        add_test(NAME test_component_linux_desktop_tray
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_desktop_tray.py")
        set_tests_properties(test_component_linux_desktop_tray PROPERTIES LABELS "component" TIMEOUT 60)
        add_test(NAME test_component_linux_machine_tray_metadata
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_machine_tray_metadata.py"
                "${CMAKE_CXX_COMPILER}"
                "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>")
        set_tests_properties(test_component_linux_machine_tray_metadata PROPERTIES LABELS "component")
        add_test(NAME test_component_linux_local_deploy
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_local_deploy.py")
        set_tests_properties(test_component_linux_local_deploy PROPERTIES LABELS "component" TIMEOUT 30)
        add_test(
            NAME test_component_linux_vibepollo_vkms_peercred
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${SUNSHINE_LIBVIRTUALDISPLAY_SOURCE_DIR}/tests/linux/test-vkms-peercred.py"
                "$<TARGET_FILE:vibeshine_vkms_peercred>")
        set_tests_properties(test_component_linux_vibepollo_vkms_peercred PROPERTIES LABELS "component")
        set(_sunshine_native_drm_source_root "${SUNSHINE_LIBVIRTUALDISPLAY_SOURCE_DIR}")
        if(NOT _sunshine_native_drm_source_root)
            set(_sunshine_native_drm_source_root "${SUNSHINE_TEST_REPOSITORY_ROOT}/third-party/libvirtualdisplay")
        endif()
        add_test(
            NAME test_component_linux_drm_native_allocation
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${_sunshine_native_drm_source_root}/tests/linux/test-native-dumb-allocation.py"
                "${_sunshine_native_drm_source_root}/linux/vibeshine-drm")
        set_tests_properties(test_component_linux_drm_native_allocation PROPERTIES LABELS "component" TIMEOUT 30)
        unset(_sunshine_native_drm_source_root)
        add_test(
            NAME test_component_linux_vibepollo_drm_contract
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${SUNSHINE_LIBVIRTUALDISPLAY_SOURCE_DIR}/tests/linux/test-vibeshine-drm-contract.py"
                "${LIBVIRTUALDISPLAY_LINUX_ROOT}/vibeshine-drm")
        set_tests_properties(test_component_linux_vibepollo_drm_contract PROPERTIES LABELS "component")
        add_test(
            NAME test_component_linux_kwin_gpu_bridge_contract
            COMMAND "${SUNSHINE_TEST_PYTHON_EXECUTABLE}"
                "${SUNSHINE_LIBVIRTUALDISPLAY_SOURCE_DIR}/tests/linux/test-kwin-gpu-bridge-contract.py"
                "${LIBVIRTUALDISPLAY_LINUX_ROOT}")
        set_tests_properties(test_component_linux_kwin_gpu_bridge_contract PROPERTIES LABELS "component")
        if(TARGET vibeshine_kwin_gpu)
            find_package(PkgConfig REQUIRED)
            pkg_check_modules(SUNSHINE_TEST_KWIN_GPU REQUIRED IMPORTED_TARGET libdrm gbm)
            add_executable(test_linux_kwin_gpu_routing
                "${SUNSHINE_LIBVIRTUALDISPLAY_SOURCE_DIR}/tests/linux/test-kwin-gpu-routing.cpp")
            target_link_libraries(test_linux_kwin_gpu_routing PRIVATE
                PkgConfig::SUNSHINE_TEST_KWIN_GPU ${CMAKE_DL_LIBS})
            add_test(NAME test_component_linux_kwin_gpu_routing COMMAND test_linux_kwin_gpu_routing)
            set_tests_properties(test_component_linux_kwin_gpu_routing PROPERTIES LABELS "component" TIMEOUT 30)
        endif()
    else()
        message(WARNING "python3 was not found; skipping the Vibepollo DRM source contract test")
    endif()
endif()

if(WIN32)
    sunshine_register_component(NAME test_component_playnite_protocol TEST_SOURCE test_playnite_protocol.cpp
        PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/playnite_protocol.cpp")
    foreach(_playnite_test IN ITEMS
            test_playnite_autosync_reconcile.cpp
            test_playnite_sync_autosync.cpp)
        get_filename_component(_playnite_name "${_playnite_test}" NAME_WE)
        sunshine_register_component(NAME "test_component_${_playnite_name}" TEST_SOURCE "${_playnite_test}"
            PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/playnite_sync_policy.cpp"
            LINK_LIBRARIES nlohmann_json::nlohmann_json)
    endforeach()
    sunshine_register_component(NAME test_component_test_playnite_sync TEST_SOURCE test_playnite_sync.cpp
        PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/playnite_sync_policy.cpp"
            LINK_LIBRARIES nlohmann_json::nlohmann_json)
    sunshine_register_component(NAME test_component_display_helper_watchdog TEST_SOURCE unit/test_display_helper_watchdog.cpp
        PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_watchdog.cpp")
    sunshine_register_component(NAME test_component_display_helper_session_deferral TEST_SOURCE unit/test_display_helper_session_deferral.cpp
        PRODUCT_SOURCES
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_request_policy.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_session_deferral.cpp"
        LINK_LIBRARIES sunshine_test_displaydevice_headers)
    sunshine_register_component(NAME test_component_startup_encoder_probe_policy TEST_SOURCE unit/test_startup_encoder_probe_policy.cpp)
    sunshine_register_component(NAME test_component_audio_visibility_recovery TEST_SOURCE unit/test_audio_visibility_recovery.cpp)
    sunshine_register_component(NAME test_component_display_helper_v2_async_dispatcher TEST_SOURCE unit/test_display_helper_v2_async_dispatcher.cpp
        PRODUCT_SOURCES
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/diagnostics.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/text_storage.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/topology_policy.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/async_dispatcher.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/golden_health.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/operations.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/snapshot.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/snapshot_codec.cpp"
        LINK_LIBRARIES sunshine_test_displaydevice_headers Boost::algorithm nlohmann_json::nlohmann_json)
    set_tests_properties(test_component_display_helper_v2_async_dispatcher PROPERTIES TIMEOUT 30)
    sunshine_register_component(NAME test_component_display_helper_v2_core TEST_SOURCE unit/test_display_helper_v2_core.cpp
        PRODUCT_SOURCES
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/diagnostics.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/text_storage.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/topology_policy.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/golden_health.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/operations.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/snapshot.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/snapshot_codec.cpp"
        LINK_LIBRARIES sunshine_test_displaydevice_headers Boost::algorithm nlohmann_json::nlohmann_json)
    sunshine_register_component(NAME test_component_display_helper_v2_restore_engine TEST_SOURCE unit/test_display_helper_v2_restore_engine.cpp
        PRODUCT_SOURCES
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/diagnostics.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/text_storage.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/topology_policy.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/golden_health.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/operations.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/snapshot.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/snapshot_codec.cpp"
        LINK_LIBRARIES sunshine_test_displaydevice_headers Boost::algorithm nlohmann_json::nlohmann_json)
    sunshine_register_component(NAME test_component_display_helper_v2_state_machine TEST_SOURCE unit/test_display_helper_v2_state_machine.cpp
        PRODUCT_SOURCES
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/diagnostics.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/text_storage.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/topology_policy.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/async_dispatcher.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/golden_health.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/operations.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/snapshot.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/snapshot_codec.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/state_machine.cpp"
        LINK_LIBRARIES sunshine_test_displaydevice_headers Boost::algorithm nlohmann_json::nlohmann_json)
    sunshine_register_component(NAME test_component_display_helper_v2_topology_transition TEST_SOURCE unit/test_display_helper_v2_topology_transition.cpp
        PRODUCT_SOURCES
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_request_policy.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/diagnostics.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/text_storage.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/topology_policy.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/golden_health.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/operations.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/snapshot.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/snapshot_codec.cpp"
        LINK_LIBRARIES sunshine_test_displaydevice_headers Boost::algorithm nlohmann_json::nlohmann_json)
    sunshine_register_component(NAME test_component_utf_utils TEST_SOURCE unit/platform/windows/test_utf_utils.cpp
        PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/utf_utils.cpp")
    sunshine_register_component(NAME test_component_game_activity TEST_SOURCE unit/platform/windows/test_game_activity.cpp
        PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/game_activity_policy.cpp")
    sunshine_register_component(NAME test_component_native_amf_policy TEST_SOURCE unit/platform/windows/test_native_amf_review.cpp)
    sunshine_register_component(NAME test_component_udp_send_retry TEST_SOURCE unit/platform/windows/test_udp_send_retry.cpp)
    sunshine_register_component(NAME test_component_rtx_hdr_policy TEST_SOURCE unit/platform/windows/test_rtx_hdr_runtime.cpp
        PRODUCT_SOURCES
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/rtx_hdr_policy.cpp")
    sunshine_register_component(NAME test_component_virtual_display_policy TEST_SOURCE unit/platform/windows/test_virtual_display_sunshine.cpp
        PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/virtual_display_identity.cpp")
    sunshine_register_component(NAME test_component_display_mode_resolution TEST_SOURCE unit/platform/windows/test_display_mode_resolution.cpp
        PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/third-party/libdisplaydevice/src/windows/display_mode_resolution.cpp"
        LINK_LIBRARIES sunshine_test_displaydevice_headers)
    sunshine_register_component(NAME test_component_vhf_gamepad_policy TEST_SOURCE unit/platform/windows/test_vhf_gamepad_policy.cpp
        PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/vhf_gamepad_policy.cpp"
        INCLUDE_DIRECTORIES "${SUNSHINE_LIBVIRTUALGAMEPAD_INCLUDE_DIR}"
        LINK_LIBRARIES sunshine_test_moonlight_headers)
    sunshine_register_component(NAME test_component_virtual_display_cleanup_policy TEST_SOURCE unit/test_virtual_display_cleanup_policy.cpp)
    sunshine_register_component(NAME test_component_lossless_scaling_policy TEST_SOURCE unit/test_lossless_scaling.cpp
        PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/tools/playnite_launcher/lossless_scaling_policy.cpp")
endif()

if(APPLE)
    # This file intentionally contains only portable C++ policy tests. Compile
    # it as C++ so this target does not enable or link Objective-C frameworks.
    set_source_files_properties(
        "${CMAKE_CURRENT_LIST_DIR}/unit/platform/macos/test_av_audio.mm"
        PROPERTIES LANGUAGE CXX)
    sunshine_register_component(NAME test_component_av_audio_policy TEST_SOURCE unit/platform/macos/test_av_audio.mm
        PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/macos/av_audio_policy.cpp")
endif()


# Production-code regression harnesses exercise Windows policies on the native host.
find_package(Python3 COMPONENTS Interpreter)
# Compile the actual notification method against the production C tray ABI.
if(SUNSHINE_TEST_ENABLE_COMPONENT AND Python3_Interpreter_FOUND AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_test(NAME test_component_update_notification_tray_interface
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/unit/test_update_notification_tray_interface.py"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${CMAKE_CXX_COMPILER}")
    set_tests_properties(test_component_update_notification_tray_interface PROPERTIES LABELS "component")
endif()
find_program(SUNSHINE_TEST_GXX NAMES g++)
if(Python3_Interpreter_FOUND AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME regression_cuda_release_policy
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/unit/test_cuda_release_policy.py")
endif()
if(Python3_Interpreter_FOUND AND SUNSHINE_TEST_GXX)
    add_test(NAME regression_windows_color_profile
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/unit/test_windows_color_profile.py")
    add_test(NAME regression_delayed_mouse_release
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/input/test_delayed_mouse_release.py")
    add_test(NAME regression_windows_absolute_mouse
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/input/test_windows_absolute_mouse.py")
    add_test(NAME regression_windows_mouse_keys
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/input/test_windows_mouse_keys.py" --compiler "${SUNSHINE_TEST_GXX}")
    add_test(NAME regression_frame_limiter_vsync
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/unit/platform/windows/test_frame_limiter_vsync.py")
    add_test(NAME regression_playnite_stop_callback
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/unit/platform/windows/test_playnite_stop_callback.py")
    add_test(NAME regression_playnite_close_grace
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/unit/platform/windows/test_playnite_close_grace.py")
    add_test(NAME regression_playnite_plugin_upgrade
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/unit/platform/windows/test_playnite_plugin_upgrade.py")
    find_program(SUNSHINE_TEST_DOTNET NAMES dotnet dotnet.exe)
    add_test(NAME regression_playnite_connector_lifecycle
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/playnite_connector/test_lifecycle.py"
            --dotnet "${SUNSHINE_TEST_DOTNET}")
    set_tests_properties(regression_playnite_connector_lifecycle regression_playnite_close_grace regression_playnite_plugin_upgrade
        PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME regression_rtss_profile_version
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/unit/platform/windows/test_rtss_profile_version.py")
    add_test(NAME regression_virtual_capture_readiness
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/unit/platform/windows/test_virtual_capture_readiness.py")
    add_test(NAME regression_virtual_display_scale_boundary
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/unit/platform/windows/test_virtual_display_scale_boundary.py")
    add_test(NAME regression_configured_adapter_boundary
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/unit/platform/windows/test_configured_adapter_boundary.py"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}" "${SUNSHINE_TEST_GXX}")
    add_test(NAME regression_cancel_after_app_exit
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/unit/test_cancel_after_app_exit.py")
    add_test(NAME regression_stream_disconnect_lifecycle
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/unit/test_stream_disconnect_lifecycle.py"
            --compiler "${SUNSHINE_TEST_GXX}")
    set_tests_properties(regression_stream_disconnect_lifecycle PROPERTIES LABELS "component" TIMEOUT 45)
    add_test(NAME regression_display_policy_adapter
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/unit/test_display_policy_adapter.py")
    add_library(sunshine_test_session_display_precedence_dependencies INTERFACE)
    target_link_libraries(sunshine_test_session_display_precedence_dependencies INTERFACE Boost::algorithm)
    add_test(NAME regression_session_display_precedence
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/unit/test_session_display_precedence.py"
            --compiler "${CMAKE_CXX_COMPILER}"
            --include-dir "$<TARGET_PROPERTY:sunshine_test_session_display_precedence_dependencies,INTERFACE_INCLUDE_DIRECTORIES>")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME regression_touch_mapping_environment
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/unit/platform/linux/test_touch_mapping_environment.py")
    endif()
    add_test(NAME regression_selected_output_fallback
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/unit/test_selected_output_fallback.py")
    add_test(NAME regression_virtual_display_refresh
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/unit/test_virtual_display_refresh.py")
    add_test(NAME regression_virtual_display_recovery_transport
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/unit/test_virtual_display_recovery_transport.py")
    add_test(NAME regression_virtual_display_recovery_modes
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/unit/test_virtual_display_recovery_modes.py")
endif()

if(TARGET test_component_resource_locale_catalog)
    set_tests_properties(test_component_resource_locale_catalog PROPERTIES
        WORKING_DIRECTORY "${SUNSHINE_TEST_REPOSITORY_ROOT}")
endif()
# Compile extracted production AMF submission and delivery without a GPU.
find_program(SUNSHINE_TEST_AMF_CXX NAMES g++)
if(Python3_Interpreter_FOUND AND SUNSHINE_TEST_AMF_CXX)
    add_test(NAME regression_amf_output_first_retry
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/unit/platform/windows/test_amf_output_first_retry.py"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}" --compiler "${SUNSHINE_TEST_AMF_CXX}")
endif()

find_program(SUNSHINE_TEST_POWERSHELL NAMES pwsh powershell)
if(SUNSHINE_TEST_POWERSHELL)
    add_test(NAME regression_windows_legacy_driver_trust_plan
        COMMAND "${SUNSHINE_TEST_POWERSHELL}" -NoLogo -NonInteractive -NoProfile
            -File "${SUNSHINE_TEST_REPOSITORY_ROOT}/packaging/windows/test_legacy_driver_trust_plan.ps1"
            -RepositoryRoot "${SUNSHINE_TEST_REPOSITORY_ROOT}")
    set_tests_properties(regression_windows_legacy_driver_trust_plan PROPERTIES LABELS "component" TIMEOUT 120)
    add_test(NAME regression_windows_display_reboot_state
        COMMAND "${SUNSHINE_TEST_POWERSHELL}" -NoLogo -NonInteractive -NoProfile
            -File "${SUNSHINE_TEST_REPOSITORY_ROOT}/packaging/windows/test_display_reboot_state.ps1"
            -RepositoryRoot "${SUNSHINE_TEST_REPOSITORY_ROOT}")
    set_tests_properties(regression_windows_display_reboot_state PROPERTIES LABELS "component" TIMEOUT 120)
    add_test(NAME regression_windows_usbip_package
        COMMAND "${SUNSHINE_TEST_POWERSHELL}" -NoLogo -NonInteractive -NoProfile
            -File "${SUNSHINE_TEST_REPOSITORY_ROOT}/packaging/windows/usbip_transport/tests/test_package_contract.ps1")
    set_tests_properties(regression_windows_usbip_package PROPERTIES LABELS "component")
endif()

sunshine_register_component(NAME test_component_dualsense_audio_pcm TEST_SOURCE unit/test_dualsense_audio_pcm.cpp)
sunshine_register_component(NAME test_component_dualsense_launch_policy TEST_SOURCE unit/test_dualsense_launch_policy.cpp)
set(_dualsense_protocol_test_adapter "")
if(NOT WIN32)
    # Only the public GUID/IOCTL declarations are needed for the portable Sony
    # encoder. The production Windows targets use real Windows SDK headers.
    set(_dualsense_protocol_test_adapter "${CMAKE_CURRENT_LIST_DIR}/support/virtual_gamepad_protocol")
endif()
sunshine_register_component(NAME test_component_dualsense_usbip_gamepad_policy TEST_SOURCE unit/platform/windows/test_dualsense_usbip_gamepad_policy.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/third-party/libvirtualgamepad/driver/src/dualsense.cpp"
    INCLUDE_DIRECTORIES ${_dualsense_protocol_test_adapter}
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/third-party/libvirtualgamepad/include")
sunshine_register_component(NAME test_component_dualsense_usbip_gamepad TEST_SOURCE unit/platform/windows/test_dualsense_usbip_gamepad.cpp
    PRODUCT_SOURCES
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/dualsense_usbip_gamepad.cpp"
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/vhf_gamepad_policy.cpp"
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/third-party/libvirtualgamepad/driver/src/dualsense.cpp"
    INCLUDE_DIRECTORIES ${_dualsense_protocol_test_adapter}
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/third-party/libvirtualgamepad/include"
    LINK_LIBRARIES nlohmann_json::nlohmann_json Boost::log Boost::process Boost::format sunshine_test_moonlight_headers Threads::Threads)
if(TARGET test_component_dualsense_usbip_gamepad)
    set_tests_properties(test_component_dualsense_usbip_gamepad PROPERTIES TIMEOUT 30)
endif()
unset(_dualsense_protocol_test_adapter)
sunshine_add_test_target(NAME test_component_dualsense_usbip_protocol CATEGORY component
    TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/unit/platform/windows/test_dualsense_usbip_protocol.cpp"
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/dualsense_usbip_protocol.cpp"
    INCLUDE_DIRECTORIES "${SUNSHINE_TEST_REPOSITORY_ROOT}/third-party/libvirtualgamepad/include")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET libinputtino)
    add_executable(test_ds5_haptics unit/platform/linux/test_ds5_haptics.cpp)
    target_include_directories(test_ds5_haptics PRIVATE
        "${SUNSHINE_TEST_REPOSITORY_ROOT}"
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/third-party/moonlight-common-c/src")
    add_test(NAME test_ds5_haptics COMMAND test_ds5_haptics)
    foreach(profile ds4 ds5)
        add_executable(test_${profile}_usb unit/platform/linux/test_${profile}_usb.cpp)
        target_include_directories(test_${profile}_usb PRIVATE "${SUNSHINE_TEST_REPOSITORY_ROOT}/third-party/inputtino/include")
        add_test(NAME test_component_${profile}_usb COMMAND test_${profile}_usb)
    endforeach()
    # Manual only: creates temporary devices visible to the desktop and Steam.
    add_executable(probe_playstation_uhid integration/probe_playstation_uhid.cpp
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/linux/input/inputtino_ds4.cpp")
    target_include_directories(probe_playstation_uhid PRIVATE "${SUNSHINE_TEST_REPOSITORY_ROOT}")
    target_link_libraries(probe_playstation_uhid PRIVATE inputtino::libinputtino)
    add_executable(probe_ds5_gadget_lifecycle integration/probe_ds5_gadget_lifecycle.cpp)
    target_include_directories(probe_ds5_gadget_lifecycle PRIVATE "${SUNSHINE_TEST_REPOSITORY_ROOT}")
    target_link_libraries(probe_ds5_gadget_lifecycle PRIVATE inputtino::libinputtino)
    find_package(ALSA QUIET)
    if(ALSA_FOUND)
        add_executable(probe_ds5_haptics integration/probe_ds5_haptics.cpp)
        target_include_directories(probe_ds5_haptics PRIVATE ${ALSA_INCLUDE_DIRS})
        target_link_libraries(probe_ds5_haptics PRIVATE inputtino::libinputtino ${ALSA_LIBRARIES})
    endif()
endif()

if(WIN32 AND TARGET pyrowave::capi)
    add_executable(test_pyrowave_windows_gpu test_pyrowave_windows_gpu.cpp
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/pyrowave_d3d11_core.cpp"
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/pyrowave_policy.cpp")
    target_include_directories(test_pyrowave_windows_gpu PRIVATE "${SUNSHINE_TEST_REPOSITORY_ROOT}")
    target_compile_features(test_pyrowave_windows_gpu PRIVATE cxx_std_20)
    target_compile_definitions(test_pyrowave_windows_gpu PRIVATE
        "PYROWAVE_TEST_SHADER_PATH=L\"${SUNSHINE_TEST_REPOSITORY_ROOT}/src_assets/windows/assets/shaders/directx/convert_pyrowave_cs.hlsl\"")
    target_link_libraries(test_pyrowave_windows_gpu PRIVATE pyrowave::capi d3d11 dxgi d3dcompiler)
    if(SUNSHINE_EXPERIMENTAL_PYROWAVE_D3D12)
        target_sources(test_pyrowave_windows_gpu PRIVATE
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/pyrowave_d3d12_conversion.cpp")
        target_compile_definitions(test_pyrowave_windows_gpu PRIVATE SUNSHINE_EXPERIMENTAL_PYROWAVE_D3D12=1)
        target_link_libraries(test_pyrowave_windows_gpu PRIVATE d3d12)
        add_test(NAME pyrowave_windows_d3d12_gpu COMMAND test_pyrowave_windows_gpu 0 d3d12)
        set_tests_properties(pyrowave_windows_d3d12_gpu PROPERTIES SKIP_RETURN_CODE 77 LABELS "integration;gpu;experimental")
    endif()
    add_test(NAME pyrowave_windows_gpu COMMAND test_pyrowave_windows_gpu)
    set_tests_properties(pyrowave_windows_gpu PROPERTIES SKIP_RETURN_CODE 77 LABELS "integration;gpu")
endif()

if(TARGET pyrowave-linux)
    add_executable(test_pyrowave_linux_gpu test_pyrowave_linux_gpu.cpp)
    target_include_directories(test_pyrowave_linux_gpu PRIVATE "${SUNSHINE_TEST_REPOSITORY_ROOT}")
    target_link_libraries(test_pyrowave_linux_gpu PRIVATE pyrowave-linux pyrowave::capi gbm vulkan)
    add_test(NAME pyrowave_linux_gpu COMMAND test_pyrowave_linux_gpu)
    set_tests_properties(pyrowave_linux_gpu PROPERTIES SKIP_RETURN_CODE 77 LABELS "integration;gpu")
endif()

# Compiles the production bootstrapper and tests migration using temporary data.
# Never invokes MSI, services, driver installers, or certificate store mutations.
if(WIN32)
    set(_bootstrap_migration_shell "$ENV{WINDIR}/System32/WindowsPowerShell/v1.0/powershell.exe")
    if(EXISTS "${_bootstrap_migration_shell}")
        add_test(NAME test_script_bootstrap_migration
            COMMAND "${_bootstrap_migration_shell}" -NoLogo -NoProfile -ExecutionPolicy Bypass
                -File "${SUNSHINE_TEST_REPOSITORY_ROOT}/packaging/windows/bootstrapper/tests/test_bootstrap_migration.ps1"
                -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/bootstrap-migration")
        set_tests_properties(test_script_bootstrap_migration PROPERTIES LABELS "installer;component")
    endif()
endif()

# Managed focus has native effect ports; execute its production portable logic.
if(Python3_Interpreter_FOUND AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(SUNSHINE_TEST_FOCUS_NODE NAMES node nodejs)
    if(SUNSHINE_TEST_FOCUS_NODE)
        add_test(NAME regression_managed_app_focus_adapters
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/unit/test_managed_app_focus_adapters.py"
                --compiler "${CMAKE_CXX_COMPILER}" --node "${SUNSHINE_TEST_FOCUS_NODE}")
        set_tests_properties(regression_managed_app_focus_adapters PROPERTIES LABELS "component" TIMEOUT 60)
    endif()
endif()

include("${CMAKE_CURRENT_LIST_DIR}/cmake/WindowsPortablePackagingTest.cmake")
