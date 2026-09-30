# PyroWave: the vendored upstream codec and its C API, built as a static library.
# See third-party/pyrowave/CMakeLists.txt and docs/pyrowave-protocol.md.

# The bounded hybrid transport and fast LZ4 codec have no GPU dependency. Build
# them on all platforms so video.cpp remains valid with PyroWave disabled; the
# host advertises the mode only after the platform's PyroWave probe succeeds.
add_library(sunshine_pyrowave_hybrid STATIC
        "${CMAKE_SOURCE_DIR}/src/pyrowave_hybrid/pyrowavehybrid.cpp"
        "${CMAKE_SOURCE_DIR}/third-party/lz4/lz4.c")
set_target_properties(sunshine_pyrowave_hybrid PROPERTIES POSITION_INDEPENDENT_CODE ON)
list(APPEND SUNSHINE_EXTERNAL_LIBRARIES sunshine_pyrowave_hybrid)

if(SUNSHINE_ENABLE_PYROWAVE)
    add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/pyrowave" "${CMAKE_BINARY_DIR}/third-party/pyrowave")
    list(APPEND SUNSHINE_EXTERNAL_LIBRARIES pyrowave::capi)
    list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_ENABLE_PYROWAVE=1)
endif()
