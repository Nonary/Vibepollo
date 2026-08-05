cmake_minimum_required(VERSION 3.20)

# This is deliberately a standalone entry point. Normal Sunshine configuration
# continues to force BUILD_TESTS=OFF and does not add this directory.
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

set(GTEST_SOURCE_DIR "${SUNSHINE_TEST_REPOSITORY_ROOT}/third-party/googletest")
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
    LINK_LIBRARIES GTest::gmock GTest::gtest
    DEPENDENCIES
)
if(TARGET test_fast_nvenc_api)
    # nvenc_api.h directly includes the Video Codec SDK public header.
    target_include_directories(test_fast_nvenc_api PRIVATE
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/third-party/nv-codec-headers/include"
    )
endif()

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
    LINK_LIBRARIES GTest::gtest
    DEPENDENCIES
)
