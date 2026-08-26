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

    # configure service
    configure_file(packaging/linux/app-${PROJECT_FQDN}.service.in app-${PROJECT_FQDN}.service @ONLY)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # These files are executed/read by root. They intentionally do not
        # follow a user-selectable CMAKE_INSTALL_PREFIX.
        set(VIBEPOLLO_PRIVILEGED_LIBEXEC_INSTALL_DIR "/usr/libexec/vibepollo")
        set(VIBEPOLLO_DRM_SOURCE_INSTALL_DIR "/usr/src/vibepollo-drm-${PROJECT_VERSION_NUMERIC}")
        set(VIBEPOLLO_SYSTEM_UNIT_INSTALL_DIR "/usr/lib/systemd/system")
        file(GLOB VIBEPOLLO_DRM_HASH_INPUTS CONFIGURE_DEPENDS
                "${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo-drm/*.c"
                "${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo-drm/*.h")
        list(APPEND VIBEPOLLO_DRM_HASH_INPUTS
                "${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo-drm/Makefile"
                "${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo-drm/build-module"
                "${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo-drm/dkms.conf.in")
        list(SORT VIBEPOLLO_DRM_HASH_INPUTS)
        set(VIBEPOLLO_DRM_HASH_MATERIAL "")
        foreach(VIBEPOLLO_DRM_HASH_INPUT IN LISTS VIBEPOLLO_DRM_HASH_INPUTS)
            file(SHA256 "${VIBEPOLLO_DRM_HASH_INPUT}" VIBEPOLLO_DRM_INPUT_HASH)
            file(RELATIVE_PATH VIBEPOLLO_DRM_INPUT_NAME
                    "${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo-drm"
                    "${VIBEPOLLO_DRM_HASH_INPUT}")
            string(APPEND VIBEPOLLO_DRM_HASH_MATERIAL
                    "${VIBEPOLLO_DRM_INPUT_NAME}:${VIBEPOLLO_DRM_INPUT_HASH}\n")
        endforeach()
        string(SHA256 VIBEPOLLO_DRM_SOURCE_ID "${VIBEPOLLO_DRM_HASH_MATERIAL}")
        # Privileged services that build and provision Vibepollo's virtual
        # display outputs before the display manager enumerates DRM devices.
        configure_file(packaging/linux/vibepollo-vkms.service.in vibepollo-vkms.service @ONLY)
        configure_file(packaging/linux/vibepollo-vkms-control.socket.in vibepollo-vkms-control.socket @ONLY)
        configure_file(packaging/linux/vibepollo-vkms-control@.service.in vibepollo-vkms-control@.service @ONLY)
        configure_file(packaging/linux/vibepollo-drm-setup.service.in vibepollo-drm-setup.service @ONLY)
        configure_file(packaging/linux/vibepollo-drm-install.in vibepollo-drm-install @ONLY)
        configure_file(packaging/linux/vibepollo-drm/dkms.conf.in vibepollo-drm-dkms.conf @ONLY)
        file(READ "${CMAKE_SOURCE_DIR}/src_assets/linux/misc/postinst" VIBEPOLLO_BASE_POSTINST)
        configure_file(packaging/linux/vibepollo-postinst.in postinst @ONLY)
        configure_file(packaging/linux/vibepollo-prerm.in prerm @ONLY)
    endif()

    # configure kwin desktop permission file
    if (${SUNSHINE_ENABLE_KWIN})
        configure_file(packaging/linux/${PROJECT_FQDN}.kwin.desktop.in ${PROJECT_FQDN}.kwin.desktop @ONLY)
    endif()

    # configure the arch linux pkgbuild
    if(${SUNSHINE_CONFIGURE_PKGBUILD})
        configure_file(packaging/linux/Arch/PKGBUILD PKGBUILD @ONLY)
        configure_file(packaging/linux/Arch/sunshine.install sunshine.install @ONLY)
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
