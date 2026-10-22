# The native C API needs no SDK headers or Python at runtime. Retain the pinned
# wheel's layout/SONAMEs, licenses and provenance in the immutable library tree.
option(SUNSHINE_ENABLE_NV_TRUEHDR "Enable native NVIDIA TrueHDR on Linux" ON)
if(SUNSHINE_ENABLE_NV_TRUEHDR AND CUDA_FOUND AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(SUNSHINE_TRUEHDR_LINUX_ROOT "${CMAKE_BINARY_DIR}/truehdr-runtime" CACHE PATH "Extracted pinned NVIDIA VFX runtime")
    if(NOT EXISTS "${SUNSHINE_TRUEHDR_LINUX_ROOT}/runtime-provenance.txt")
        execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/linux_truehdr/fetch_runtime.py"
                        "${SUNSHINE_TRUEHDR_LINUX_ROOT}" COMMAND_ERROR_IS_FATAL ANY)
    endif()
    file(READ "${SUNSHINE_TRUEHDR_LINUX_ROOT}/runtime-provenance.txt" _truehdr_provenance)
    if(NOT _truehdr_provenance MATCHES "sha256=0252866a7b798a6f92c8543443d88f716e8c5bbc6a7ddd77a2754728dada9fa8")
        message(FATAL_ERROR "NVIDIA TrueHDR runtime provenance does not match the pinned wheel")
    endif()
    foreach(_truehdr_library IN ITEMS libnppc.so.12 libnppial.so.12 libnppicc.so.12
            libnppidei.so.12 libnppif.so.12 libnppig.so.12 libnppim.so.12 libnppist.so.12
            libnppitc.so.12 libcudnn.so.9 libNVCVImage.so libnvngxruntime.so
            libVideoFXLocal.so libVideoFX.so libnvVFXTrueHDR.so libnvidia-ngx-truehdr.so.1.2.3)
        if(NOT EXISTS "${SUNSHINE_TRUEHDR_LINUX_ROOT}/nvvfx/libs/${_truehdr_library}")
            message(FATAL_ERROR "Pinned TrueHDR runtime is missing ${_truehdr_library}")
        endif()
    endforeach()
    add_library(linux-truehdr STATIC "${CMAKE_SOURCE_DIR}/src/platform/linux/truehdr.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/truehdr_gl.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/truehdr_peak.cu")
    target_compile_features(linux-truehdr PUBLIC cxx_std_20)
    target_link_libraries(linux-truehdr PRIVATE glad ${CMAKE_DL_LIBS})
    list(APPEND PLATFORM_LIBRARIES linux-truehdr)
    list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_ENABLE_NV_TRUEHDR=1
        SUNSHINE_TRUEHDR_RUNTIME_DIR="${CMAKE_INSTALL_FULL_LIBDIR}/vibeshine/truehdr")
    install(DIRECTORY "${SUNSHINE_TRUEHDR_LINUX_ROOT}/nvvfx/libs/"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/vibeshine/truehdr/nvvfx/libs"
        PATTERN "libnvVFXVideoFrameGeneration.so" EXCLUDE
        PATTERN "libnvVFXVideoSuperRes.so" EXCLUDE
        PATTERN "libnvidia-ngx-vsr.so*" EXCLUDE)
    install(DIRECTORY "${SUNSHINE_TRUEHDR_LINUX_ROOT}/nvidia_vfx.libs"
        "${SUNSHINE_TRUEHDR_LINUX_ROOT}/nvidia_vfx-0.2.0.0.dist-info/licenses"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/vibeshine/truehdr")
    install(FILES "${SUNSHINE_TRUEHDR_LINUX_ROOT}/runtime-provenance.txt"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/vibeshine/truehdr")
endif()
