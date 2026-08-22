cmake_minimum_required(VERSION 3.25)

get_filename_component(wix_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${wix_root}/sortable_product_guid_contract.cmake")

function(assert_guid_accepted case_name value)
  vibepollo_is_sortable_product_guid(case_accepted "${value}")
  if(NOT case_accepted)
    message(FATAL_ERROR "${case_name}: valid sortable UUIDv7 value was rejected.")
  endif()
endfunction()

function(assert_guid_rejected case_name value)
  vibepollo_is_sortable_product_guid(case_accepted "${value}")
  if(case_accepted)
    message(FATAL_ERROR "${case_name}: malformed sortable UUIDv7 value was accepted.")
  endif()
endfunction()

assert_guid_accepted(
  "canonical UUIDv7"
  "{018F0C11-1111-7ABC-8DEF-0123456789AB}")
assert_guid_accepted(
  "lowercase UUIDv7"
  "{018f0c11-1111-7abc-abcd-0123456789ab}")

assert_guid_rejected(
  "malformed group lengths"
  "{018F0C1-11111-7ABC-8DEF-0123456789AB}")
assert_guid_rejected(
  "short overall value"
  "{018F0C11-1111-7ABC-8DEF-0123456789A}")
assert_guid_rejected(
  "nonhex value"
  "{018F0C1G-1111-7ABC-8DEF-0123456789AB}")
assert_guid_rejected(
  "non-v7 version"
  "{018F0C11-1111-6ABC-8DEF-0123456789AB}")
assert_guid_rejected(
  "invalid UUID variant"
  "{018F0C11-1111-7ABC-7DEF-0123456789AB}")

message(STATUS "Sortable UUIDv7 ProductCode contract checks passed.")
