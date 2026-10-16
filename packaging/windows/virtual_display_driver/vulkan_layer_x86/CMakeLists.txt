cmake_minimum_required(VERSION 3.24)
project(SunshineVulkanHdrLayerX86 LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "The Vulkan HDR x86 layer requires a 32-bit Windows toolchain")
endif()
set(LIBVIRTUALDISPLAY_DIR "" CACHE PATH "Pinned libvirtualdisplay source checkout")
set(_layer_source "${LIBVIRTUALDISPLAY_DIR}/src/driver/vulkan_layer/vk_layer_sunshine_hdr.cpp")
if(NOT EXISTS "${_layer_source}")
    message(FATAL_ERROR "Pinned Vulkan HDR layer source is missing: ${_layer_source}")
endif()
# This layer has no Vulkan SDK, WDK, or display-driver dependency. Keep the
# released x64 driver untouched while providing the same layer to WOW64 games.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_library(vk_layer_sunshine_hdr_x86 SHARED "${_layer_source}")
set_target_properties(vk_layer_sunshine_hdr_x86 PROPERTIES
    OUTPUT_NAME VkLayer_sunshine_hdr_x86 PREFIX "")
target_compile_features(vk_layer_sunshine_hdr_x86 PRIVATE cxx_std_20)
target_link_libraries(vk_layer_sunshine_hdr_x86 PRIVATE user32)
if(MSVC)
    # __stdcall is decorated on x86; the Vulkan loader uses the plain name.
    target_sources(vk_layer_sunshine_hdr_x86 PRIVATE vk_layer_sunshine_hdr_x86.def)
elseif(MINGW)
    target_link_options(vk_layer_sunshine_hdr_x86 PRIVATE -Wl,--kill-at)
endif()
configure_file("${LIBVIRTUALDISPLAY_DIR}/src/driver/vulkan_layer/VkLayer_sunshine_hdr.json"
    "${CMAKE_CURRENT_BINARY_DIR}/VkLayer_sunshine_hdr_x86.json" COPYONLY)
file(READ "${CMAKE_CURRENT_BINARY_DIR}/VkLayer_sunshine_hdr_x86.json" _manifest)
string(REPLACE "VkLayer_sunshine_hdr.dll" "VkLayer_sunshine_hdr_x86.dll" _manifest "${_manifest}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/VkLayer_sunshine_hdr_x86.json" "${_manifest}")

add_executable(vulkan_layer_export_probe check_vulkan_layer_export.cpp)
# An x86 process must load the DLL and find the exact loader entry point before
# package refresh copies it. This also catches missing dynamic CRT dependencies.
if(NOT CMAKE_CROSSCOMPILING)
    add_custom_target(check_vulkan_layer_export
        COMMAND $<TARGET_FILE:vulkan_layer_export_probe> $<TARGET_FILE:vk_layer_sunshine_hdr_x86>
        DEPENDS vulkan_layer_export_probe vk_layer_sunshine_hdr_x86
        VERBATIM)
else()
    add_custom_target(check_vulkan_layer_export
        DEPENDS vulkan_layer_export_probe vk_layer_sunshine_hdr_x86)
endif()
