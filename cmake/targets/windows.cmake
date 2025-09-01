# windows specific target definitions
set_target_properties(sunshine PROPERTIES LINK_SEARCH_START_STATIC 1)
set(CMAKE_FIND_LIBRARY_SUFFIXES ".dll")
find_library(ZLIB ZLIB1)
list(APPEND SUNSHINE_EXTERNAL_LIBRARIES
        $<TARGET_OBJECTS:sunshine_rc_object>
        Windowsapp.lib
        Wtsapi32.lib)

# Ensure the Windows display helper is built and placed next to the Sunshine binary
# so the runtime launcher can find it reliably.
if (TARGET sunshine_display_helper)
    # Build helper before sunshine to make the copy step reliable
    add_dependencies(sunshine sunshine_display_helper)

    # Copy helper next to the sunshine executable after build
    add_custom_command(TARGET sunshine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:sunshine_display_helper>
                $<TARGET_FILE_DIR:sunshine>
        COMMENT "Copying sunshine_display_helper next to Sunshine binary")
endif()
