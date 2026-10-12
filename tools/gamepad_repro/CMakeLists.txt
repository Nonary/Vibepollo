cmake_minimum_required(VERSION 3.21)
project(vhf_gamepad_repro LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This probe needs Windows and an installed libvirtualgamepad driver.")
endif()

# Standalone build: only the client library and Windows SDK are needed, not WDK.
set(LVG_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../third-party/libvirtualgamepad")
add_subdirectory("${LVG_ROOT}/client" "libvirtualgamepad-client")
add_executable(vhf-gamepad-repro main.cpp)
target_compile_features(vhf-gamepad-repro PRIVATE cxx_std_20)
target_compile_definitions(vhf-gamepad-repro PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(vhf-gamepad-repro PRIVATE libvirtualgamepad::client hid)
