# This file is loaded by CPack at package time through CPACK_PROJECT_CONFIG_FILE.
# Keep the UpgradeCode stable, but generate a new ProductCode for every MSI so
# same-numeric-version prereleases can be ordered by canonical UUID text.

function(_vibepollo_generate_sortable_product_guid _output_variable)
  set(_guid "")

  if(CMAKE_HOST_WIN32)
    execute_process(
      COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -File
        "${CMAKE_CURRENT_LIST_DIR}/generate_sortable_product_guid.ps1"
      OUTPUT_VARIABLE _guid
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
  endif()

  if(NOT _guid MATCHES "^\\{[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-7[0-9A-Fa-f]{3}-[89ABab][0-9A-Fa-f]{3}-[0-9A-Fa-f]{12}\\}$")
    # Fallback for non-PowerShell hosts.  CMake only exposes seconds here, so
    # random bits keep ProductCodes unique if two packages are produced within
    # the same second; the leading timestamp still preserves sortable ordering.
    string(TIMESTAMP _seconds "%s" UTC)
    math(EXPR _unix_ms "${_seconds} * 1000")
    set(_hex_digits "0123456789abcdef")
    set(_timestamp_hex "")
    set(_hex_value "${_unix_ms}")
    while(_hex_value GREATER 0)
      math(EXPR _hex_remainder "${_hex_value} % 16")
      math(EXPR _hex_value "${_hex_value} / 16")
      string(SUBSTRING "${_hex_digits}" "${_hex_remainder}" 1 _hex_digit)
      string(PREPEND _timestamp_hex "${_hex_digit}")
    endwhile()
    if(_timestamp_hex STREQUAL "")
      set(_timestamp_hex "0")
    endif()
    string(LENGTH "${_timestamp_hex}" _timestamp_length)
    while(_timestamp_length LESS 12)
      string(PREPEND _timestamp_hex "0")
      string(LENGTH "${_timestamp_hex}" _timestamp_length)
    endwhile()
    string(SUBSTRING "${_timestamp_hex}" 0 12 _timestamp_hex)

    string(RANDOM LENGTH 18 ALPHABET "0123456789abcdef" _random_hex)
    string(SUBSTRING "${_timestamp_hex}" 0 8 _time_a)
    string(SUBSTRING "${_timestamp_hex}" 8 4 _time_b)
    string(SUBSTRING "${_random_hex}" 0 3 _rand_a)
    string(SUBSTRING "${_random_hex}" 3 3 _rand_b)
    string(SUBSTRING "${_random_hex}" 6 12 _rand_c)
    set(_guid "{${_time_a}-${_time_b}-7${_rand_a}-8${_rand_b}-${_rand_c}}")
  endif()

  string(TOUPPER "${_guid}" _guid)
  set(${_output_variable} "${_guid}" PARENT_SCOPE)
endfunction()

_vibepollo_generate_sortable_product_guid(_vibepollo_product_guid)
set(CPACK_WIX_PRODUCT_GUID "${_vibepollo_product_guid}")
message(STATUS "CPACK_WIX_PRODUCT_GUID = ${CPACK_WIX_PRODUCT_GUID} (sortable per-package ProductCode)")
