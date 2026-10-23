cmake_minimum_required(VERSION 3.24)
project(FramePushTransportPrerequisite LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(FRAME_TRANSPORT_DRIVER_ROOT "" CACHE PATH "Exact pinned driver source plus reviewed prerequisite patches")
set(FRAME_TRANSPORT_GTEST_ROOT "" CACHE PATH "Local googletest source; no network fetch")
if(NOT EXISTS "${FRAME_TRANSPORT_DRIVER_ROOT}/src/driver/frame_ring_server.cpp" OR
   NOT EXISTS "${FRAME_TRANSPORT_GTEST_ROOT}/CMakeLists.txt")
  message(FATAL_ERROR "Supply FRAME_TRANSPORT_DRIVER_ROOT and FRAME_TRANSPORT_GTEST_ROOT")
endif()
set(BUILD_VIRTUALDISPLAY_TOOLS OFF CACHE BOOL "" FORCE)
set(BUILD_VIRTUALDISPLAY_PROBE OFF CACHE BOOL "" FORCE)
set(BUILD_VIRTUALDISPLAY_VULKAN_LAYER OFF CACHE BOOL "" FORCE)
add_subdirectory("${FRAME_TRANSPORT_DRIVER_ROOT}" driver)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(INSTALL_GMOCK OFF CACHE BOOL "" FORCE)
add_subdirectory("${FRAME_TRANSPORT_GTEST_ROOT}" googletest)
add_executable(test_frame_push_transport test_frame_push_transport.cpp
    ../../src/platform/windows/frame_push_transport.cpp
    ../../src/platform/windows/frame_push_io.cpp
    ../../src/platform/windows/frame_push_windows_io.cpp
    ../../src/platform/windows/frame_push_d3d11.cpp)
target_link_libraries(test_frame_push_transport PRIVATE gtest_main libvirtualdisplay::driver)
if(WIN32)
  target_link_libraries(test_frame_push_transport PRIVATE d3d11 dxgi)
endif()
enable_testing()
include(GoogleTest)
gtest_discover_tests(test_frame_push_transport DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 10)
