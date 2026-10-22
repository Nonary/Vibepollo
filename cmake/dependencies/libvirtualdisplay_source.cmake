include_guard(GLOBAL)

# Keep the host capture path and VDD producer as one explicit, default-off
# opt-in. This file is included before libvirtualdisplay is added, so the
# driver's CMake option receives the same decision for this build.
option(SUNSHINE_ENABLE_FRAME_PUSH_CAPTURE
        "Compile the experimental frame-push capture path (default off)"
        OFF)

set(SUNSHINE_LIBVIRTUALDISPLAY_SOURCE_DIR "" CACHE PATH "Path to libvirtualdisplay source")
if(NOT SUNSHINE_LIBVIRTUALDISPLAY_SOURCE_DIR)
    if(EXISTS "${CMAKE_SOURCE_DIR}/third-party/libvirtualdisplay/CMakeLists.txt")
        set(SUNSHINE_LIBVIRTUALDISPLAY_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third-party/libvirtualdisplay")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/../libvirtualdisplay/CMakeLists.txt")
        set(SUNSHINE_LIBVIRTUALDISPLAY_SOURCE_DIR "${CMAKE_SOURCE_DIR}/../libvirtualdisplay")
    endif()
endif()

if(NOT SUNSHINE_LIBVIRTUALDISPLAY_SOURCE_DIR OR
        NOT EXISTS "${SUNSHINE_LIBVIRTUALDISPLAY_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "libvirtualdisplay source not found. Set SUNSHINE_LIBVIRTUALDISPLAY_SOURCE_DIR.")
endif()

if(SUNSHINE_ENABLE_FRAME_PUSH_CAPTURE)
    if(NOT WIN32)
        message(FATAL_ERROR "SUNSHINE_ENABLE_FRAME_PUSH_CAPTURE requires a Windows target.")
    endif()
    if(NOT EXISTS "${SUNSHINE_LIBVIRTUALDISPLAY_SOURCE_DIR}/src/driver/include/virtual_display/driver/frame_transport_protocol.h")
        message(FATAL_ERROR "SUNSHINE_ENABLE_FRAME_PUSH_CAPTURE requires the reviewed libvirtualdisplay frame-push protocol source.")
    endif()
    set(LIBVIRTUALDISPLAY_ENABLE_FRAME_TRANSPORT ON CACHE BOOL
            "Build the experimental Windows frame-ring producer" FORCE)
else()
    # Reconfiguration of an existing build tree must keep the producer paired
    # with the host opt-in instead of retaining a cached ON value.
    set(LIBVIRTUALDISPLAY_ENABLE_FRAME_TRANSPORT OFF CACHE BOOL
            "Build the experimental Windows frame-ring producer" FORCE)
endif()

set(LIBVIRTUALDISPLAY_LINUX_ROOT "${SUNSHINE_LIBVIRTUALDISPLAY_SOURCE_DIR}/linux")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(LIBVIRTUALDISPLAY_LINUX_UAPI_INCLUDE_DIR
            "${LIBVIRTUALDISPLAY_LINUX_ROOT}/vibeshine-drm")
    if(NOT EXISTS "${LIBVIRTUALDISPLAY_LINUX_UAPI_INCLUDE_DIR}/vibeshine_drm_uapi.h")
        message(FATAL_ERROR
                "libvirtualdisplay DRM UAPI header not found: "
                "${LIBVIRTUALDISPLAY_LINUX_UAPI_INCLUDE_DIR}/vibeshine_drm_uapi.h")
    endif()

    add_library(sunshine_libvirtualdisplay_uapi INTERFACE)
    target_include_directories(sunshine_libvirtualdisplay_uapi SYSTEM INTERFACE
            "${LIBVIRTUALDISPLAY_LINUX_UAPI_INCLUDE_DIR}")
endif()
