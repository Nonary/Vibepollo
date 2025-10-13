cmake_minimum_required(VERSION 3.20)

project(sunshine_tools)

include_directories(${CMAKE_SOURCE_DIR})

add_executable(dxgi-info dxgi.cpp)
set_target_properties(dxgi-info PROPERTIES CXX_STANDARD 23)
target_link_libraries(dxgi-info
        ${CMAKE_THREAD_LIBS_INIT}
        dxgi
        ${PLATFORM_LIBRARIES})
target_compile_options(dxgi-info PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_executable(audio-info audio.cpp utils.cpp)
set_target_properties(audio-info PROPERTIES CXX_STANDARD 23)
target_link_libraries(audio-info
        ${Boost_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        ksuser
        ${PLATFORM_LIBRARIES})
target_compile_options(audio-info PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_executable(sunshinesvc sunshinesvc.cpp)
set_target_properties(sunshinesvc PROPERTIES CXX_STANDARD 23)
target_link_libraries(sunshinesvc
        ${CMAKE_THREAD_LIBS_INIT}
        wtsapi32
        ${PLATFORM_LIBRARIES})
target_compile_options(sunshinesvc PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_custom_command(TARGET sunshinesvc POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/wix_payload/tools"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:sunshinesvc>" "${CMAKE_BINARY_DIR}/wix_payload/tools/sunshinesvc.exe"
)

# Playnite launcher: per-launch helper that hosts a private IPC server
add_executable(playnite-launcher
        playnite_launcher/main.cpp
        playnite_launcher/launcher.cpp
        playnite_launcher/arguments.cpp
        playnite_launcher/cleanup.cpp
        playnite_launcher/focus_utils.cpp
        playnite_launcher/lossless_scaling.cpp
        playnite_launcher/playnite_process.cpp)
set_target_properties(playnite-launcher PROPERTIES CXX_STANDARD 23)
set_target_properties(playnite-launcher PROPERTIES WIN32_EXECUTABLE TRUE)
target_include_directories(playnite-launcher PRIVATE ${CMAKE_SOURCE_DIR}/tools/playnite_launcher)
target_sources(playnite-launcher PRIVATE
        ${CMAKE_SOURCE_DIR}/src/logging.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/playnite_ipc.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/playnite_protocol.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/pipes.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/misc_utils.cpp)
target_compile_definitions(playnite-launcher PRIVATE SUNSHINE_PLAYNITE_LAUNCHER)
target_link_libraries(playnite-launcher
        ${Boost_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        nlohmann_json::nlohmann_json
        user32
        userenv
        ole32
        oleaut32
        Psapi
        shell32
        shlwapi
        wtsapi32
        ${PLATFORM_LIBRARIES})
target_compile_options(playnite-launcher PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
# Add sunshine_wgc_capture executable
add_executable(sunshine_wgc_capture
        sunshine_wgc_capture.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/pipes.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/misc_utils.cpp
        ${CMAKE_SOURCE_DIR}/src/logging.cpp)
set_target_properties(sunshine_wgc_capture PROPERTIES CXX_STANDARD 23)

# Define helper build flag to conditionally compile logging
target_compile_definitions(sunshine_wgc_capture PRIVATE
        SUNSHINE_EXTERNAL_PROCESS=1
)

target_link_libraries(sunshine_wgc_capture
        ${Boost_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        d3d11
        dxgi
        ole32
        user32
        gdi32
        windowsapp
        avrt
        shcore
        wtsapi32
        ${PLATFORM_LIBRARIES})
target_compile_options(sunshine_wgc_capture PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

# Display settings helper: detached helper that applies/reverts display settings via IPC
add_executable(sunshine_display_helper
        display_settings_helper.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/pipes.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/misc_utils.cpp
        ${CMAKE_SOURCE_DIR}/src/logging.cpp)
set_target_properties(sunshine_display_helper PROPERTIES CXX_STANDARD 23)
target_compile_definitions(sunshine_display_helper PRIVATE SETUP_LIBDISPLAYDEVICE_LOGGING)
target_link_libraries(sunshine_display_helper
        ${Boost_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        libdisplaydevice::display_device
        user32
        wtsapi32
        advapi32
        secur32
        ole32
        taskschd
        ${PLATFORM_LIBRARIES})
target_compile_options(sunshine_display_helper PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
