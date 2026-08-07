cmake_minimum_required(VERSION 3.20)

# This directory is also usable as a standalone entry point for focused test
# work, and is included by the normal project build when BUILD_TESTS is enabled.
project(VibeshineTests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(SUNSHINE_TEST_REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

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
find_package(Boost CONFIG REQUIRED COMPONENTS log log_setup)
find_package(OpenSSL REQUIRED)

# Header-only SDK boundaries are named targets rather than process-wide include
# paths.  A component explicitly opts in to the one external surface it uses;
# this keeps edits in a vendored SDK from rebuilding unrelated test targets.
add_library(sunshine_test_simple_web_headers INTERFACE)
target_include_directories(sunshine_test_simple_web_headers SYSTEM INTERFACE
    "${SUNSHINE_TEST_REPOSITORY_ROOT}/third-party")
target_link_libraries(sunshine_test_simple_web_headers INTERFACE
    OpenSSL::SSL OpenSSL::Crypto)

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
    LINK_LIBRARIES GTest::gtest
    DEPENDENCIES
)

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

sunshine_register_component(NAME test_component_stream_protocol TEST_SOURCE unit/test_stream.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/stream_protocol.cpp")
sunshine_register_component(NAME test_component_config_parse TEST_SOURCE unit/test_config_parse.cpp
    PRODUCT_SOURCES)
sunshine_register_component(NAME test_component_config_playnite TEST_SOURCE test_config_playnite.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/config_playnite_parse.cpp")
sunshine_register_component(NAME test_component_file_handler TEST_SOURCE unit/test_file_handler.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/file_handler_core.cpp")
sunshine_register_component(NAME test_component_state_storage TEST_SOURCE unit/test_state_storage.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/state_storage_policy.cpp")
sunshine_register_component(NAME test_component_http_auth TEST_SOURCE unit/test_http_auth.cpp
    EXTRA_TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/unit/test_confighttp.cpp"
    PRODUCT_SOURCES
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/http_auth_policy.cpp"
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/http_auth_request_policy.cpp"
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/config_http_policy.cpp"
    LINK_LIBRARIES GTest::gmock sunshine_test_simple_web_headers ws2_32)
sunshine_register_component(NAME test_component_http_policy TEST_SOURCE unit/test_httpcommon.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/http_policy.cpp")
sunshine_register_component(NAME test_component_network_policy TEST_SOURCE unit/test_network.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/network_policy.cpp"
    LINK_LIBRARIES ws2_32)
sunshine_register_component(NAME test_component_input_validation TEST_SOURCE unit/test_input.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/input_validation_policy.cpp"
    LINK_LIBRARIES sunshine_test_moonlight_headers)
sunshine_register_component(NAME test_component_display_device_policy TEST_SOURCE unit/test_display_device.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/display_device_policy.cpp")
sunshine_register_component(NAME test_component_process_policy TEST_SOURCE unit/test_process.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/app_catalog_policy.cpp")
sunshine_register_component(NAME test_component_process_framegen_json TEST_SOURCE unit/test_process_framegen_json.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/app_framegen_config.cpp")
sunshine_register_component(NAME test_component_session_history TEST_SOURCE unit/test_session_history.cpp
    PRODUCT_SOURCES
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/session_history_storage.cpp"
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/session_history_storage_diagnostics.cpp"
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/session_history_policy.cpp"
    LINK_LIBRARIES PkgConfig::SQLITE3)
sunshine_register_component(NAME test_component_host_stats TEST_SOURCE unit/test_host_stats.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/host_stats_service.cpp")
sunshine_register_component(NAME test_component_audio_policy TEST_SOURCE unit/test_audio.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/audio_policy.cpp")
sunshine_register_component(NAME test_component_mouse_policy TEST_SOURCE unit/test_mouse.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/mouse_input.cpp")
sunshine_register_component(NAME test_component_platform_common TEST_SOURCE unit/platform/test_common.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/common_services.cpp")
sunshine_register_component(NAME test_component_logging_policy TEST_SOURCE unit/test_logging.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/logging_policy.cpp")
sunshine_register_component(NAME test_component_entry_metadata TEST_SOURCE unit/test_entry_handler.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/entry_metadata.cpp")
sunshine_register_component(NAME test_component_config_http_policy TEST_SOURCE unit/test_config_http_policy.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/config_http_policy.cpp"
    LINK_LIBRARIES ws2_32)
sunshine_register_component(NAME test_component_http_pairing_policy TEST_SOURCE unit/test_http_pairing.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/http_pairing_policy.cpp")
sunshine_register_component(NAME test_component_video_policy TEST_SOURCE unit/test_video.cpp
    PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/video_policy.cpp")
sunshine_register_component(NAME test_component_encoder_probe_policy TEST_SOURCE unit/test_encoder_probe_policy.cpp)
sunshine_register_component(NAME test_component_resource_config_catalog TEST_SOURCE unit/test_config_catalog_contract.cpp
    INCLUDE_DIRECTORIES "${SUNSHINE_TEST_GENERATED_INCLUDE_DIR}")
sunshine_register_component(NAME test_component_resource_locale_catalog TEST_SOURCE integration/test_locale_consistency.cpp
    SUPPORT_SOURCES "${CMAKE_CURRENT_LIST_DIR}/support/file_handler_read_only.cpp")
sunshine_register_component(NAME test_component_external_command_policy TEST_SOURCE integration/test_external_commands.cpp)
sunshine_register_component(NAME test_component_virtual_display_manifest TEST_SOURCE integration/test_virtual_display_packaging.cpp
    INCLUDE_DIRECTORIES "${SUNSHINE_TEST_GENERATED_INCLUDE_DIR}")

if(WIN32)
    sunshine_register_component(NAME test_component_playnite_protocol TEST_SOURCE test_playnite_protocol.cpp
        PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/playnite_protocol.cpp")
    foreach(_playnite_test IN ITEMS
            test_playnite_autosync_reconcile.cpp
            test_playnite_sync_autosync.cpp)
        get_filename_component(_playnite_name "${_playnite_test}" NAME_WE)
        sunshine_register_component(NAME "test_component_${_playnite_name}" TEST_SOURCE "${_playnite_test}"
            PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/playnite_sync_policy.cpp")
    endforeach()
    sunshine_register_component(NAME test_component_test_playnite_sync TEST_SOURCE test_playnite_sync.cpp
        PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/playnite_sync_policy.cpp")
    sunshine_register_component(NAME test_component_display_helper_watchdog TEST_SOURCE unit/test_display_helper_watchdog.cpp
        PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_watchdog.cpp")
    sunshine_register_component(NAME test_component_display_helper_session_deferral TEST_SOURCE unit/test_display_helper_session_deferral.cpp
        PRODUCT_SOURCES
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_request_policy.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_session_deferral.cpp"
        LINK_LIBRARIES sunshine_test_displaydevice_headers)
    sunshine_register_component(NAME test_component_startup_display_policy TEST_SOURCE unit/test_startup_display_policy.cpp)
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
        LINK_LIBRARIES sunshine_test_displaydevice_headers)
    sunshine_register_component(NAME test_component_display_helper_v2_core TEST_SOURCE unit/test_display_helper_v2_core.cpp
        PRODUCT_SOURCES
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/diagnostics.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/text_storage.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/topology_policy.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/golden_health.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/operations.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/snapshot.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/snapshot_codec.cpp"
        LINK_LIBRARIES sunshine_test_displaydevice_headers)
    sunshine_register_component(NAME test_component_display_helper_v2_restore_engine TEST_SOURCE unit/test_display_helper_v2_restore_engine.cpp
        PRODUCT_SOURCES
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/diagnostics.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/text_storage.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/topology_policy.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/golden_health.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/operations.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/snapshot.cpp"
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/display_helper_v2/snapshot_codec.cpp"
        LINK_LIBRARIES sunshine_test_displaydevice_headers)
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
        LINK_LIBRARIES sunshine_test_displaydevice_headers)
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
        LINK_LIBRARIES sunshine_test_displaydevice_headers)
    sunshine_register_component(NAME test_component_utf_utils TEST_SOURCE unit/platform/windows/test_utf_utils.cpp
        PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/utf_utils.cpp")
    sunshine_register_component(NAME test_component_game_activity TEST_SOURCE unit/platform/windows/test_game_activity.cpp
        PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/game_activity_policy.cpp")
    sunshine_register_component(NAME test_component_native_amf_policy TEST_SOURCE unit/platform/windows/test_native_amf_review.cpp)
    sunshine_register_component(NAME test_component_rtx_hdr_policy TEST_SOURCE unit/platform/windows/test_rtx_hdr_runtime.cpp
        PRODUCT_SOURCES
            "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/rtx_hdr_policy.cpp")
    sunshine_register_component(NAME test_component_virtual_display_policy TEST_SOURCE unit/platform/windows/test_virtual_display_sunshine.cpp
        PRODUCT_SOURCES "${SUNSHINE_TEST_REPOSITORY_ROOT}/src/platform/windows/virtual_display_identity.cpp"
        DEFINITIONS "SUNSHINE_SOURCE_DIR=\"${SUNSHINE_TEST_REPOSITORY_ROOT}\"")
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
