if(UNIX)
    if(${SUNSHINE_CONFIGURE_HOMEBREW})
        configure_file(packaging/sunshine.rb sunshine.rb @ONLY)
    endif()
endif()

if(APPLE)
    if(${SUNSHINE_CONFIGURE_PORTFILE})
        configure_file(packaging/macos/Portfile Portfile @ONLY)
    endif()
elseif(UNIX)
    # configure the .desktop file
    set(SUNSHINE_DESKTOP_ICON "apollo.svg")
    if(${SUNSHINE_BUILD_APPIMAGE})
        configure_file(packaging/linux/AppImage/${PROJECT_FQDN}.desktop ${PROJECT_FQDN}.desktop @ONLY)
    elseif(${SUNSHINE_BUILD_FLATPAK})
        set(SUNSHINE_DESKTOP_ICON "${PROJECT_FQDN}")
        configure_file(packaging/linux/flatpak/${PROJECT_FQDN}.desktop ${PROJECT_FQDN}.desktop @ONLY)
    else()
        configure_file(packaging/linux/${PROJECT_FQDN}.desktop ${PROJECT_FQDN}.desktop @ONLY)
        configure_file(packaging/linux/${PROJECT_FQDN}.terminal.desktop ${PROJECT_FQDN}.terminal.desktop @ONLY)
    endif()

    # configure metadata file
    configure_file(packaging/linux/${PROJECT_FQDN}.metainfo.xml ${PROJECT_FQDN}.metainfo.xml @ONLY)

    # User-scoped portable services retain their startup delay. Native machine
    # hosts are installed as separate system units and reconciled by the
    # session controller without participating in the desktop startup path.
    set(SUNSHINE_SERVICE_READINESS_COMMAND "ExecStartPre=/bin/sleep 5")

    # configure service
    configure_file(packaging/linux/app-${PROJECT_FQDN}.service.in app-${PROJECT_FQDN}.service @ONLY)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT SUNSHINE_BUILD_STEAMOS)
        # These files are executed/read by root. They intentionally do not
        # follow a user-selectable CMAKE_INSTALL_PREFIX.
        set(VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR "/usr/libexec/vibeshine")
        set(VIBESHINE_DRM_SOURCE_INSTALL_DIR "/usr/src/vibeshine-drm-${PROJECT_VERSION_NUMERIC}")
        set(VIBESHINE_SYSTEM_UNIT_INSTALL_DIR "/usr/lib/systemd/system")
        set(VIBESHINE_SYSUSERS_INSTALL_DIR "/usr/lib/sysusers.d")
        if(NOT LIBVIRTUALDISPLAY_LINUX_ROOT OR NOT EXISTS "${LIBVIRTUALDISPLAY_LINUX_ROOT}/vibeshine-drm/Makefile")
            message(FATAL_ERROR "libvirtualdisplay Linux assets are unavailable")
        endif()
        file(GLOB VIBESHINE_DRM_HASH_INPUTS CONFIGURE_DEPENDS
                "${LIBVIRTUALDISPLAY_LINUX_ROOT}/vibeshine-drm/*.c"
                "${LIBVIRTUALDISPLAY_LINUX_ROOT}/vibeshine-drm/*.h")
        list(FILTER VIBESHINE_DRM_HASH_INPUTS EXCLUDE REGEX "\\.mod\\.c$")
        list(APPEND VIBESHINE_DRM_HASH_INPUTS
                "${LIBVIRTUALDISPLAY_LINUX_ROOT}/vibeshine-drm/Makefile"
                "${LIBVIRTUALDISPLAY_LINUX_ROOT}/vibeshine-drm/build-module"
                "${LIBVIRTUALDISPLAY_LINUX_ROOT}/vibeshine-drm/dkms.conf.in")
        list(SORT VIBESHINE_DRM_HASH_INPUTS)
        # The source identity is embedded in the configured root installer.
        # CONFIGURE_DEPENDS on the glob only notices files entering or leaving
        # it, not content changes to an existing driver source file.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                ${VIBESHINE_DRM_HASH_INPUTS})
        set(VIBESHINE_DRM_HASH_MATERIAL "")
        foreach(VIBESHINE_DRM_HASH_INPUT IN LISTS VIBESHINE_DRM_HASH_INPUTS)
            file(SHA256 "${VIBESHINE_DRM_HASH_INPUT}" VIBESHINE_DRM_INPUT_HASH)
            file(RELATIVE_PATH VIBESHINE_DRM_INPUT_NAME
                    "${LIBVIRTUALDISPLAY_LINUX_ROOT}/vibeshine-drm"
                    "${VIBESHINE_DRM_HASH_INPUT}")
            string(APPEND VIBESHINE_DRM_HASH_MATERIAL
                    "${VIBESHINE_DRM_INPUT_NAME}:${VIBESHINE_DRM_INPUT_HASH}\n")
        endforeach()
        string(APPEND VIBESHINE_DRM_HASH_MATERIAL
                "package-version:${PROJECT_VERSION_NUMERIC}\n")
        string(SHA256 VIBESHINE_DRM_SOURCE_ID "${VIBESHINE_DRM_HASH_MATERIAL}")
        # Privileged services that build and provision Vibepollo's virtual
        # display outputs before the display manager enumerates DRM devices.
        configure_file("${LIBVIRTUALDISPLAY_LINUX_ROOT}/packaging/vibeshine-vkms.service.in" vibeshine-vkms.service @ONLY)
        configure_file("${LIBVIRTUALDISPLAY_LINUX_ROOT}/packaging/vibeshine-vkms-control.socket.in" vibeshine-vkms-control.socket @ONLY)
        configure_file("${LIBVIRTUALDISPLAY_LINUX_ROOT}/packaging/vibeshine-vkms-control@.service.in" vibeshine-vkms-control@.service @ONLY)
        configure_file("${LIBVIRTUALDISPLAY_LINUX_ROOT}/packaging/vibeshine-drm-setup.service.in" vibeshine-drm-setup.service @ONLY)
        configure_file("${LIBVIRTUALDISPLAY_LINUX_ROOT}/packaging/vibeshine-drm-install.in" vibeshine-drm-install @ONLY)
        configure_file("${LIBVIRTUALDISPLAY_LINUX_ROOT}/vibeshine-drm/dkms.conf.in" vibeshine-drm-dkms.conf @ONLY)
        file(READ "${CMAKE_SOURCE_DIR}/src_assets/linux/misc/postinst" VIBESHINE_BASE_POSTINST)
        configure_file(packaging/linux/vibeshine-preinst.in preinst @ONLY)
        configure_file(packaging/linux/vibeshine-postinst.in postinst @ONLY)
        configure_file(packaging/linux/vibeshine-prerm.in prerm @ONLY)
        configure_file(packaging/linux/vibeshine-postrm.in postrm @ONLY)
    endif()

    # configure kwin desktop permission file
    if (${SUNSHINE_ENABLE_KWIN})
        configure_file(packaging/linux/${PROJECT_FQDN}.kwin.desktop.in ${PROJECT_FQDN}.kwin.desktop @ONLY)
    endif()

    # configure the arch linux pkgbuild
    if(${SUNSHINE_CONFIGURE_PKGBUILD})
        # Arch forbids hyphens in pkgver. Removing the SemVer prerelease
        # separator also keeps e.g. 1.19.0beta.4 ordered below 1.19.0.
        set(SUNSHINE_ARCH_PKGVER "${PROJECT_VERSION_FULL}")
        string(REPLACE "-" "" SUNSHINE_ARCH_PKGVER "${SUNSHINE_ARCH_PKGVER}")
        string(REPLACE "+" "." SUNSHINE_ARCH_PKGVER "${SUNSHINE_ARCH_PKGVER}")
        configure_file(packaging/linux/Arch/PKGBUILD PKGBUILD @ONLY)
        configure_file(packaging/linux/Arch/vibepollo.install vibepollo.install @ONLY)
    endif()

    # configure the flatpak manifest
    if(${SUNSHINE_CONFIGURE_FLATPAK_MAN})
        configure_file(packaging/linux/flatpak/${PROJECT_FQDN}.yml ${PROJECT_FQDN}.yml @ONLY)
        file(COPY packaging/linux/flatpak/deps/ DESTINATION ${CMAKE_BINARY_DIR})
        file(COPY packaging/linux/flatpak/modules DESTINATION ${CMAKE_BINARY_DIR})
    endif()
endif()

# return if configure only is set
if(${SUNSHINE_CONFIGURE_ONLY})
    # message
    message(STATUS "SUNSHINE_CONFIGURE_ONLY: ON, exiting...")
    set(END_BUILD ON)
else()
    set(END_BUILD OFF)
endif()
