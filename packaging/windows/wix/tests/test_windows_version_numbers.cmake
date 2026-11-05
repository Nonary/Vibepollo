cmake_minimum_required(VERSION 3.20)

if(WIX_CHILD)
    set(CMAKE_SOURCE_DIR "${SOURCE_ROOT}")
    set(PROJECT_VERSION_NUMERIC "${TEST_VERSION}")
    set(PROJECT_VERSION_FULL "${TEST_VERSION_FULL}")
    set(PROJECT_BUILD_CHANNEL stable)
    include("${SOURCE_ROOT}/cmake/packaging/windows_wix.cmake")
    message(STATUS "OBSERVED_PRODUCT_VERSION=${CPACK_WIX_PRODUCT_VERSION}")
    return()
endif()

if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()
find_program(GIT_EXECUTABLE git REQUIRED)

function(_run_wix_case version full expected_status expected_product expected_error)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE_ROOT=${SOURCE_ROOT}"
            -DWIX_CHILD=ON
            "-DTEST_VERSION=${version}"
            "-DTEST_VERSION_FULL=${full}"
            -P "${CMAKE_CURRENT_LIST_FILE}"
        WORKING_DIRECTORY "${SOURCE_ROOT}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr)
    set(_all_output "${_stdout}\n${_stderr}")
    if(expected_status STREQUAL "success")
        if(NOT _result EQUAL 0 OR NOT _all_output MATCHES "OBSERVED_PRODUCT_VERSION=${expected_product}")
            message(FATAL_ERROR "WiX version case ${version} (${full}) failed: ${_all_output}")
        endif()
    else()
        if(_result EQUAL 0 OR NOT _all_output MATCHES "${expected_error}")
            message(FATAL_ERROR "WiX version case ${version} did not fail closed: ${_all_output}")
        endif()
    endif()
endfunction()

# Keep the stable upper boundary, then prove the next patch is rejected rather
# than clamped or allowed to collide with another ProductVersion.
_run_wix_case("1.18.654" "1.18.654" success "1.18.65499.0" "")
_run_wix_case("1.18.655" "1.18.655" failure "" "max 654")
_run_wix_case("255.18.654" "255.18.654" success "255.18.65499.0" "")
_run_wix_case("256.18.0" "256.18.0" failure "" "maximum 255")
_run_wix_case("1.255.654" "1.255.654" success "1.255.65499.0" "")
_run_wix_case("1.256.0" "1.256.0" failure "" "maximum 255")

# Exercise a nightly after a numeric minor bump where no stable 1.18.x tag
# exists. The latest reachable stable tag below 1.18.0 must anchor the series.
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef _nonce)
set(_fixture "/tmp/windows-versioninfo-fixture-${_nonce}")
file(MAKE_DIRECTORY "${_fixture}")
file(WRITE "${_fixture}/marker.txt" "fixture\n")
execute_process(COMMAND "${GIT_EXECUTABLE}" init -q "${_fixture}" RESULT_VARIABLE _git_result)
if(NOT _git_result EQUAL 0)
    message(FATAL_ERROR "Could not initialize the versioninfo git fixture")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${_fixture}" config user.name "Version Test")
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${_fixture}" config user.email "version-test@example.invalid")
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${_fixture}" add marker.txt)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "GIT_AUTHOR_DATE=2024-01-01T00:00:00Z"
        "GIT_COMMITTER_DATE=2024-01-01T00:00:00Z"
        "${GIT_EXECUTABLE}" -C "${_fixture}" commit -q -m fixture
    RESULT_VARIABLE _git_result)
if(NOT _git_result EQUAL 0)
    message(FATAL_ERROR "Could not create the versioninfo git fixture commit")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "GIT_COMMITTER_DATE=2025-01-01T00:00:00Z"
        "${GIT_EXECUTABLE}" -C "${_fixture}" tag -a v1.17.9 -m stable
    RESULT_VARIABLE _git_result)
if(NOT _git_result EQUAL 0)
    message(FATAL_ERROR "Could not create the versioninfo stable fixture tag")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "GIT_COMMITTER_DATE=2025-05-01T00:00:00Z"
        "${GIT_EXECUTABLE}" -C "${_fixture}" tag -a 1.18.0 -m nightly
    RESULT_VARIABLE _git_result)
if(NOT _git_result EQUAL 0)
    message(FATAL_ERROR "Could not create the numeric nightly fixture tag")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "GIT_COMMITTER_DATE=2024-01-02T00:00:00Z"
        "${GIT_EXECUTABLE}" -C "${_fixture}" tag -a v1.17.9-stable.1 -m stable-respin
    RESULT_VARIABLE _git_result)
if(NOT _git_result EQUAL 0)
    message(FATAL_ERROR "Could not create the stable respin fixture tag")
endif()
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_fixture}" for-each-ref refs/tags/v1.17.9 "--format=%(creatordate:unix)"
    OUTPUT_VARIABLE _stable_anchor
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _git_result)
if(NOT _git_result EQUAL 0 OR NOT _stable_anchor MATCHES "^[0-9]+$")
    message(FATAL_ERROR "Could not read the versioninfo stable fixture tag time: result=${_git_result}, value=${_stable_anchor}")
endif()
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_fixture}" rev-parse "v1.17.9^{commit}"
    OUTPUT_VARIABLE _stable_commit
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _git_result)
if(NOT _git_result EQUAL 0 OR NOT _stable_commit MATCHES "^[0-9A-Fa-f]+$")
    message(FATAL_ERROR "Could not read the versioninfo stable fixture commit")
endif()
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_fixture}" for-each-ref refs/tags/v1.17.9-stable.1 "--format=%(creatordate:unix)"
    OUTPUT_VARIABLE _respin_anchor
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _git_result)
if(NOT _git_result EQUAL 0 OR NOT _respin_anchor MATCHES "^[0-9]+$")
    message(FATAL_ERROR "Could not read the versioninfo stable respin fixture tag time")
endif()
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_fixture}" rev-parse "v1.17.9-stable.1^{commit}"
    OUTPUT_VARIABLE _respin_commit
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _git_result)
if(NOT _git_result EQUAL 0 OR NOT _respin_commit MATCHES "^[0-9A-Fa-f]+$")
    message(FATAL_ERROR "Could not read the versioninfo stable respin fixture commit")
endif()
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_fixture}" for-each-ref refs/tags/1.18.0 "--format=%(creatordate:unix)"
    OUTPUT_VARIABLE _reservation_anchor
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _git_result)
if(NOT _git_result EQUAL 0 OR NOT _reservation_anchor MATCHES "^[0-9]+$")
    message(FATAL_ERROR "Could not read the versioninfo numeric reservation tag time")
endif()

set(_generated_header "${_fixture}/generated/versioninfo.h")
set(_generated_cache "${_fixture}/generated/versioninfo.cache")
math(EXPR _source_time "${_reservation_anchor} + 120")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DOUTPUT_FILE=${_generated_header}"
        "-DCACHE_FILE=${_generated_cache}"
        "-DSOURCE_DIR=${_fixture}"
        -DPROJECT_VERSION_MAJOR=1
        -DPROJECT_VERSION_MINOR=18
        -DPROJECT_VERSION_PATCH=0
        -DPROJECT_VERSION_FULL=1.18.0
        -DPROJECT_BUILD_CHANNEL=nightly
        -DPROJECT_STABLE_ANCHOR_TAG=v1.17.9
        "-DPROJECT_STABLE_ANCHOR_COMMIT=${_stable_commit}"
        "-DTEST_SOURCE_TIME_EPOCH=${_source_time}"
        -P "${SOURCE_ROOT}/cmake/prep/emit_windows_versioninfo.cmake"
    RESULT_VARIABLE _emit_result
    OUTPUT_VARIABLE _emit_stdout
    ERROR_VARIABLE _emit_stderr)
if(NOT _emit_result EQUAL 0)
    message(FATAL_ERROR "Versioninfo generation failed for the minor-bumped nightly: ${_emit_stdout}\n${_emit_stderr}")
endif()
file(READ "${_generated_cache}" _cache_text)
file(READ "${_generated_header}" _header_text)
if(NOT _cache_text MATCHES "anchor_time=${_reservation_anchor}" OR
   NOT _header_text MATCHES "#define RC_VERSION_MINOR 18" OR
   NOT _header_text MATCHES "#define RC_VERSION_REVISION 2")
    message(FATAL_ERROR "Versioninfo did not use the latest reachable prior stable anchor: ${_cache_text}\n${_header_text}")
endif()

set(_respin_header "${_fixture}/generated/respin-versioninfo.h")
set(_respin_cache "${_fixture}/generated/respin-versioninfo.cache")
math(EXPR _respin_source_time "${_reservation_anchor} + 120")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DOUTPUT_FILE=${_respin_header}"
        "-DCACHE_FILE=${_respin_cache}"
        "-DSOURCE_DIR=${_fixture}"
        -DPROJECT_VERSION_MAJOR=1
        -DPROJECT_VERSION_MINOR=18
        -DPROJECT_VERSION_PATCH=0
        -DPROJECT_VERSION_FULL=1.18.0
        -DPROJECT_BUILD_CHANNEL=nightly
        -DPROJECT_STABLE_ANCHOR_TAG=v1.17.9-stable.1
        "-DPROJECT_STABLE_ANCHOR_COMMIT=${_respin_commit}"
        "-DTEST_SOURCE_TIME_EPOCH=${_respin_source_time}"
        -P "${SOURCE_ROOT}/cmake/prep/emit_windows_versioninfo.cmake"
    RESULT_VARIABLE _emit_result
    OUTPUT_VARIABLE _emit_stdout
    ERROR_VARIABLE _emit_stderr)
if(NOT _emit_result EQUAL 0)
    message(FATAL_ERROR "Versioninfo generation failed for the stable-respin anchor: ${_emit_stdout}\n${_emit_stderr}")
endif()
file(READ "${_respin_cache}" _respin_cache_text)
if(NOT _respin_cache_text MATCHES "anchor_time=${_reservation_anchor}")
    message(FATAL_ERROR "Versioninfo did not honor the numeric reservation anchor: ${_respin_cache_text}")
endif()

# Retrying the same reserved version at the same source time is deterministic.
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DOUTPUT_FILE=${_generated_header}"
        "-DCACHE_FILE=${_generated_cache}"
        "-DSOURCE_DIR=${_fixture}"
        -DPROJECT_VERSION_MAJOR=1
        -DPROJECT_VERSION_MINOR=18
        -DPROJECT_VERSION_PATCH=0
        -DPROJECT_VERSION_FULL=1.18.0
        -DPROJECT_BUILD_CHANNEL=nightly
        -DPROJECT_STABLE_ANCHOR_TAG=v1.17.9
        "-DPROJECT_STABLE_ANCHOR_COMMIT=${_stable_commit}"
        "-DTEST_SOURCE_TIME_EPOCH=${_source_time}"
        -P "${SOURCE_ROOT}/cmake/prep/emit_windows_versioninfo.cmake"
    RESULT_VARIABLE _retry_result
    OUTPUT_VARIABLE _retry_stdout
    ERROR_VARIABLE _retry_stderr)
if(NOT _retry_result EQUAL 0)
    message(FATAL_ERROR "Retrying a numeric reservation failed: ${_retry_stdout}\n${_retry_stderr}")
endif()

# A missing immutable numeric reservation tag fails closed.
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DOUTPUT_FILE=${_fixture}/generated/missing-versioninfo.h"
        "-DSOURCE_DIR=${_fixture}"
        -DPROJECT_VERSION_MAJOR=1
        -DPROJECT_VERSION_MINOR=18
        -DPROJECT_VERSION_PATCH=1
        -DPROJECT_VERSION_FULL=1.18.1
        -DPROJECT_BUILD_CHANNEL=nightly
        -DPROJECT_STABLE_ANCHOR_TAG=v1.17.9
        "-DPROJECT_STABLE_ANCHOR_COMMIT=${_stable_commit}"
        "-DTEST_SOURCE_TIME_EPOCH=${_source_time}"
        -P "${SOURCE_ROOT}/cmake/prep/emit_windows_versioninfo.cmake"
    RESULT_VARIABLE _missing_result
    OUTPUT_VARIABLE _missing_stdout
    ERROR_VARIABLE _missing_stderr)
if(_missing_result EQUAL 0 OR NOT "${_missing_stdout}\n${_missing_stderr}" MATCHES "unstable revision")
    message(FATAL_ERROR "A missing numeric reservation tag did not fail closed: ${_missing_stdout}\n${_missing_stderr}")
endif()

file(REMOVE_RECURSE "${_fixture}")
message(STATUS "Windows version-number checks passed")
