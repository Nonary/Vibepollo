#!/usr/bin/env python3

import pathlib
import sys


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"{label} is missing required contract: {needle}")


root = pathlib.Path(sys.argv[1])
unit = (root / "packaging/linux/app-io.github.Nonary.vibepollo.service.in").read_text()
pam_module = (root / "packaging/linux/pam_vibepollo_session.c").read_text()
handoff = (root / "packaging/linux/vibepollo-session-handoff").read_text()
ready = (root / "packaging/linux/vibepollo-session-ready").read_text()
restore_unit = (root / "packaging/linux/vibepollo-session-restore@.service").read_text()
packaging = (root / "cmake/packaging/linux.cmake").read_text()
package_configuration = (root / "cmake/prep/special_package_configuration.cmake").read_text()
rpm_spec = (root / "packaging/linux/copr/Sunshine.spec").read_text()
arch_package = (root / "packaging/linux/Arch/PKGBUILD").read_text()

require(unit, "StartLimitIntervalSec=0", "user service")
require(unit, "PartOf=graphical-session.target", "user service")
require(unit, "RestartSteps=7", "user service")
require(unit, "RestartMaxDelaySec=5min", "user service")
require(unit, "TimeoutStartSec=infinity", "user service")
require(unit, "@SUNSHINE_SERVICE_READINESS_COMMAND@", "user service")
if "StartLimitBurst=" in unit:
    raise AssertionError("user service must not retain a permanent start-limit latch")

# The privileged boundary obtains identity from PAM and execs a fixed helper
# with an empty environment, so a login request cannot inject root shell or
# dynamic-loader startup behavior.
require(pam_module, "pam_get_item(pamh, PAM_SERVICE", "PAM module")
require(pam_module, 'strcmp(service, "plasmalogin")', "PAM module")
require(pam_module, "pam_get_user(pamh", "PAM module")
require(pam_module, "getpwnam_r", "PAM module")
require(pam_module, "getrandom", "PAM module")
require(pam_module, "pam_set_data", "PAM module")
require(pam_module, "pam_get_data", "PAM module")
require(pam_module, "char *const environment[] = {NULL}", "PAM module")
require(pam_module, "execve(helper_path, arguments, environment)", "PAM module")
require(pam_module, "allowing the desktop login without streaming handoff", "fail-soft PAM policy")
if "getenv(" in pam_module or "system(" in pam_module:
    raise AssertionError("PAM module must not consume the caller environment or a shell")

require(handoff, "pam-open USER UID TOKEN", "session helper")
require(handoff, "pam-close USER UID TOKEN", "session helper")
require(handoff, "session-handoffs", "per-UID handoff state")
require(handoff, "session-restores", "per-UID restore state")
require(handoff, "valid_token", "session token validation")
require(handoff, "/usr/bin/flock --exclusive --timeout 30", "serialized handoff")
require(handoff, "root:root:600:regular file", "trusted transition lock")
require(handoff, "/usr/bin/env -i", "sanitized user-manager call")
require(handoff, '[[ -S "$control_socket" && ! -L "$control_socket" ]]', "control socket validation")
require(handoff, '/usr/bin/timeout --signal=KILL "$socket_timeout"', "control socket timeout")
require(handoff, "rollback_to_prelogin", "transactional login handoff")
require(handoff, "another desktop session owns the stream handoff", "multi-user exclusion")
require(handoff, '[[ "$state_token" != "$token" ]]', "stale close rejection")
require(handoff, 'systemctl start --no-block "$prelogin_service"', "pre-login restoration")
require(handoff, 'user_systemctl "$desktop_user" stop "$desktop_target"', "desktop shutdown")
require(handoff, "stream_ports_are_free", "port ownership handoff")
require(handoff, "release_connector", "connector ownership handoff")
require(handoff, "wait-prelogin", "pre-login readiness gate")
require(handoff, "attempt & (attempt - 1)", "rate-limited status logging")
if "PAM_TYPE" in handoff or "PAM_USER" in handoff:
    raise AssertionError("session helper must not trust PAM environment variables")

require(ready, "session-handoffs/$(/usr/bin/id -u)", "per-UID readiness gate")
require(ready, "systemctl --user is-active --quiet plasma-kwin_wayland.service", "readiness gate")
require(ready, "kscreen-doctor -j", "readiness gate")
require(ready, "timeout --signal=KILL 5", "readiness gate")
require(ready, "attempt & (attempt - 1)", "readiness gate")
require(ready, "VIBEPOLLO_REQUIRE_PRELOGIN_HANDOFF", "readiness gate")
require(ready, "kwin_service_is_installed", "readiness gate")

require(restore_unit, "Type=oneshot", "restore supervisor unit")
require(restore_unit, "vibepollo-session-handoff restore %i", "restore supervisor unit")
require(restore_unit, "TimeoutStartSec=infinity", "restore supervisor unit")

require(packaging, "add_library(pam_vibepollo_session MODULE", "native PAM build")
require(packaging, "install(TARGETS pam_vibepollo_session", "native PAM install")
require(packaging, "libpam0g", "Debian PAM dependency")
require(packaging, "socat", "native socket dependency")
require(packaging, "iproute2", "Debian socket-inspection dependency")
require(packaging, "util-linux", "native flock/runuser dependency")
require(package_configuration, '"package-version:${PROJECT_VERSION_NUMERIC}\\n"',
        "versioned kernel source fingerprint")
require(packaging, '"${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo-session-handoff"', "native install")
require(packaging, '"${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo-session-ready"', "native install")
require(packaging, '"${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo-session-restore@.service"', "native install")

require(rpm_spec, "BuildRequires: pam-devel", "RPM PAM build dependency")
require(rpm_spec, "Requires: pam", "RPM PAM runtime dependency")
require(rpm_spec, "pam_vibepollo_session.so", "RPM PAM module")
for dependency in ("'pam'", "'socat'", "'iproute2'", "'util-linux'"):
    require(arch_package, dependency, "Arch runtime dependency")

print("PASS: hardened Linux session handoff contract")
