#!/usr/bin/env python3

import pathlib
import sys


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"{label} is missing required contract: {needle}")


root = pathlib.Path(sys.argv[1])
unit = (root / "packaging/linux/app-io.github.Nonary.vibepollo.service.in").read_text()
handoff = (root / "packaging/linux/vibepollo-session-handoff").read_text()
ready = (root / "packaging/linux/vibepollo-session-ready").read_text()
restore_unit = (root / "packaging/linux/vibepollo-session-restore@.service").read_text()
packaging = (root / "cmake/packaging/linux.cmake").read_text()
rpm_spec = (root / "packaging/linux/copr/Sunshine.spec").read_text()

require(unit, "StartLimitIntervalSec=0", "user service")
require(unit, "PartOf=graphical-session.target", "user service")
require(unit, "RestartSteps=7", "user service")
require(unit, "RestartMaxDelaySec=5min", "user service")
require(unit, "TimeoutStartSec=infinity", "user service")
require(unit, "@SUNSHINE_SERVICE_READINESS_COMMAND@", "user service")
if "StartLimitBurst=" in unit:
    raise AssertionError("user service must not retain a permanent start-limit latch")

require(handoff, '[[ "${PAM_TYPE:-}" == close_session', "PAM handoff")
require(handoff, "systemctl start --no-block \"$prelogin_service\"", "PAM handoff")
require(handoff, 'systemctl --user stop "$desktop_target"', "PAM handoff")
require(handoff, "desktop_target=graphical-session.target", "PAM handoff")
require(handoff, 'signal=KILL "$desktop_compositor"', "PAM handoff")
require(handoff, 'start --no-block "$desktop_service"', "PAM handoff")
require(handoff, 'start --no-block "${restore_unit_prefix}${desktop_uid}.service"', "PAM handoff")
require(handoff, 'restore_prelogin "$2"', "restore supervisor")
require(handoff, "/usr/bin/sleep 1", "restore supervisor")
require(handoff, 'start "$greeter_target"', "restore supervisor")
require(handoff, "stream_ports_are_free", "PAM handoff")
require(handoff, "release_connector", "PAM handoff")
require(handoff, "root:root:644", "PAM handoff")
require(handoff, "wait-prelogin", "pre-login readiness gate")
require(handoff, "attempt & (attempt - 1)", "pre-login readiness gate")
require(handoff, "maximum_delay=5", "pre-login readiness gate")

require(ready, "systemctl --user is-active --quiet plasma-kwin_wayland.service", "readiness gate")
require(ready, "kscreen-doctor -j", "readiness gate")
require(ready, "timeout --signal=KILL 5", "readiness gate")
require(ready, "attempt & (attempt - 1)", "readiness gate")
require(ready, "maximum_delay=5", "readiness gate")
require(ready, "VIBEPOLLO_REQUIRE_PRELOGIN_HANDOFF", "readiness gate")
require(ready, "kwin_service_is_installed", "readiness gate")

require(restore_unit, "Type=oneshot", "restore supervisor unit")
require(restore_unit, "vibepollo-session-handoff restore %i", "restore supervisor unit")
require(restore_unit, "TimeoutStartSec=infinity", "restore supervisor unit")

require(packaging, '"${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo-session-handoff"', "native install")
require(packaging, '"${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo-session-ready"', "native install")
require(packaging, '"${CMAKE_SOURCE_DIR}/packaging/linux/vibepollo-session-restore@.service"', "native install")

require(rpm_spec, "vibepollo-session-handoff", "RPM package")
require(rpm_spec, "vibepollo-session-ready", "RPM package")
require(rpm_spec, "vibepollo-session-restore@.service", "RPM package")

print("PASS: Linux session handoff contract")
