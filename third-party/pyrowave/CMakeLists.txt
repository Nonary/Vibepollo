# Builds the vendored PyroWave C API (pyrowave/pyrowave.h) as a static library
# for Sunshine. Upstream only exposes the C API as a shared library when it is
# the top-level project, so compile pyrowave_c.cpp (and the colour-conversion
# scaler it uses) into a static target here, against the same pruned Granite
# the codec library uses. Modelled on Punktfunk's pyrowave-sys build.
#
# Consumers link `pyrowave::capi` and include <pyrowave.h> after
# <vulkan/vulkan_core.h>; both include directories are provided by the target.

set(PYROWAVE_VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/pyrowave")
if(NOT EXISTS "${PYROWAVE_VENDOR_DIR}/pyrowave.h")
    message(FATAL_ERROR "Vendored PyroWave is missing at ${PYROWAVE_VENDOR_DIR}. "
            "Run third-party/pyrowave/vendor-pyrowave.ps1.")
endif()

# Granite is only the Vulkan backend the codec is written against; none of its
# engine, windowing, shader compiler or FFmpeg features are wanted.
set(GRANITE_RENDERER OFF CACHE BOOL "" FORCE)
set(GRANITE_VULKAN_SPIRV_CROSS OFF CACHE BOOL "" FORCE)
set(GRANITE_VULKAN_SHADER_MANAGER_RUNTIME_COMPILER OFF CACHE BOOL "" FORCE)
set(GRANITE_VULKAN_SYSTEM_HANDLES OFF CACHE BOOL "" FORCE)
set(GRANITE_VULKAN_FOSSILIZE OFF CACHE BOOL "" FORCE)
set(GRANITE_VULKAN_DXGI_INTEROP OFF CACHE BOOL "" FORCE)
set(GRANITE_SHADER_COMPILER_OPTIMIZE OFF CACHE BOOL "" FORCE)
set(GRANITE_POSITION_INDEPENDENT ON CACHE BOOL "" FORCE)
set(GRANITE_SHIPPING ON CACHE BOOL "" FORCE)
set(GRANITE_TOOLS OFF CACHE BOOL "" FORCE)
set(GRANITE_FFMPEG OFF CACHE BOOL "" FORCE)
set(GRANITE_FFMPEG_VULKAN OFF CACHE BOOL "" FORCE)
set(GRANITE_AUDIO OFF CACHE BOOL "" FORCE)
set(GRANITE_BULLET OFF CACHE BOOL "" FORCE)
set(GRANITE_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
set(GRANITE_INSTALL_EXE_TARGETS OFF CACHE BOOL "" FORCE)
set(GRANITE_PLATFORM "null" CACHE STRING "" FORCE)
# volk defines the Vulkan entry points as global function pointers. Keep them
# (and every other Granite/PyroWave symbol) out of the dynamic symbol table so
# they cannot interpose on libraries that link libvulkan on Linux. PE images
# export nothing by default, so Windows needs no visibility flags.
if(WIN32)
    set(GRANITE_HIDDEN OFF CACHE BOOL "" FORCE)
else()
    set(GRANITE_HIDDEN ON CACHE BOOL "" FORCE)
endif()

add_subdirectory("${PYROWAVE_VENDOR_DIR}/Granite" "${CMAKE_CURRENT_BINARY_DIR}/Granite" EXCLUDE_FROM_ALL)
add_subdirectory("${PYROWAVE_VENDOR_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/pyrowave" EXCLUDE_FROM_ALL)

add_library(pyrowave-capi STATIC
        "${PYROWAVE_VENDOR_DIR}/pyrowave_c.cpp"
        "${PYROWAVE_VENDOR_DIR}/Granite/video/scaler.cpp")
# Upstream's shared target compiles the same files; PYROWAVE_EXPORT_SYMBOLS stays
# undefined so the entry points carry no dllexport attribute.
target_include_directories(pyrowave-capi PRIVATE
        "${PYROWAVE_VENDOR_DIR}/shaders"
        "${PYROWAVE_VENDOR_DIR}/Granite/video")
target_link_libraries(pyrowave-capi PRIVATE pyrowave granite-vulkan granite-math)
set_target_properties(pyrowave-capi PROPERTIES
        CXX_STANDARD 14
        POSITION_INDEPENDENT_CODE ON)
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(pyrowave-capi PRIVATE -Wno-missing-field-initializers)
    if(NOT WIN32)
        target_compile_options(pyrowave-capi PRIVATE -fvisibility=hidden)
    endif()
elseif(MSVC)
    target_compile_options(pyrowave-capi PRIVATE /D_CRT_SECURE_NO_WARNINGS /wd4267 /wd4244 /wd4309 /wd4005)
endif()
if(WIN32)
    # Granite's breadcrumbs tracker reports device hangs with MessageBoxA.
    target_link_libraries(pyrowave-capi PRIVATE user32)
endif()

# Public face: the C header and the Vulkan headers it was compiled against.
add_library(pyrowave-capi-interface INTERFACE)
target_include_directories(pyrowave-capi-interface INTERFACE
        "${PYROWAVE_VENDOR_DIR}"
        "${PYROWAVE_VENDOR_DIR}/Granite/third_party/khronos/vulkan-headers/include")
target_link_libraries(pyrowave-capi-interface INTERFACE pyrowave-capi)
add_library(pyrowave::capi ALIAS pyrowave-capi-interface)
