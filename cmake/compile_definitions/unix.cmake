# unix specific compile definitions
# put anything here that applies to both linux and macos

list(APPEND SUNSHINE_EXTERNAL_LIBRARIES
        ${CURL_LIBRARIES})

# Resolve relative asset roots below the install prefix; preserve explicit
# system-wide roots used by split user-binary/system-asset installations.
if(NOT APPLE AND NOT IS_ABSOLUTE "${SUNSHINE_ASSETS_DIR}")
    set(SUNSHINE_ASSETS_DIR "${CMAKE_INSTALL_PREFIX}/${SUNSHINE_ASSETS_DIR}")
endif()
