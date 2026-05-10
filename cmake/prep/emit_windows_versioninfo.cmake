# Build-time generator for the Windows VERSIONINFO header.
#
# Runs during the Windows build so the fixed FILEVERSION stamped into the
# resource reflects the current git/build state instead of the last CMake
# configure.  The visible string version remains PROJECT_VERSION_FULL.
#
# For normal semver tags (patch <= 65534), the fixed version is:
#   major.minor.patch.revision
# where revision is a monotonic 2-minute slot from the earliest reachable tag in
# the same major.minor.patch series.  Dirty and post-tag builds use current UTC
# time; exact clean tag builds use the tag/commit time.  A build-dir cache keeps
# repeated local builds in the same slot strictly increasing.
#
# For oversized/date-style patch values, the legacy split fallback is preserved:
#   major.minor.(patch / 100).(patch % 100)
#
# Invoked from cmake/compile_definitions/windows.cmake; do not invoke directly
# for production builds.  Tests may pass TEST_ANCHOR_TIME_EPOCH and
# TEST_SOURCE_TIME_EPOCH to exercise edge cases without manufacturing git dates.

if(NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "emit_windows_versioninfo: OUTPUT_FILE not specified")
endif()
if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "emit_windows_versioninfo: SOURCE_DIR not specified")
endif()
if(NOT DEFINED CACHE_FILE OR "${CACHE_FILE}" STREQUAL "")
    set(CACHE_FILE "${OUTPUT_FILE}.cache")
endif()

function(_wv_int_or_zero input output_var)
    if("${input}" MATCHES "^[0-9]+$")
        set(${output_var} "${input}" PARENT_SCOPE)
    else()
        set(${output_var} 0 PARENT_SCOPE)
    endif()
endfunction()

function(_wv_current_time output_var)
    if(DEFINED TEST_SOURCE_TIME_EPOCH AND NOT "${TEST_SOURCE_TIME_EPOCH}" STREQUAL "")
        set(${output_var} "${TEST_SOURCE_TIME_EPOCH}" PARENT_SCOPE)
    else()
        string(TIMESTAMP _now_epoch "%s" UTC)
        set(${output_var} "${_now_epoch}" PARENT_SCOPE)
    endif()
endfunction()

function(_wv_git_epoch_for_ref ref output_var)
    set(_ref_time "")
    if(GIT_EXECUTABLE AND NOT "${ref}" STREQUAL "")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} for-each-ref "refs/tags/${ref}" --format=%(creatordate:unix)
            WORKING_DIRECTORY "${SOURCE_DIR}"
            OUTPUT_VARIABLE _creator_raw
            RESULT_VARIABLE _creator_error
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(NOT _creator_error AND "${_creator_raw}" MATCHES "^[0-9]+$")
            set(_ref_time "${_creator_raw}")
        endif()

        if("${_ref_time}" STREQUAL "")
            execute_process(
                COMMAND ${GIT_EXECUTABLE} log -1 --format=%ct "${ref}"
                WORKING_DIRECTORY "${SOURCE_DIR}"
                OUTPUT_VARIABLE _commit_raw
                RESULT_VARIABLE _commit_error
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
            if(NOT _commit_error AND "${_commit_raw}" MATCHES "^[0-9]+$")
                set(_ref_time "${_commit_raw}")
            endif()
        endif()
    endif()
    set(${output_var} "${_ref_time}" PARENT_SCOPE)
endfunction()

function(_wv_scan_tag_lines lines major minor patch anchor_var exact_var exact_tag_var)
    set(_anchor_time "")
    set(_exact_time "")
    set(_exact_tag "")

    foreach(_line IN LISTS lines)
        if("${_line}" STREQUAL "")
            continue()
        endif()

        string(REPLACE "|" ";" _parts "${_line}")
        list(LENGTH _parts _part_count)
        if(_part_count LESS 1)
            continue()
        endif()
        list(GET _parts 0 _tag_name)
        set(_tag_time "")
        if(_part_count GREATER 1)
            list(GET _parts 1 _tag_time)
        endif()

        if(NOT "${_tag_name}" MATCHES "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)(($)|[-.+].*)")
            continue()
        endif()
        if(NOT "${CMAKE_MATCH_1}" STREQUAL "${major}" OR
           NOT "${CMAKE_MATCH_2}" STREQUAL "${minor}" OR
           NOT "${CMAKE_MATCH_3}" STREQUAL "${patch}")
            continue()
        endif()

        if(NOT "${_tag_time}" MATCHES "^[0-9]+$")
            _wv_git_epoch_for_ref("${_tag_name}" _tag_time)
        endif()
        if(NOT "${_tag_time}" MATCHES "^[0-9]+$")
            continue()
        endif()

        if("${_anchor_time}" STREQUAL "" OR _tag_time LESS _anchor_time)
            set(_anchor_time "${_tag_time}")
        endif()
        if("${_exact_time}" STREQUAL "" OR _tag_time LESS _exact_time)
            set(_exact_time "${_tag_time}")
            set(_exact_tag "${_tag_name}")
        endif()
    endforeach()

    set(${anchor_var} "${_anchor_time}" PARENT_SCOPE)
    set(${exact_var} "${_exact_time}" PARENT_SCOPE)
    set(${exact_tag_var} "${_exact_tag}" PARENT_SCOPE)
endfunction()

function(_wv_read_previous_from_header header key out_var)
    set(_previous "")
    if(EXISTS "${header}")
        file(READ "${header}" _header_content)
        set(_old_major "")
        set(_old_minor "")
        set(_old_patch "")
        set(_old_build "")
        set(_old_revision "")
        if(_header_content MATCHES "#define[ \t]+RC_VERSION_MAJOR[ \t]+([0-9]+)")
            set(_old_major "${CMAKE_MATCH_1}")
        endif()
        if(_header_content MATCHES "#define[ \t]+RC_VERSION_MINOR[ \t]+([0-9]+)")
            set(_old_minor "${CMAKE_MATCH_1}")
        endif()
        if(_header_content MATCHES "#define[ \t]+RC_VERSION_PATCH[ \t]+([0-9]+)")
            set(_old_patch "${CMAKE_MATCH_1}")
        endif()
        if(_header_content MATCHES "#define[ \t]+RC_VERSION_BUILD[ \t]+([0-9]+)")
            set(_old_build "${CMAKE_MATCH_1}")
        endif()
        if(_header_content MATCHES "#define[ \t]+RC_VERSION_REVISION[ \t]+([0-9]+)")
            set(_old_revision "${CMAKE_MATCH_1}")
        endif()
        if("${key}" MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
            if("${_old_major}" STREQUAL "")
                set(_old_major "${CMAKE_MATCH_1}")
            endif()
            if("${_old_minor}" STREQUAL "")
                set(_old_minor "${CMAKE_MATCH_2}")
            endif()
            if("${_old_patch}" STREQUAL "" AND "${_old_build}" MATCHES "^[0-9]+$")
                set(_old_patch "${_old_build}")
            endif()
        endif()
        set(_old_key "${_old_major}.${_old_minor}.${_old_patch}")
        if("${_old_key}" STREQUAL "${key}" AND "${_old_revision}" MATCHES "^[0-9]+$")
            set(_previous "${_old_revision}")
        endif()
    endif()
    set(${out_var} "${_previous}" PARENT_SCOPE)
endfunction()

function(_wv_read_previous_from_cache cache key out_var)
    set(_previous "")
    if(EXISTS "${cache}")
        file(READ "${cache}" _cache_content)
        set(_cache_key "")
        set(_cache_revision "")
        if(_cache_content MATCHES "(^|\n)key=([^\n\r]+)")
            set(_cache_key "${CMAKE_MATCH_2}")
        endif()
        if(_cache_content MATCHES "(^|\n)revision=([0-9]+)")
            set(_cache_revision "${CMAKE_MATCH_2}")
        endif()
        if("${_cache_key}" STREQUAL "${key}" AND "${_cache_revision}" MATCHES "^[0-9]+$")
            set(_previous "${_cache_revision}")
        endif()
    endif()
    set(${out_var} "${_previous}" PARENT_SCOPE)
endfunction()

_wv_int_or_zero("${PROJECT_VERSION_MAJOR}" _major)
_wv_int_or_zero("${PROJECT_VERSION_MINOR}" _minor)
_wv_int_or_zero("${PROJECT_VERSION_PATCH}" _patch)

set(_rc_major "${_major}")
set(_rc_minor "${_minor}")
set(_rc_patch "${_patch}")
set(_rc_build 0)
set(_rc_revision 0)
set(_source_kind "legacy")
set(_anchor_time "")
set(_source_time "")
set(_exact_tag "")
set(_dirty 0)
set(_previous_revision "")
set(_computed_revision "")

if(_patch GREATER 65534)
    math(EXPR _rc_build "${_patch} / 100")
    math(EXPR _rc_revision "${_patch} % 100")
    set(_computed_revision "${_rc_revision}")
    set(_source_kind "large-patch-fallback")
else()
    set(_rc_build "${_patch}")

    find_package(Git QUIET)

    if(GIT_EXECUTABLE)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=normal
            WORKING_DIRECTORY "${SOURCE_DIR}"
            OUTPUT_VARIABLE _status_raw
            RESULT_VARIABLE _status_error
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(NOT _status_error AND NOT "${_status_raw}" STREQUAL "")
            set(_dirty 1)
        endif()

        execute_process(
            COMMAND ${GIT_EXECUTABLE} tag --merged HEAD "--format=%(refname:short)|%(creatordate:unix)"
            WORKING_DIRECTORY "${SOURCE_DIR}"
            OUTPUT_VARIABLE _merged_tags_raw
            RESULT_VARIABLE _merged_tags_error
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(NOT _merged_tags_error AND NOT "${_merged_tags_raw}" STREQUAL "")
            string(REPLACE "\n" ";" _merged_tag_lines "${_merged_tags_raw}")
            _wv_scan_tag_lines("${_merged_tag_lines}" "${_major}" "${_minor}" "${_patch}" _anchor_time _unused_exact_time _unused_exact_tag)
        endif()

        execute_process(
            COMMAND ${GIT_EXECUTABLE} tag --points-at HEAD "--format=%(refname:short)|%(creatordate:unix)"
            WORKING_DIRECTORY "${SOURCE_DIR}"
            OUTPUT_VARIABLE _head_tags_raw
            RESULT_VARIABLE _head_tags_error
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(NOT _head_tags_error AND NOT "${_head_tags_raw}" STREQUAL "")
            string(REPLACE "\n" ";" _head_tag_lines "${_head_tags_raw}")
            _wv_scan_tag_lines("${_head_tag_lines}" "${_major}" "${_minor}" "${_patch}" _unused_anchor_time _exact_tag_time _exact_tag)
        endif()
    endif()

    if(DEFINED TEST_ANCHOR_TIME_EPOCH AND NOT "${TEST_ANCHOR_TIME_EPOCH}" STREQUAL "")
        set(_anchor_time "${TEST_ANCHOR_TIME_EPOCH}")
    endif()

    if(NOT "${_anchor_time}" MATCHES "^[0-9]+$")
        if(GIT_EXECUTABLE)
            execute_process(
                COMMAND ${GIT_EXECUTABLE} log -1 --format=%ct HEAD
                WORKING_DIRECTORY "${SOURCE_DIR}"
                OUTPUT_VARIABLE _head_time_raw
                RESULT_VARIABLE _head_time_error
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
            if(NOT _head_time_error AND "${_head_time_raw}" MATCHES "^[0-9]+$")
                set(_anchor_time "${_head_time_raw}")
            endif()
        endif()
    endif()
    if(NOT "${_anchor_time}" MATCHES "^[0-9]+$")
        _wv_current_time(_anchor_time)
    endif()

    if(NOT _dirty AND "${_exact_tag_time}" MATCHES "^[0-9]+$")
        set(_source_time "${_exact_tag_time}")
        set(_source_kind "exact-clean-tag")
    else()
        _wv_current_time(_source_time)
        if(_dirty)
            set(_source_kind "dirty-build")
        else()
            set(_source_kind "post-tag-build")
        endif()
    endif()

    if(DEFINED TEST_SOURCE_TIME_EPOCH AND NOT "${TEST_SOURCE_TIME_EPOCH}" STREQUAL "")
        set(_source_time "${TEST_SOURCE_TIME_EPOCH}")
        if("${_source_kind}" STREQUAL "")
            set(_source_kind "test-source-time")
        endif()
    endif()

    math(EXPR _elapsed_seconds "${_source_time} - ${_anchor_time}")
    if(_elapsed_seconds LESS 0)
        message(WARNING "emit_windows_versioninfo: source time ${_source_time} predates anchor ${_anchor_time}; clamping elapsed time to 0")
        set(_elapsed_seconds 0)
    endif()

    math(EXPR _rc_revision "(${_elapsed_seconds} / 120) + 1")
    set(_computed_revision "${_rc_revision}")

    set(_cache_key "${_major}.${_minor}.${_patch}")
    _wv_read_previous_from_cache("${CACHE_FILE}" "${_cache_key}" _previous_from_cache)
    _wv_read_previous_from_header("${OUTPUT_FILE}" "${_cache_key}" _previous_from_header)

    if("${_previous_from_cache}" MATCHES "^[0-9]+$")
        set(_previous_revision "${_previous_from_cache}")
    endif()
    if("${_previous_from_header}" MATCHES "^[0-9]+$" AND
       ("${_previous_revision}" STREQUAL "" OR _previous_from_header GREATER _previous_revision))
        set(_previous_revision "${_previous_from_header}")
    endif()

    if("${_previous_revision}" MATCHES "^[0-9]+$" AND NOT _rc_revision GREATER _previous_revision)
        math(EXPR _rc_revision "${_previous_revision} + 1")
    endif()

    if(_rc_revision GREATER 65534)
        message(FATAL_ERROR
            "emit_windows_versioninfo: computed RC_VERSION_REVISION=${_rc_revision} exceeds 65534 for ${_major}.${_minor}.${_patch}. "
            "Create a new tag/version before producing another Windows installer build. "
            "anchor=${_anchor_time}, source=${_source_time}, computed=${_computed_revision}, previous=${_previous_revision}")
    endif()
endif()

get_filename_component(_output_dir "${OUTPUT_FILE}" DIRECTORY)
if(NOT "${_output_dir}" STREQUAL "")
    file(MAKE_DIRECTORY "${_output_dir}")
endif()
get_filename_component(_cache_dir "${CACHE_FILE}" DIRECTORY)
if(NOT "${_cache_dir}" STREQUAL "")
    file(MAKE_DIRECTORY "${_cache_dir}")
endif()

set(_header_content
"// Auto-generated by emit_windows_versioninfo.cmake -- do not edit.
#ifndef SUNSHINE_WINDOWS_VERSIONINFO_GENERATED_H
#define SUNSHINE_WINDOWS_VERSIONINFO_GENERATED_H
#define RC_VERSION_MAJOR ${_rc_major}
#define RC_VERSION_MINOR ${_rc_minor}
#define RC_VERSION_PATCH ${_rc_patch}
#define RC_VERSION_BUILD ${_rc_build}
#define RC_VERSION_REVISION ${_rc_revision}
#endif
")

if(EXISTS "${OUTPUT_FILE}")
    file(READ "${OUTPUT_FILE}" _existing_content)
else()
    set(_existing_content "")
endif()

if(NOT _existing_content STREQUAL _header_content)
    file(WRITE "${OUTPUT_FILE}" "${_header_content}")
endif()

set(_cache_content
"key=${_rc_major}.${_rc_minor}.${_rc_patch}
version=${_rc_major}.${_rc_minor}.${_rc_build}.${_rc_revision}
revision=${_rc_revision}
computed_revision=${_computed_revision}
anchor_time=${_anchor_time}
source_time=${_source_time}
source_kind=${_source_kind}
exact_tag=${_exact_tag}
dirty=${_dirty}
")
file(WRITE "${CACHE_FILE}" "${_cache_content}")

message(STATUS
    "emit_windows_versioninfo: fixed FILEVERSION=${_rc_major}.${_rc_minor}.${_rc_build}.${_rc_revision} "
    "(series=${_rc_major}.${_rc_minor}.${_rc_patch}, source=${_source_kind}, anchor=${_anchor_time}, source_time=${_source_time}, previous=${_previous_revision})")
