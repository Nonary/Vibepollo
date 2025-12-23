# libwebrtc dependency (Windows only)

if(NOT SUNSHINE_ENABLE_WEBRTC)
    return()
endif()

set(WEBRTC_ROOT "${CMAKE_BINARY_DIR}/libwebrtc"
        CACHE PATH "Path to libwebrtc root (contains include/ and lib/).")
set(WEBRTC_LIBRARY "" CACHE FILEPATH "Path to libwebrtc library file.")
set(WEBRTC_INCLUDE_DIR "" CACHE PATH "Path to libwebrtc include directory.")
set(WEBRTC_EXTRA_LIBRARIES "" CACHE STRING "Extra libraries required by libwebrtc.")

if(NOT WEBRTC_INCLUDE_DIR)
    if(WEBRTC_ROOT)
        if(EXISTS "${WEBRTC_ROOT}/include/libwebrtc.h")
            set(WEBRTC_INCLUDE_DIR "${WEBRTC_ROOT}/include")
        endif()
        find_path(WEBRTC_INCLUDE_DIR
                NAMES webrtc/api/peer_connection_interface.h api/peer_connection_interface.h libwebrtc.h
                PATHS "${WEBRTC_ROOT}"
                PATH_SUFFIXES include)
    else()
        find_path(WEBRTC_INCLUDE_DIR
                NAMES webrtc/api/peer_connection_interface.h api/peer_connection_interface.h libwebrtc.h)
    endif()
endif()

if(NOT WEBRTC_LIBRARY)
    if(WEBRTC_ROOT)
        if(WIN32)
            foreach(candidate
                    "${WEBRTC_ROOT}/lib/libwebrtc.dll.lib"
                    "${WEBRTC_ROOT}/lib/libwebrtc.lib"
                    "${WEBRTC_ROOT}/lib/webrtc.lib")
                if(EXISTS "${candidate}")
                    set(WEBRTC_LIBRARY "${candidate}")
                    break()
                endif()
            endforeach()
            find_file(WEBRTC_LIBRARY
                    NAMES libwebrtc.dll.lib libwebrtc.lib webrtc.lib
                    PATHS "${WEBRTC_ROOT}"
                    PATH_SUFFIXES lib)
        else()
            find_library(WEBRTC_LIBRARY
                    NAMES webrtc libwebrtc
                    PATHS "${WEBRTC_ROOT}"
                    PATH_SUFFIXES lib)
        endif()
    else()
        if(WIN32)
            find_file(WEBRTC_LIBRARY
                    NAMES libwebrtc.dll.lib libwebrtc.lib webrtc.lib)
        else()
            find_library(WEBRTC_LIBRARY
                    NAMES webrtc libwebrtc)
        endif()
    endif()
endif()

if(NOT WEBRTC_INCLUDE_DIR OR NOT WEBRTC_LIBRARY)
    message(FATAL_ERROR
            "libwebrtc not found. Set WEBRTC_ROOT to the libwebrtc install root, or "
            "set WEBRTC_INCLUDE_DIR and WEBRTC_LIBRARY explicitly.")
endif()

list(APPEND WEBRTC_INCLUDE_DIRS "${WEBRTC_INCLUDE_DIR}")
list(APPEND WEBRTC_LIBRARIES "${WEBRTC_LIBRARY}" ${WEBRTC_EXTRA_LIBRARIES})
list(APPEND SUNSHINE_EXTERNAL_LIBRARIES ${WEBRTC_LIBRARIES})
list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_ENABLE_WEBRTC=1)
