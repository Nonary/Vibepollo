include_guard(GLOBAL)

function(vibepollo_is_sortable_product_guid output_variable value)
  set(is_valid FALSE)
  string(LENGTH "${value}" value_length)
  if(value_length EQUAL 38)
    string(SUBSTRING "${value}" 0 1 opening_brace)
    string(SUBSTRING "${value}" 37 1 closing_brace)
    if(opening_brace STREQUAL "{" AND closing_brace STREQUAL "}")
      string(SUBSTRING "${value}" 1 36 uuid_text)
      string(REPLACE "-" ";" uuid_groups "${uuid_text}")
      list(LENGTH uuid_groups group_count)
      if(group_count EQUAL 5)
        list(GET uuid_groups 0 group_1)
        list(GET uuid_groups 1 group_2)
        list(GET uuid_groups 2 group_3)
        list(GET uuid_groups 3 group_4)
        list(GET uuid_groups 4 group_5)
        string(LENGTH "${group_1}" group_1_length)
        string(LENGTH "${group_2}" group_2_length)
        string(LENGTH "${group_3}" group_3_length)
        string(LENGTH "${group_4}" group_4_length)
        string(LENGTH "${group_5}" group_5_length)
        if(group_1_length EQUAL 8 AND
           group_2_length EQUAL 4 AND
           group_3_length EQUAL 4 AND
           group_4_length EQUAL 4 AND
           group_5_length EQUAL 12)
          string(CONCAT uuid_hex
            "${group_1}" "${group_2}" "${group_3}" "${group_4}" "${group_5}")
          string(SUBSTRING "${group_3}" 0 1 version_nibble)
          string(SUBSTRING "${group_4}" 0 1 variant_nibble)
          if(uuid_hex MATCHES "^[0-9A-Fa-f]+$" AND
             version_nibble STREQUAL "7" AND
             variant_nibble MATCHES "^[89ABab]$")
            set(is_valid TRUE)
          endif()
        endif()
      endif()
    endif()
  endif()
  set(${output_variable} "${is_valid}" PARENT_SCOPE)
endfunction()
