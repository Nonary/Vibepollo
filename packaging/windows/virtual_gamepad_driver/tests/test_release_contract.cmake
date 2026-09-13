cmake_minimum_required(VERSION 3.25)

get_filename_component(repository_root "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
include("${repository_root}/cmake/packaging/windows_virtual_gamepad_contract.cmake")

# The runtime installer independently validates the producer identity. A
# packaging-only pin update otherwise builds successfully but fails at install.
file(READ "${repository_root}/src_assets/windows/drivers/vhf-gamepad/install.ps1" installer_script)
string(REGEX REPLACE "^v" "" producer_version "${SUNSHINE_VHF_GAMEPAD_RELEASE_TAG}")
foreach(expected_assignment IN ITEMS
    "$expectedProducerTag = '${SUNSHINE_VHF_GAMEPAD_RELEASE_TAG}'"
    "$expectedProducerAsset = 'libvirtualgamepad-${producer_version}-windows-x64.zip'"
    "$expectedProducerArchiveSha256 = '${SUNSHINE_VHF_GAMEPAD_RELEASE_ASSET_SHA256}'"
    "$expectedProducerSourceRevision = '${SUNSHINE_VHF_GAMEPAD_SOURCE_REVISION}'"
    "$expectedDriverVer = '${SUNSHINE_VHF_GAMEPAD_DRIVER_VER}'"
    "$expectedProtocolVersion = ${SUNSHINE_VHF_GAMEPAD_PROTOCOL_VERSION}")
    string(FIND "${installer_script}" "${expected_assignment}" assignment_offset)
    if(assignment_offset EQUAL -1)
        message(FATAL_ERROR "Runtime installer producer pin differs from packaging: ${expected_assignment}")
    endif()
endforeach()

function(assert_fixed_hex_accepted case_name value expected_length)
    sunshine_vhf_is_fixed_length_hex(case_accepted "${value}" "${expected_length}")
    if(NOT case_accepted)
        message(FATAL_ERROR "${case_name}: valid fixed-length hexadecimal value was rejected.")
    endif()
endfunction()

function(assert_fixed_hex_rejected case_name value expected_length)
    sunshine_vhf_is_fixed_length_hex(case_accepted "${value}" "${expected_length}")
    if(case_accepted)
        message(FATAL_ERROR "${case_name}: invalid fixed-length hexadecimal value was accepted.")
    endif()
endfunction()

string(REPEAT "a" 64 sha256_lower)
string(TOUPPER "${sha256_lower}" sha256_upper)
assert_fixed_hex_accepted("lowercase SHA-256" "${sha256_lower}" 64)
assert_fixed_hex_accepted("uppercase SHA-256" "${sha256_upper}" 64)

string(REPEAT "b" 40 sha1_lower)
string(TOUPPER "${sha1_lower}" sha1_upper)
assert_fixed_hex_accepted("lowercase SHA-1" "${sha1_lower}" 40)
assert_fixed_hex_accepted("uppercase SHA-1" "${sha1_upper}" 40)

string(REPEAT "c" 63 sha256_short)
string(REPEAT "d" 65 sha256_long)
string(REPEAT "e" 63 sha256_nonhex_prefix)
assert_fixed_hex_rejected("short SHA-256" "${sha256_short}" 64)
assert_fixed_hex_rejected("long SHA-256" "${sha256_long}" 64)
assert_fixed_hex_rejected("nonhex SHA-256" "${sha256_nonhex_prefix}g" 64)

string(REPEAT "1" 39 sha1_short)
string(REPEAT "2" 41 sha1_long)
string(REPEAT "3" 39 sha1_nonhex_prefix)
assert_fixed_hex_rejected("short SHA-1" "${sha1_short}" 40)
assert_fixed_hex_rejected("long SHA-1" "${sha1_long}" 40)
assert_fixed_hex_rejected("nonhex SHA-1" "${sha1_nonhex_prefix}z" 40)

message(STATUS "VHF fixed-length hexadecimal release contract checks passed.")
