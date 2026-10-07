# PyroWave: the vendored upstream codec and its C API, built as a static library.
# See third-party/pyrowave/CMakeLists.txt and docs/pyrowave-protocol.md.

if(SUNSHINE_ENABLE_PYROWAVE)
    add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/pyrowave" "${CMAKE_BINARY_DIR}/third-party/pyrowave")
    list(APPEND SUNSHINE_EXTERNAL_LIBRARIES pyrowave::capi)
    list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_ENABLE_PYROWAVE=1)
endif()
