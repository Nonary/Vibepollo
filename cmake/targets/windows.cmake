# windows specific target definitions
set_target_properties(sunshine PROPERTIES LINK_SEARCH_START_STATIC 1)
set(CMAKE_FIND_LIBRARY_SUFFIXES ".dll")
find_library(ZLIB ZLIB1)
list(APPEND SUNSHINE_EXTERNAL_LIBRARIES
        $<TARGET_OBJECTS:sunshine_rc_object>
        Windowsapp.lib
        Wtsapi32.lib
        avrt.lib)

# Copy Playnite plugin sources into build output (for packaging/installers)
## Copy Playnite plugin sources into build output (for packaging/installers)
## Make the copy step incremental: only re-run when source files change.
file(GLOB_RECURSE SUNSHINE_PLAYNITE_PLUGIN_SOURCES
        CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/plugins/playnite/*")
set(SUNSHINE_PLAYNITE_PLUGIN_STAMP "${CMAKE_BINARY_DIR}/plugins/playnite/.copy_stamp")

add_custom_command(
        OUTPUT ${SUNSHINE_PLAYNITE_PLUGIN_STAMP}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/plugins/playnite"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/plugins/playnite" "${CMAKE_BINARY_DIR}/plugins/playnite"
        COMMAND ${CMAKE_COMMAND} -E touch ${SUNSHINE_PLAYNITE_PLUGIN_STAMP}
        DEPENDS ${SUNSHINE_PLAYNITE_PLUGIN_SOURCES}
        COMMENT "Copying Playnite plugin sources"
)
add_custom_target(copy_playnite_plugin DEPENDS ${SUNSHINE_PLAYNITE_PLUGIN_STAMP})
add_dependencies(sunshine copy_playnite_plugin)

# Ensure the Windows display helper is built and staged under the Sunshine tools
# directory so the runtime launcher can find it reliably.
if (TARGET sunshine_display_helper)
    # Build helper before sunshine to make the copy step reliable
    add_dependencies(sunshine sunshine_display_helper)

    # Copy helper into the tools directory next to the sunshine executable after build
    add_custom_command(TARGET sunshine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:sunshine>/tools"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:sunshine_display_helper>
                "$<TARGET_FILE_DIR:sunshine>/tools"
        COMMENT "Copying sunshine_display_helper into tools directory")
endif()

# Enable libdisplaydevice logging in the main Sunshine binary only
target_compile_definitions(sunshine PRIVATE SUNSHINE_USE_DISPLAYDEVICE_LOGGING)

# Convenience target to build MSI via CPack (WiX)
add_custom_target(package_msi
    COMMAND "${CMAKE_CPACK_COMMAND}" -G WIX -C "$<IF:$<CONFIG:>,${CMAKE_BUILD_TYPE},$<CONFIG>>"
    DEPENDS sunshine copy_playnite_plugin
    COMMENT "Building MSI installer via CPack (WiX)"
)
