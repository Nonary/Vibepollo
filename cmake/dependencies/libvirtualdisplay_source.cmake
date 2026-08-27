include_guard(GLOBAL)

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
