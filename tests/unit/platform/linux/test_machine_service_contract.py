#!/usr/bin/env python3

import pathlib
import sys


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"{label} is missing required contract: {needle}")


root = pathlib.Path(sys.argv[1])
service = (root / "packaging/linux/vibepollo.service").read_text()
prepare_service = (root / "packaging/linux/vibepollo-machine-prepare.service").read_text()
host = (root / "packaging/linux/vibepollo-machine-host").read_text()
handoff = (root / "packaging/linux/vibepollo-session-handoff").read_text()
launcher = (root / "packaging/linux/vibepollo-session-exec.c").read_text()
private_display = (root / "src/platform/linux/private_display.cpp").read_text()
audio = (root / "src/platform/linux/audio.cpp").read_text()
sysusers = (root / "packaging/linux/vibepollo.sysusers").read_text()
apps = (root / "packaging/linux/prelogin/apps.json").read_text()
packaging = (root / "cmake/packaging/linux.cmake").read_text()
root_cmake = (root / "CMakeLists.txt").read_text()
arch_install = (root / "packaging/linux/Arch/vibepollo.install").read_text()
rpm = (root / "packaging/linux/copr/Sunshine.spec").read_text()
postinst = (root / "packaging/linux/vibeshine-postinst.in").read_text()
prerm = (root / "packaging/linux/vibeshine-prerm.in").read_text()
special_packaging = (root / "cmake/prep/special_package_configuration.cmake").read_text()
drm_setup = (root / "third-party/libvirtualdisplay/linux/packaging/vibeshine-drm-setup.service.in").read_text()

require(sysusers, 'u vibepollo - "Vibepollo machine host" /var/lib/vibepollo /usr/bin/nologin', "sysusers")
require(service, "User=vibepollo", "machine service")
require(service, "Group=vibepollo", "machine service")
require(service, "StateDirectory=vibepollo", "machine service")
require(service, "StateDirectoryMode=0700", "machine service")
require(service, "vibepollo-machine-host run", "machine service")
if "User=root" in service or "CAP_DAC_OVERRIDE" in service:
    raise AssertionError("machine host must not run as root or bypass filesystem permissions")
require(prepare_service, "Type=oneshot", "prepare service")
require(prepare_service, "vibepollo-machine-host prepare", "prepare service")
require(prepare_service, "ReadOnlyPaths=/home", "prepare service")
require(prepare_service, "InaccessiblePaths=/root", "prepare service")
if "WantedBy=" in prepare_service:
    raise AssertionError("privileged preparation must run only as a dependency of the machine host")

require(host, "readonly machine_profile=/var/lib/vibepollo", "machine host")
require(host, "source=$desktop_home/.config/vibepollo", "one-time migration")
require(host, "profile is non-empty without its marker", "one-time migration")
require(host, 'chown "$service_user:$service_user" "$machine_profile"', "machine profile ownership")
require(host, 'chmod 0700 "$machine_profile"', "machine profile mode")
require(host, 'setfacl -m "u:$service_user:--x" "$runtime"', "runtime socket boundary")
require(host, 'chmod 0640 "$temporary"', "trusted session record")
require(host, 'VIBEPOLLO_MACHINE_HOST=1', "child-launch routing")
if "/var/lib/vibepollo-prelogin" in host or "allowed_client_uuid=" in host:
    raise AssertionError("machine host must not recreate a copied pre-login profile or client allowlist")

require(launcher, 'static const char session_path[] = "/run/vibepollo/session.env"', "session launcher")
require(launcher, "O_NOFOLLOW", "session launcher")
require(launcher, "attributes.st_uid != 0", "session launcher")
require(launcher, "attributes.st_gid != service_gid", "session launcher")
require(launcher, "setgroups(identity.group_count", "session launcher")
require(launcher, "setgid(identity.gid)", "session launcher")
require(launcher, "setuid(identity.uid)", "session launcher")
require(launcher, "cap_init()", "session launcher")
require(launcher, "PR_SET_NO_NEW_PRIVS", "session launcher")
require(launcher, 'unsetenv("VIBEPOLLO_MACHINE_HOST")', "session launcher")
if "system(" in launcher or "/bin/sh" in launcher:
    raise AssertionError("privileged session launcher must not invoke a shell")
require(private_display, 'std::getenv("VIBEPOLLO_MACHINE_HOST")', "KScreen routing")
require(private_display, 'owned_argv.emplace_back("/usr/libexec/vibeshine/vibepollo-session-exec")', "KScreen routing")
require(audio, 'std::getenv("VIBEPOLLO_MACHINE_HOST")', "session audio routing")
require(audio, 'constexpr auto session_exec_path = "/usr/libexec/vibeshine/vibepollo-session-exec"', "session audio boundary")
require(audio, 'constexpr auto pactl_path = "/usr/bin/pactl"', "session audio control")
require(audio, 'constexpr auto parec_path = "/usr/bin/parec"', "session audio capture")
if "system(" in audio or "/bin/sh" in audio:
    raise AssertionError("session audio bridge must not invoke a shell")

require(handoff, "readonly host_service=vibepollo.service", "session handoff")
require(handoff, 'write_state_file "$marker_directory" "$desktop_user" "$desktop_uid" "$token" 0600', "session handoff")
if "user_systemctl \"$desktop_user\" start" in handoff:
    raise AssertionError("handoff must restart the machine service, not a per-user host")
require(apps, "vibepollo-machine-host activate", "pre-login desktop app")

require(packaging, "add_executable(vibepollo_session_exec", "native packaging")
require(packaging, "vibepollo-machine-host", "native packaging")
require(packaging, "vibepollo.service", "native packaging")
require(packaging, "vibepollo.sysusers", "native packaging")
require(packaging, "cap_setgid,cap_setuid+ep", "native packaging")
if '"${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo-prelogin.service"' in packaging:
    raise AssertionError("native package must not install the copied-profile pre-login service")

require(arch_install, "setcap cap_setgid,cap_setuid=ep", "Arch package")
require(arch_install, "chown root:vibepollo", "Arch package")
require(arch_install, "systemctl enable vibepollo.service", "Arch package")
require(arch_install, "local helper=/usr/libexec/vibeshine/vibeshine-kwin-capability", "Arch package")
require(arch_install, '"$helper" prepare', "Arch package")
require(arch_install, "/usr/libexec/vibeshine/vibeshine-kwin-capability restore", "Arch package")
require(postinst, "@VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR@/vibeshine-kwin-capability", "native post-install")
require(prerm, "@VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR@/vibeshine-kwin-capability", "native pre-remove")
require(special_packaging, "if(BUILD_VIBESHINE_KWIN_GPU_BRIDGE)", "package configuration")
require(special_packaging, "VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR}/vibeshine-kwin-capability prepare", "package configuration")
bridge_setting = "set(BUILD_VIBESHINE_KWIN_GPU_BRIDGE ON CACHE BOOL"
special_include = "include(${CMAKE_MODULE_PATH}/prep/special_package_configuration.cmake)"
require(root_cmake, bridge_setting, "top-level package configuration")
require(root_cmake, special_include, "top-level package configuration")
if root_cmake.index(bridge_setting) > root_cmake.index(special_include):
    raise AssertionError("KWin GPU bridge must be enabled before package templates are configured")
require(drm_setup, "@VIBESHINE_KWIN_CAPABILITY_EXEC_START@", "DRM setup service")
require(drm_setup, "@VIBESHINE_KWIN_CAPABILITY_EXEC_STOP@", "DRM setup service")
require(drm_setup, "@VIBESHINE_KWIN_CAPABILITY_REMAIN_AFTER_EXIT@", "DRM setup service")
require(rpm, "%caps(cap_setgid,cap_setuid=ep)", "RPM package")
require(rpm, "vibeshine-kwin-capability prepare", "RPM package")
require(rpm, "vibeshine-kwin-capability restore", "RPM package")
require(rpm, "%{_unitdir}/vibepollo.service", "RPM package")

print("PASS: least-privilege Linux machine service contract")
