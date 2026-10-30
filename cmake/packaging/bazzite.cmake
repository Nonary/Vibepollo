# Experimental Fedora Atomic package metadata. Runtime provisioning is separate.
option(SUNSHINE_BUILD_BAZZITE "Build an experimental rpm-ostree package (not a live-host installer)" OFF)
if(NOT SUNSHINE_BUILD_BAZZITE)
    return()
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR SUNSHINE_BUILD_STEAMOS OR
        SUNSHINE_BUILD_APPIMAGE OR SUNSHINE_BUILD_FLATPAK)
    message(FATAL_ERROR "The Bazzite profile requires a native Linux RPM build")
endif()
# rpm-ostree composes a deployment offline. The mutable-distro scriptlets
# stop/start services, chmod/setcap /usr and invoke DKMS; none may run here.
# Fedora's RPM >=4.20 handles the shipped sysusers.d declarations natively.
foreach(_hook PRE_INSTALL POST_INSTALL PRE_UNINSTALL POST_UNINSTALL)
    unset(CPACK_RPM_${_hook}_SCRIPT_FILE)
endforeach()
unset(_hook)
set(CPACK_GENERATOR RPM)
set(CPACK_RPM_PACKAGE_RELEASE "3.bazzite")
string(APPEND CPACK_RPM_PACKAGE_REQUIRES ", rpm >= 4.20, /usr/bin/systemd-sysusers, vibepollo-kmod")
# The native KWin bridge is a trusted preload library. Preserve the same
# payload mode as the Arch/spec packages without chmod in a scriptlet.
list(APPEND CPACK_RPM_USER_FILELIST
    "%attr(4755,root,root) ${CMAKE_INSTALL_FULL_LIBDIR}/libvibeshine-kwin-gpu.so")

# auditwheel renamed the pinned CUDA runtime's SONAME, while its version
# definition still names libcudart.so.12. RPM discovers the unversioned
# hashed provide and the original versioned provide, but consumers require
# this combination. The private runtime is installed with the TrueHDR bundle.
if(TARGET linux-truehdr)
    set(_cuda_provide "libcudart-256e6409.so.12.9.79(libcudart.so.12)(64bit)")
    if(CPACK_RPM_PACKAGE_PROVIDES)
        string(APPEND CPACK_RPM_PACKAGE_PROVIDES ", ${_cuda_provide}")
    else()
        set(CPACK_RPM_PACKAGE_PROVIDES "${_cuda_provide}")
    endif()
    unset(_cuda_provide)
endif()
