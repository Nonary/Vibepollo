cmake_minimum_required(VERSION 3.20)

# This same target is included by the application test suite and can be built
# independently without unrelated capture, driver or web dependencies.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(ProcessCatalogTests LANGUAGES CXX)
    enable_testing()
endif()
find_package(Threads REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS process)
if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json CONFIG REQUIRED)
endif()

set(_catalog_repository_root "${CMAKE_CURRENT_LIST_DIR}/../..")
add_executable(test_component_process_refresh
    "${CMAKE_CURRENT_LIST_DIR}/../unit/test_process_catalog.cpp"
    "${_catalog_repository_root}/src/process_catalog.cpp")
target_compile_features(test_component_process_refresh PRIVATE cxx_std_23)
target_include_directories(test_component_process_refresh PRIVATE "${_catalog_repository_root}")
target_link_libraries(test_component_process_refresh PRIVATE Boost::process Threads::Threads nlohmann_json::nlohmann_json)
add_test(NAME test_component_process_refresh COMMAND test_component_process_refresh)
set_tests_properties(test_component_process_refresh PROPERTIES LABELS "component" TIMEOUT 45)
unset(_catalog_repository_root)

# The fixture extracts production read/recovery and migration control flow and
# links the real catalog publisher. Per-app conversion and I/O are test seams.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    set(_catalog_failure_fixture "${CMAKE_CURRENT_BINARY_DIR}/app-catalog-failure.cpp")
    add_custom_command(
        OUTPUT "${_catalog_failure_fixture}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../unit/test_app_catalog_failure.py"
            "${CMAKE_CURRENT_LIST_DIR}/../.." --output "${_catalog_failure_fixture}"
        DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../unit/test_app_catalog_failure.py"
            "${CMAKE_CURRENT_LIST_DIR}/../unit/test_app_catalog_failure.cpp.in"
            "${CMAKE_CURRENT_LIST_DIR}/../../src/process.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/../../src/platform/windows/virtual_display.h"
            "${CMAKE_CURRENT_LIST_DIR}/../../src/builtin_virtual_display.h"
        VERBATIM)
    add_executable(test_component_app_catalog_failure
        "${_catalog_failure_fixture}" "${CMAKE_CURRENT_LIST_DIR}/../../src/process_catalog.cpp")
    target_compile_features(test_component_app_catalog_failure PRIVATE cxx_std_23)
    target_include_directories(test_component_app_catalog_failure PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../..")
    target_link_libraries(test_component_app_catalog_failure PRIVATE Boost::process Threads::Threads nlohmann_json::nlohmann_json)
    add_test(NAME test_component_app_catalog_failure
        COMMAND test_component_app_catalog_failure "${CMAKE_CURRENT_BINARY_DIR}/app-catalog-fixture")
    set_tests_properties(test_component_app_catalog_failure PROPERTIES LABELS "component" TIMEOUT 45)
    if(NOT WIN32)
        # Compile and execute the extracted Windows built-in branch without
        # defining _WIN32 for unrelated platform headers.
        set(_catalog_windows_fixture "${CMAKE_CURRENT_BINARY_DIR}/app-catalog-windows.cpp")
        add_custom_command(
            OUTPUT "${_catalog_windows_fixture}"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../unit/test_app_catalog_failure.py"
                "${CMAKE_CURRENT_LIST_DIR}/../.." --output "${_catalog_windows_fixture}"
            DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../unit/test_app_catalog_failure.py"
                "${CMAKE_CURRENT_LIST_DIR}/../unit/test_app_catalog_failure.cpp.in"
                "${CMAKE_CURRENT_LIST_DIR}/../../src/process.cpp"
                "${CMAKE_CURRENT_LIST_DIR}/../../src/platform/windows/virtual_display.h"
                "${CMAKE_CURRENT_LIST_DIR}/../../src/builtin_virtual_display.h"
            VERBATIM)
        add_executable(test_component_app_catalog_windows_branch
            "${_catalog_windows_fixture}" "${CMAKE_CURRENT_LIST_DIR}/../../src/process_catalog.cpp")
        target_compile_features(test_component_app_catalog_windows_branch PRIVATE cxx_std_23)
        target_compile_definitions(test_component_app_catalog_windows_branch PRIVATE SUNSHINE_TEST_WINDOWS_BUILTIN=1)
        target_include_directories(test_component_app_catalog_windows_branch PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../..")
        target_link_libraries(test_component_app_catalog_windows_branch PRIVATE Boost::process Threads::Threads nlohmann_json::nlohmann_json)
        add_test(NAME test_component_app_catalog_windows_branch
            COMMAND test_component_app_catalog_windows_branch "${CMAKE_CURRENT_BINARY_DIR}/app-catalog-fixture-windows")
        set_tests_properties(test_component_app_catalog_windows_branch PROPERTIES LABELS "component" TIMEOUT 45)
    endif()
endif()
