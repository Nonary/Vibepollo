# unix specific packaging
# put anything here that applies to both linux and macos

# return here if building a macos .app
if(APPLE AND NOT SUNSHINE_BUILD_HOMEBREW)
    return()
endif()

# Installation destination dir
set(CPACK_SET_DESTDIR true)
if(NOT CMAKE_INSTALL_PREFIX)
    set(CMAKE_INSTALL_PREFIX "/usr/share/vibepollo")
endif()

install(TARGETS sunshine RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

if(WEBRTC_RUNTIME_LIB)
    install(FILES "${WEBRTC_RUNTIME_LIB}" DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    # Resolve the installed copy relative to the binary so a staged tree runs before install.
    set_target_properties(sunshine PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()
