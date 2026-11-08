include("${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/packaging/windows_virtual_gamepad_contract.cmake")

# Exercise the real stage selector with disposable scripts. A test package
# refresh may replace its build-tree stage but must not touch release assets.
set(_root "${CMAKE_CURRENT_BINARY_DIR}/build/vhf-stage-contract")
set(_source "${_root}/release-assets")
set(_build "${_root}/isolated-build")
file(MAKE_DIRECTORY "${_source}" "${_build}")
file(WRITE "${_source}/install.ps1" "original install")
file(WRITE "${_source}/cleanup.ps1" "original cleanup")

sunshine_vhf_prepare_package_stage(_production OFF "${_source}" "${_build}")
if(NOT _production STREQUAL _source)
    message(FATAL_ERROR "Production staging path changed: ${_production}")
endif()

sunshine_vhf_prepare_package_stage(_local ON "${_source}" "${_build}")
if(NOT _local STREQUAL "${_build}/local-vhf-gamepad")
    message(FATAL_ERROR "Local package did not use the isolated build tree: ${_local}")
endif()
file(READ "${_local}/install.ps1" _install)
file(READ "${_local}/cleanup.ps1" _cleanup)
if(NOT _install STREQUAL "original install" OR NOT _cleanup STREQUAL "original cleanup")
    message(FATAL_ERROR "Local stage did not receive the release installer scripts")
endif()

file(WRITE "${_local}/install.ps1" "local change")
file(READ "${_source}/install.ps1" _original)
if(NOT _original STREQUAL "original install")
    message(FATAL_ERROR "Local staging changed the release asset")
endif()
message(STATUS "VHF local and production package staging paths passed")
