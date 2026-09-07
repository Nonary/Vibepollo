# linux specific packaging

# CPack's common numeric version is appropriate for MSI, but native Linux
# package managers must retain the channel/build identity.  Tilde orders real
# prereleases below the final release; the project's stable.N respins order
# above the matching stable release.
set(VIBESHINE_NATIVE_PACKAGE_VERSION "${PROJECT_VERSION_FULL}")
if(VIBESHINE_NATIVE_PACKAGE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+-stable([.].+)?$")
    string(REPLACE "-stable" "+stable" VIBESHINE_NATIVE_PACKAGE_VERSION
            "${VIBESHINE_NATIVE_PACKAGE_VERSION}")
elseif(VIBESHINE_NATIVE_PACKAGE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+-")
    string(REGEX REPLACE "^([0-9]+\\.[0-9]+\\.[0-9]+)-" "\\1~"
            VIBESHINE_NATIVE_PACKAGE_VERSION "${VIBESHINE_NATIVE_PACKAGE_VERSION}")
endif()
set(CPACK_DEBIAN_PACKAGE_VERSION "${VIBESHINE_NATIVE_PACKAGE_VERSION}")
set(CPACK_RPM_PACKAGE_VERSION "${VIBESHINE_NATIVE_PACKAGE_VERSION}")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" VIBESHINE_PACKAGE_PROCESSOR)
    if(VIBESHINE_PACKAGE_PROCESSOR MATCHES "^(x86_64|amd64)$")
        set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
    elseif(VIBESHINE_PACKAGE_PROCESSOR MATCHES "^(aarch64|arm64)$")
        set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "arm64")
    endif()
endif()

install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/linux/assets/"
        DESTINATION "${SUNSHINE_ASSETS_DIR}")

# copy assets (excluding shaders) to build directory, for running without install
file(COPY "${SUNSHINE_SOURCE_ASSETS_DIR}/linux/assets/"
        DESTINATION "${CMAKE_BINARY_DIR}/assets"
        PATTERN "shaders" EXCLUDE)
# use symbolic link for shaders directory
file(CREATE_LINK "${SUNSHINE_SOURCE_ASSETS_DIR}/linux/assets/shaders"
        "${CMAKE_BINARY_DIR}/assets/shaders" COPY_ON_ERROR SYMBOLIC)

install(PROGRAMS "${SUNSHINE_SOURCE_ASSETS_DIR}/linux/misc/vibepollo-mangohud"
        DESTINATION "${CMAKE_INSTALL_BINDIR}")

if(SUNSHINE_BUILD_STEAMOS)
    # Transitive application dependencies also resolve from the bundle. Keep
    # the OS loader, C library and graphics/session drivers owned by SteamOS.
    set_target_properties(sunshine PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
    target_link_options(sunshine PRIVATE "LINKER:--disable-new-dtags")
    install(CODE [[
        file(GET_RUNTIME_DEPENDENCIES
            EXECUTABLES "$<TARGET_FILE:sunshine>"
            RESOLVED_DEPENDENCIES_VAR _steamos_libraries
            UNRESOLVED_DEPENDENCIES_VAR _steamos_missing
            PRE_EXCLUDE_REGEXES
                "^linux-vdso" "^ld-linux"
                "^lib(c|m|mvec|dl|pthread|rt|resolv|util|nss_[^.]+)\\.so"
                "^lib(gio|glib|gmodule|gobject|gthread)-2\\.0\\.so"
                "^lib(EGL|GL|GLX|GLdispatch|OpenGL|GLES[^.]*|glapi|gbm|drm[^.]*|vulkan|va[^.]*|wayland[^.]*|pipewire[^/]*|pulse[^/]*)\\.so")
        if(_steamos_missing)
            message(FATAL_ERROR "Unresolved SteamOS runtime dependencies: ${_steamos_missing}")
        endif()
        foreach(_steamos_library IN LISTS _steamos_libraries)
            file(INSTALL "${_steamos_library}" DESTINATION "${CMAKE_INSTALL_PREFIX}/lib"
                TYPE SHARED_LIBRARY FOLLOW_SYMLINK_CHAIN)
        endforeach()
    ]])
    install(PROGRAMS
            "${CMAKE_SOURCE_DIR}/packaging/linux/steamos/install-user.sh"
            "${CMAKE_SOURCE_DIR}/packaging/linux/steamos/uninstall-user.sh"
            "${CMAKE_SOURCE_DIR}/packaging/linux/steamos/check-host.sh"
            "${CMAKE_SOURCE_DIR}/packaging/linux/steamos/vibepollo-steamos-session"
            DESTINATION "share/vibepollo/steamos")
    install(FILES
            "${CMAKE_SOURCE_DIR}/packaging/linux/steamos/vibepollo-steamos.service"
            "${CMAKE_SOURCE_DIR}/packaging/linux/steamos/README.md"
            "${CMAKE_SOURCE_DIR}/packaging/linux/steamos/AUDIT.md"
            DESTINATION "share/vibepollo/steamos")
    install(FILES "${CMAKE_SOURCE_DIR}/LICENSE" "${CMAKE_SOURCE_DIR}/NOTICE"
            DESTINATION "share/licenses/vibepollo")
    install(DIRECTORY
            "${CMAKE_SOURCE_DIR}/packaging/linux/steamos/gamescope"
            "${CMAKE_SOURCE_DIR}/packaging/linux/steamos/local"
            "${CMAKE_SOURCE_DIR}/packaging/linux/steamos/sysext"
            DESTINATION "share/vibepollo/steamos"
            PATTERN "__pycache__" EXCLUDE)
    set(CPACK_GENERATOR "TGZ")
    set(CPACK_PACKAGE_FILE_NAME "Vibepollo-SteamOS-${PROJECT_VERSION_FULL}-${CMAKE_SYSTEM_PROCESSOR}")
    # No machine services, udev rules, kernel modules or native package hooks.
    return()
endif()

if(${SUNSHINE_BUILD_APPIMAGE} OR ${SUNSHINE_BUILD_FLATPAK})
    install(FILES "${SUNSHINE_SOURCE_ASSETS_DIR}/linux/misc/60-sunshine.rules"
            DESTINATION "${SUNSHINE_ASSETS_DIR}/udev/rules.d")
    install(FILES "${SUNSHINE_SOURCE_ASSETS_DIR}/linux/misc/60-sunshine.conf"
            DESTINATION "${SUNSHINE_ASSETS_DIR}/modules-load.d")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/app-${PROJECT_FQDN}.service"
            DESTINATION "${SUNSHINE_ASSETS_DIR}/systemd/user")
else()
    find_package(Systemd)
    find_package(Udev)

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(vibepollo_session_exec
                "${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo-session-exec.c")
        set_target_properties(vibepollo_session_exec PROPERTIES OUTPUT_NAME "vibepollo-session-exec")
        target_include_directories(vibepollo_session_exec PRIVATE "${LIBCAP_INCLUDE_DIRS}")
        target_link_libraries(vibepollo_session_exec PRIVATE "${LIBCAP_LIBRARIES}")
        add_executable(vibepollo_session_broker
                "${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo-session-broker.c")
        set_target_properties(vibepollo_session_broker PROPERTIES OUTPUT_NAME "vibepollo-session-broker")
        target_include_directories(vibepollo_session_broker PRIVATE "${LIBCAP_INCLUDE_DIRS}")
        target_link_libraries(vibepollo_session_broker PRIVATE "${LIBCAP_LIBRARIES}")
        add_executable(vibepollo_display_power
                "${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo-display-power.c")
        set_target_properties(vibepollo_display_power PROPERTIES OUTPUT_NAME "vibepollo-display-power")
        target_include_directories(vibepollo_display_power PRIVATE ${GIO_INCLUDE_DIRS})
        target_link_libraries(vibepollo_display_power PRIVATE ${GIO_LIBRARIES})
        add_executable(vibepollo_app_supervisor
                "${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo-app-supervisor.c")
        set_target_properties(vibepollo_app_supervisor PROPERTIES OUTPUT_NAME "vibepollo-app-supervisor")
        target_include_directories(vibepollo_app_supervisor PRIVATE "${LIBCAP_INCLUDE_DIRS}")
        target_link_libraries(vibepollo_app_supervisor PRIVATE "${LIBCAP_LIBRARIES}")
        add_executable(vibepollo_profile_import
                "${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo-profile-import.c")
        set_target_properties(vibepollo_profile_import PROPERTIES OUTPUT_NAME "vibepollo-profile-import")
        target_include_directories(vibepollo_profile_import PRIVATE "${LIBCAP_INCLUDE_DIRS}")
        target_link_libraries(vibepollo_profile_import PRIVATE "${LIBCAP_LIBRARIES}")
        add_executable(vibepollo_kwin_session_environment
                "${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo-kwin-session-environment.c")
        set_target_properties(vibepollo_kwin_session_environment PROPERTIES
                OUTPUT_NAME "vibepollo-kwin-session-environment")
        add_executable(vibepollo_provider_scan
                "${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo-provider-scan.cpp"
                "${CMAKE_SOURCE_DIR}/src/provider_scan_protocol.cpp"
                "${CMAKE_SOURCE_DIR}/src/steam_integration.cpp"
                "${CMAKE_SOURCE_DIR}/src/lutris_integration.cpp"
                "${CMAKE_SOURCE_DIR}/src/steam_artwork.cpp")
        target_include_directories(vibepollo_provider_scan PRIVATE ${FFMPEG_INCLUDE_DIRS})
        target_compile_definitions(vibepollo_provider_scan PRIVATE ${STEAM_ARTWORK_TEST_DEFINITIONS})
        target_link_libraries(vibepollo_provider_scan PRIVATE ${FFMPEG_LIBRARIES} ${STEAM_ARTWORK_TEST_LIBRARIES})
        set_target_properties(vibepollo_provider_scan PROPERTIES OUTPUT_NAME "vibepollo-provider-scan")
        target_include_directories(vibepollo_provider_scan PRIVATE
                "${CMAKE_SOURCE_DIR}"
                "${SQLITE3_INCLUDE_DIRS}")
        target_link_libraries(vibepollo_provider_scan PRIVATE
                nlohmann_json::nlohmann_json
                "${SQLITE3_LIBRARIES}")
        add_executable(vibepollo_steam_launch
                "${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo-steam-launch.cpp"
                "${CMAKE_SOURCE_DIR}/src/provider_scan_protocol.cpp"
                "${CMAKE_SOURCE_DIR}/src/steam_integration.cpp")
        set_target_properties(vibepollo_steam_launch PROPERTIES
                OUTPUT_NAME "vibepollo-steam-launch")
        target_include_directories(vibepollo_steam_launch PRIVATE
                "${CMAKE_SOURCE_DIR}")
        target_link_libraries(vibepollo_steam_launch PRIVATE
                nlohmann_json::nlohmann_json)
        file(GENERATE
                OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vibeshine_drm_version.h"
                CONTENT "#define VIBESHINE_DRM_VERSION \"${PROJECT_VERSION_NUMERIC}\"\n")

        install(PROGRAMS
                "${LIBVIRTUALDISPLAY_LINUX_ROOT}/packaging/vibeshine-vkms"
                "${LIBVIRTUALDISPLAY_LINUX_ROOT}/packaging/vibeshine-vkms-quiesce"
                "${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo-machine-host"
                "${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo-profile-normalize.py"
                "${CMAKE_SOURCE_DIR}/packaging/linux/steamos/local/pairing_migration.py"
                "${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo-session-controller"
                "${CMAKE_CURRENT_BINARY_DIR}/vibeshine-drm-install"
                DESTINATION "${VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR}")
        install(TARGETS vibepollo_session_exec vibepollo_app_supervisor
                vibepollo_profile_import vibepollo_kwin_session_environment
                vibepollo_provider_scan vibepollo_steam_launch vibepollo_display_power
                RUNTIME DESTINATION "${VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR}")
        install(TARGETS vibepollo_session_broker
                RUNTIME DESTINATION "${VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR}"
                PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
        # This must be a distinct inode from the public capability-free binary.
        # Native package hooks make it root:vibepollo 0750 and attach only
        # cap_sys_admin,cap_sys_nice+p.  With no effective file bit, its loader
        # and the first statement in main() run with E/I/A empty.
        install(PROGRAMS "$<TARGET_FILE:sunshine>"
                DESTINATION "${VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR}"
                RENAME "vibepollo-host"
                PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE)
        install(TARGETS vibeshine_vkms_peercred
                RUNTIME DESTINATION "${VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR}")
        install(FILES "${LIBVIRTUALDISPLAY_LINUX_ROOT}/packaging/vibeshine-vkms.sysusers"
                DESTINATION "${VIBESHINE_SYSUSERS_INSTALL_DIR}"
                RENAME vibeshine-vkms.conf)
        install(FILES "${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo.sysusers"
                DESTINATION "${VIBESHINE_SYSUSERS_INSTALL_DIR}"
                RENAME vibepollo.conf)
        install(FILES "${CMAKE_SOURCE_DIR}/packaging/linux/prelogin/apps.json"
                DESTINATION "${SUNSHINE_ASSETS_DIR}/prelogin")
        install(DIRECTORY "${LIBVIRTUALDISPLAY_LINUX_ROOT}/vibeshine-drm/"
                DESTINATION "${VIBESHINE_DRM_SOURCE_INSTALL_DIR}"
                FILES_MATCHING
                PATTERN "*.c"
                PATTERN "*.mod.c" EXCLUDE
                PATTERN "*.h"
                PATTERN "*.py"
                PATTERN "Makefile"
                PATTERN "README*"
                PATTERN "LICENSE*")
        install(FILES "${CMAKE_CURRENT_BINARY_DIR}/vibeshine_drm_version.h"
                DESTINATION "${VIBESHINE_DRM_SOURCE_INSTALL_DIR}")
        install(PROGRAMS "${LIBVIRTUALDISPLAY_LINUX_ROOT}/vibeshine-drm/build-module"
                DESTINATION "${VIBESHINE_DRM_SOURCE_INSTALL_DIR}")
        install(FILES "${CMAKE_CURRENT_BINARY_DIR}/vibeshine-drm-dkms.conf"
                DESTINATION "${VIBESHINE_DRM_SOURCE_INSTALL_DIR}"
                RENAME dkms.conf)
    endif()

    if(UDEV_FOUND)
        install(FILES "${SUNSHINE_SOURCE_ASSETS_DIR}/linux/misc/60-sunshine.rules"
                DESTINATION "${UDEV_RULES_INSTALL_DIR}")
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            install(FILES "${CMAKE_SOURCE_DIR}/packaging/linux/70-vibepollo-uinput.rules"
                    DESTINATION "${UDEV_RULES_INSTALL_DIR}")
        endif()
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Firewall service definitions and the PipeWire quantum the stream expects.
        install(FILES "${CMAKE_SOURCE_DIR}/packaging/linux/firewalld/vibepollo.xml"
                DESTINATION "lib/firewalld/services")
        install(FILES "${CMAKE_SOURCE_DIR}/packaging/linux/ufw/vibepollo"
                DESTINATION "${CMAKE_INSTALL_FULL_SYSCONFDIR}/ufw/applications.d")
        install(FILES "${CMAKE_SOURCE_DIR}/packaging/linux/pipewire/50-vibepollo-audio.conf"
                DESTINATION "${CMAKE_INSTALL_DATADIR}/pipewire/pipewire.conf.d")
    endif()
    if(SYSTEMD_FOUND)
        if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
            install(FILES "${CMAKE_CURRENT_BINARY_DIR}/app-${PROJECT_FQDN}.service"
                    DESTINATION "${SYSTEMD_USER_UNIT_INSTALL_DIR}")
        endif()
        install(FILES "${SUNSHINE_SOURCE_ASSETS_DIR}/linux/misc/60-sunshine.conf"
                DESTINATION "${SYSTEMD_MODULES_LOAD_DIR}")
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            install(FILES
                    "${CMAKE_CURRENT_BINARY_DIR}/vibeshine-vkms.service"
                    "${CMAKE_CURRENT_BINARY_DIR}/vibeshine-drm-setup.service"
                    "${CMAKE_CURRENT_BINARY_DIR}/vibeshine-vkms-control.socket"
                    "${CMAKE_CURRENT_BINARY_DIR}/vibeshine-vkms-control@.service"
                    "${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo-session-exec.socket"
                    "${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo-session-exec@.service"
                    "${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo-session-controller.service"
                    "${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo.service"
                    DESTINATION "${VIBESHINE_SYSTEM_UNIT_INSTALL_DIR}")
            # Both the Plasma desktop and Plasma Login greeter start their own
            # KWin instance.  Publish each compositor's generated Wayland/X11
            # credentials into its corresponding user manager so the machine
            # controller can validate either authoritative seat0 session.
            foreach(vibeshine_kwin_unit IN ITEMS
                    plasma-kwin_wayland
                    plasma-login-kwin_wayland)
                install(FILES
                        "${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo-kwin-session-environment.conf"
                        DESTINATION
                        "${SYSTEMD_USER_UNIT_INSTALL_DIR}/${vibeshine_kwin_unit}.service.d")
            endforeach()
        endif()
    endif()
endif()

# RPM specific
set(CPACK_RPM_PACKAGE_LICENSE "GPLv3")
set(CPACK_RPM_PACKAGE_CONFLICTS "Sunshine, sunshine, vibeshine")

# Native hosts share the same ports, input integration, and virtual-display
# driver. Let package managers replace either legacy host without deleting
# users' configuration or pairing data.
set(CPACK_DEBIAN_PACKAGE_CONFLICTS "sunshine, vibeshine")

# FreeBSD specific
set(CPACK_FREEBSD_PACKAGE_MAINTAINER "${CPACK_PACKAGE_VENDOR}")
set(CPACK_FREEBSD_PACKAGE_ORIGIN "misc/${CPACK_PACKAGE_NAME}")
set(CPACK_FREEBSD_PACKAGE_LICENSE "GPLv3")

# Native package lifecycle hooks build the managed-display module on a
# best-effort basis. The system service retries the custom module on boot.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
            "${CMAKE_CURRENT_BINARY_DIR}/preinst;${CMAKE_CURRENT_BINARY_DIR}/postinst;${CMAKE_CURRENT_BINARY_DIR}/prerm;${CMAKE_CURRENT_BINARY_DIR}/postrm")
    set(CPACK_RPM_PRE_INSTALL_SCRIPT_FILE "${CMAKE_CURRENT_BINARY_DIR}/preinst")
    set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE "${CMAKE_CURRENT_BINARY_DIR}/postinst")
    set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE "${CMAKE_CURRENT_BINARY_DIR}/prerm")
else()
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${SUNSHINE_SOURCE_ASSETS_DIR}/linux/misc/postinst")
    set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE "${SUNSHINE_SOURCE_ASSETS_DIR}/linux/misc/postinst")
endif()

# Encode the exact privileged-file ownership and permitted capabilities in the
# RPM payload itself. This is required on rpm-ostree systems where lifecycle
# scripts deliberately do not mutate immutable deployment files. Public and
# capability-free helpers are listed explicitly so stale package metadata
# cannot silently reattach the obsolete public/client capabilities.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CPACK_RPM_USER_FILELIST
            "%attr(0755,root,root) ${VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR}/vibepollo-display-power"
            "%attr(0755,root,root) ${CMAKE_INSTALL_FULL_BINDIR}/vibepollo"
            "%attr(0755,root,root) ${VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR}/vibepollo-session-exec"
            "%attr(0700,root,root) %caps(cap_kill,cap_setgid,cap_setuid+p) ${VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR}/vibepollo-session-broker"
            "%attr(0755,root,root) ${VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR}/vibepollo-app-supervisor"
            "%attr(0755,root,root) ${VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR}/vibepollo-steam-launch"
            "%attr(0755,root,root) ${VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR}/vibepollo-kwin-session-environment"
            "%attr(0750,root,vibepollo) %caps(cap_sys_admin,cap_sys_nice+p) ${VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR}/vibepollo-host"
    )
endif()

# FreeBSD post install/deinstall scripts
if(FREEBSD)
    # Note: CPack's FreeBSD generator does NOT natively support install/deinstall scripts
    # like CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA or CPACK_RPM_POST_INSTALL_SCRIPT_FILE.
    # This is a known limitation of the CPack FREEBSD generator.
    #
    # Workaround: Use CPACK_POST_BUILD_SCRIPTS to extract the generated .pkg file,
    # add the install/deinstall scripts, and repack the package. This ensures they are
    # recognized as package control scripts rather than installed files.
    set(CPACK_FREEBSD_PACKAGE_SCRIPTS
        "${SUNSHINE_SOURCE_ASSETS_DIR}/bsd/misc/+POST_INSTALL"
        "${SUNSHINE_SOURCE_ASSETS_DIR}/bsd/misc/+PRE_DEINSTALL"
    )
    list(APPEND CPACK_POST_BUILD_SCRIPTS "${CMAKE_MODULE_PATH}/packaging/freebsd_custom_cpack.cmake")
endif()

# Dependencies
# Native machine-service installation is atomic. Splitting the `assets`
# component from the default component would produce an API-only host package,
# and CPack would attach the same quiescing lifecycle hooks to both partial
# packages. Emit one DEB containing the executable, Web UI, units, helpers,
# drivers, and assets together.
set(CPACK_DEB_COMPONENT_INSTALL OFF)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "\
            ${CPACK_DEB_PLATFORM_PACKAGE_DEPENDS} \
            debianutils, \
            libcap2, \
            libcap2-bin, \
            libcurl4, \
            libdrm2, \
            libgbm1, \
            libevdev2, \
            iproute2, \
            jq, \
            kmod, \
            libkscreen-bin | libkf5screen-bin, \
            make, \
            libnuma1, \
            libopus0, \
            libpulse0, \
            pulseaudio-utils, \
            python3, \
            libva2, \
            libva-drm2, \
            libwayland-client0, \
            libx11-6, \
            miniupnpc, \
            openssl | libssl3, \
            socat, \
            util-linux, \
            wayland-utils, \
            x11-utils")
set(CPACK_RPM_PACKAGE_REQUIRES "\
            ${CPACK_RPM_PLATFORM_PACKAGE_REQUIRES} \
            /usr/bin/pactl, \
            /usr/bin/parec, \
            /usr/bin/wayland-info, \
            /usr/bin/xdpyinfo, \
            libcap >= 2.22, \
            libcurl >= 7.0, \
            libdrm >= 2.4.97, \
            libevdev >= 1.5.6, \
            iproute, \
            jq, \
            kmod, \
            libkscreen, \
            make, \
            libopusenc >= 0.2.1, \
            libva >= 2.14.0, \
            libwayland-client >= 1.20.0, \
            libX11 >= 1.7.3.1, \
            mesa-libgbm >= 25.0.7, \
            miniupnpc >= 2.2.4, \
            numactl-libs >= 2.0.14, \
            openssl >= 3.0.2, \
            pulseaudio-libs >= 10.0, \
            python3, \
            socat, \
            util-linux")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "dkms")
set(CPACK_RPM_PACKAGE_SUGGESTS "dkms, gcc, kernel-devel")
list(APPEND CPACK_FREEBSD_PACKAGE_DEPS
        audio/opus
        ftp/curl
        devel/libevdev
        multimedia/pipewire
        net/avahi
        net/miniupnpc
        security/openssl
        x11/libX11
)

if(NOT BOOST_USE_STATIC)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "\
                ${CPACK_DEBIAN_PACKAGE_DEPENDS}, \
                libboost-filesystem${Boost_VERSION}, \
                libboost-locale${Boost_VERSION}, \
                libboost-log${Boost_VERSION}, \
                libboost-program-options${Boost_VERSION}")
    set(CPACK_RPM_PACKAGE_REQUIRES "\
                ${CPACK_RPM_PACKAGE_REQUIRES}, \
                boost-filesystem >= ${Boost_VERSION}, \
                boost-locale >= ${Boost_VERSION}, \
                boost-log >= ${Boost_VERSION}, \
                boost-program-options >= ${Boost_VERSION}")
    list(APPEND CPACK_FREEBSD_PACKAGE_DEPS
            devel/boost-libs
    )
endif()

# This should automatically figure out dependencies on packages
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_RPM_PACKAGE_AUTOREQ ON)

# application icon
if(NOT ${SUNSHINE_BUILD_FLATPAK})
    install(FILES "${CMAKE_SOURCE_DIR}/apollo.svg"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps")
else()
    install(FILES "${CMAKE_SOURCE_DIR}/apollo.svg"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps"
            RENAME "${PROJECT_FQDN}.svg")
endif()

# tray icon
if(${SUNSHINE_TRAY} STREQUAL 1)
    if(NOT ${SUNSHINE_BUILD_FLATPAK})
        install(FILES "${CMAKE_SOURCE_DIR}/apollo.svg"
                DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/status"
                RENAME "apollo-tray.svg")
        install(FILES "${SUNSHINE_SOURCE_ASSETS_DIR}/common/assets/web/public/images/apollo-playing.svg"
                DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/status")
        install(FILES "${SUNSHINE_SOURCE_ASSETS_DIR}/common/assets/web/public/images/apollo-pausing.svg"
                DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/status")
        install(FILES "${SUNSHINE_SOURCE_ASSETS_DIR}/common/assets/web/public/images/apollo-locked.svg"
                DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/status")
    else()
        # flatpak icons must be prefixed with the app id or they will not be included in the flatpak
        install(FILES "${CMAKE_SOURCE_DIR}/apollo.svg"
                DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/status"
                RENAME "${PROJECT_FQDN}-tray.svg")
        install(FILES "${SUNSHINE_SOURCE_ASSETS_DIR}/common/assets/web/public/images/apollo-playing.svg"
                DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/status"
                RENAME "${PROJECT_FQDN}-playing.svg")
        install(FILES "${SUNSHINE_SOURCE_ASSETS_DIR}/common/assets/web/public/images/apollo-pausing.svg"
                DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/status"
                RENAME "${PROJECT_FQDN}-pausing.svg")
        install(FILES "${SUNSHINE_SOURCE_ASSETS_DIR}/common/assets/web/public/images/apollo-locked.svg"
                DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/status"
                RENAME "${PROJECT_FQDN}-locked.svg")
    endif()

    set(CPACK_DEBIAN_PACKAGE_DEPENDS "\
                    ${CPACK_DEBIAN_PACKAGE_DEPENDS}, \
                    libayatana-appindicator3-1, \
                    libnotify4")
    set(CPACK_RPM_PACKAGE_REQUIRES "\
                    ${CPACK_RPM_PACKAGE_REQUIRES}, \
                    libappindicator-gtk3 >= 12.10.0")
    list(APPEND CPACK_FREEBSD_PACKAGE_DEPS
            devel/libayatana-appindicator
            devel/libnotify
    )
endif()

# desktop file
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_FQDN}.desktop"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications")
if(NOT ${SUNSHINE_BUILD_APPIMAGE} AND NOT ${SUNSHINE_BUILD_FLATPAK})
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_FQDN}.terminal.desktop"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications")
    if(${SUNSHINE_ENABLE_KWIN})
        install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_FQDN}.kwin.desktop"
                DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications")
    endif()
endif()

# metadata file
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_FQDN}.metainfo.xml"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/metainfo")
