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
