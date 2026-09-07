#!/usr/bin/env python3

import json
import pathlib
import re
import sys


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"{label} is missing required contract: {needle}")


def forbid(text: str, needle: str, label: str) -> None:
    if needle in text:
        raise AssertionError(f"{label} retains forbidden legacy or privileged behavior: {needle}")


root = pathlib.Path(sys.argv[1])
linux = root / "packaging/linux"
controller = (linux / "vibepollo-session-controller").read_text()
controller_unit = (linux / "vibepollo-session-controller.service").read_text()
kwin_environment = (linux / "vibepollo-kwin-session-environment.c").read_text()
kwin_environment_dropin = (linux / "vibepollo-kwin-session-environment.conf").read_text()
pairing_policy = (root / "src/http_pairing_policy.cpp").read_text()
host = (linux / "vibepollo-machine-host").read_text()
host_unit = (linux / "vibepollo.service").read_text()
broker_unit = (linux / "vibepollo-session-exec@.service").read_text()
broker_socket_unit = (linux / "vibepollo-session-exec.socket").read_text()
launcher = (linux / "vibepollo-session-exec.c").read_text()
broker = (linux / "vibepollo-session-broker.c").read_text()
steam_launcher = (linux / "vibepollo-steam-launch.cpp").read_text()
session_execution = launcher + "\n" + broker
private_display = (root / "src/platform/linux/private_display.cpp").read_text()
display_power = (linux / "vibepollo-display-power.h").read_text()
display_power_client = (root / "src/platform/linux/display_power.cpp").read_text()
rtsp = (root / "src/rtsp.cpp").read_text()
stream = (root / "src/stream.cpp").read_text()
kmsgrab = (root / "src/platform/linux/kmsgrab.cpp").read_text()
audio = (root / "src/platform/linux/audio.cpp").read_text()
nvhttp = (root / "src/nvhttp.cpp").read_text()
confighttp = (root / "src/confighttp.cpp").read_text()
linux_misc = (root / "src/platform/linux/misc.cpp").read_text()
main_source = (root / "src/main.cpp").read_text()
sysusers = (linux / "vibepollo.sysusers").read_text()
packaging = (root / "cmake/packaging/linux.cmake").read_text()
special_packaging = (root / "cmake/prep/special_package_configuration.cmake").read_text()
arch_install = (linux / "Arch/vibepollo.install").read_text()
arch_pre = arch_install.split("\ndo_udev_reload()", 1)[0]
arch_pkgbuild = (linux / "Arch/PKGBUILD").read_text()
rpm = (linux / "copr/Sunshine.spec").read_text()
preinst = (linux / "vibeshine-preinst.in").read_text()
postinst = (linux / "vibeshine-postinst.in").read_text()
prerm = (linux / "vibeshine-prerm.in").read_text()
prelogin_apps = json.loads((linux / "prelogin/apps.json").read_text())
rpm_pre = rpm.split("\n%pre\n", 1)[1].split("\n%post\n", 1)[0]
rpm_post = rpm.split("\n%post\n", 1)[1].split("\n%preun\n", 1)[0]
rpm_preun = rpm.split("\n%preun\n", 1)[1].split("\n%files\n", 1)[0]
controller_unit_directives = "\n".join(
    line for line in controller_unit.splitlines() if not line.lstrip().startswith("#")
)
host_unit_directives = "\n".join(
    line for line in host_unit.splitlines() if not line.lstrip().startswith("#")
)

obsolete = (
    "pam_vibepollo_session.c",
    "vibepollo-machine-prepare.service",
    "vibepollo-session-handoff",
    "vibepollo-session-restore@.service",
    "vibepollo-prelogin-sync",
    "vibepollo-prelogin.service",
    "vibepollo-session-ready",
)
for name in obsolete:
    if (linux / name).exists():
        raise AssertionError(f"obsolete login-path component still exists: {name}")

# The controller is the only boot authority. It observes supported session
# state continuously and is deliberately not a dependency of Plasma/login.
require(controller_unit, "Wants=display-manager.service systemd-logind.service systemd-user-sessions.service vibeshine-vkms.service", "controller unit")
require(controller_unit, "After=display-manager.service systemd-logind.service systemd-user-sessions.service vibeshine-vkms.service", "controller unit")
require(controller_unit, "ExecStart=/usr/libexec/vibeshine/vibepollo-session-controller run", "controller unit")
require(controller_unit, "ExecStopPost=/usr/libexec/vibeshine/vibepollo-session-controller cleanup", "controller unit")
require(controller_unit, "Restart=always", "controller unit")
require(controller_unit, "CapabilityBoundingSet=CAP_SETGID CAP_SETUID", "controller unit")
require(controller_unit, "AmbientCapabilities=CAP_SETUID", "controller unit")
require(controller_unit, "NoNewPrivileges=yes", "controller unit")
require(controller_unit, "PrivateDevices=yes", "controller unit")
require(controller_unit, "ProtectHome=tmpfs", "controller unit")
require(controller_unit, "BindReadOnlyPaths=/run/user", "controller unit")
require(controller_unit, "IPAddressDeny=any", "controller unit")
require(controller_unit, "WantedBy=graphical.target", "controller unit")
for forbidden in ("Requires=", "CAP_SYS_ADMIN", "CAP_DAC_OVERRIDE", "ConditionPathExists=", "ReadWritePaths="):
    forbid(controller_unit_directives, forbidden, "controller unit")

for field in (
    "--property=User",
    "--property=Name",
    "--property=Seat",
    "--property=Remote",
    "--property=Type",
    "--property=Class",
    "--property=Active",
    "--property=State",
    "--property=Service",
):
    require(controller, field, "controller logind observation")
for exact_predicate in (
    '"$observed_seat" == "$seat_name"',
    '"$observed_remote" == no',
    '"$observed_type" == wayland',
    '"$observed_active" == yes',
    '"$observed_state" == active',
    '"$observed_class" == user',
    'desktop_service_supported "$observed_service"',
    '"$observed_class" == greeter',
    '"$observed_service" == plasmalogin-greeter',
):
    require(controller, exact_predicate, "controller session classification")
for readiness in (
    "plasma-login-wayland.target",
    "plasma-login-kwin_wayland.service",
    "plasma-login.service",
    "plasma-workspace-wayland.target",
    "graphical-session.target",
    "plasma-kwin_wayland.service",
    "plasma-plasmashell.service",
    "/usr/bin/wayland-info",
    "/usr/bin/kscreen-doctor -j",
    "/usr/bin/xdpyinfo",
):
    require(controller, readiness, "controller compositor readiness")
forbid(controller, "plasma-ksmserver.service", "controller compositor readiness")
require(controller, '*.target) [[ "$active" == active && "$substate" == active ]]', "target readiness without a synthetic PID")
require(controller, "readonly maximum_bytes=1048576", "bounded target-user KScreen parsing")
require(controller, '/usr/bin/head -c "$((maximum_bytes + 1))"', "bounded target-user KScreen parsing")
require(controller, "size <= maximum_bytes", "bounded target-user KScreen parsing")
require(controller, '/usr/bin/jq -e ".outputs | type == \\"array\\""', "target-user KScreen JSON parsing")
forbid(controller, '<<<"$probe_output"', "root KScreen JSON parsing")
for mutation in (
    "systemctl --user start",
    "systemctl --user restart",
    "systemctl --user set-environment",
    "user@${",
    "loginctl activate",
):
    forbid(controller, mutation, "controller passive session observation")

# KWin itself is the authority for its generated socket name and randomized X
# cookie. Its existing user unit publishes those values before Plasma/XDG
# autostart, without granting the root controller authority over user state.
for invariant in (
    '"/usr/bin/kwin_wayland"',
    'read_unified_cgroup(getpid()',
    'read_file_at_bounded',
    'process_is_session_kwin',
    'readlinkat(process_directory',
    'O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW',
    '"--socket"',
    '"--xwayland-display"',
    '"--xwayland-xauthority"',
    'discover_wayland_socket',
    'session_kind_from_cgroup',
    'KWIN_SESSION_GREETER',
    'KWIN_SESSION_DESKTOP',
    'canonical_runtime_is_safe',
    '"/run/user/%lu"',
    'root_runtime_parent_is_safe("/run")',
    'root_runtime_parent_is_safe("/run/user")',
    'AT_SYMLINK_NOFOLLOW',
    '!S_ISSOCK(attributes.st_mode)',
    'has_x_display == has_xauthority',
    'xauthority_path_is_confined',
    'S_ISSOCK(wayland_attributes.st_mode)',
    'S_ISREG(xauth_attributes.st_mode)',
    '(xauth_attributes.st_mode & 0077)',
    'PR_SET_NO_NEW_PRIVS',
    '"/usr/bin/dbus-update-activation-environment"',
    '"--systemd"',
    '"WAYLAND_DISPLAY", "DISPLAY", "XAUTHORITY"',
    '"WAYLAND_DISPLAY", (char *) NULL',
    'alarm(5)',
):
    require(kwin_environment, invariant, "KWin session environment publication")
for forbidden in ("setuid(", "setgid(", "sudo", "systemctl"):
    forbid(kwin_environment, forbidden, "KWin session environment publication")
require(
    kwin_environment_dropin,
    "ExecStartPost=-/usr/libexec/vibeshine/vibepollo-kwin-session-environment",
    "KWin session environment drop-in",
)
for forbidden in ("ExecStart=", "ExecStartPre=", "WantedBy="):
    forbid(kwin_environment_dropin, forbidden, "KWin session environment drop-in")
require(controller, 'stop_host "$deadline" || success=1', "fail-closed transition ordering")
require(controller, "binding_matches_candidate complete", "display credential rebinding")
require(controller, "observe_active_session || return 1", "authoritative transition snapshot")
require(controller, "read_user_environment || return 1", "current transition credentials")
for snapshot_field in (
    "candidate_session", "candidate_user", "candidate_uid", "candidate_gid",
    "candidate_home", "candidate_role", "candidate_display", "candidate_xdisplay",
    "candidate_xauthority",
):
    require(controller, f'"${snapshot_field}" == "$expected_{snapshot_field.removeprefix("candidate_")}"', "complete transition snapshot")
require(controller, "host_unit_is_active", "notified systemd host readiness")
require(controller, "host_unit_is_stopped", "systemd host stop proof")
for stopped_property in ("ActiveState", "SubState", "MainPID", "ControlGroup", "populated 0"):
    require(controller, stopped_property, "systemd/cgroup host stop proof")
forbid(controller, "/usr/bin/ss", "spoofable global port gating")
require(controller, 'local pattern="vibepollo-app-${generation}-*.service"', "generation-scoped application cleanup")
require(controller, 'stop_bound_session_apps "$deadline" || success=1', "application cleanup transition ordering")
require(controller, "host-stop-cleanup-failed", "stopped existing-binding cleanup")
inactive_binding_at = controller.index("if ((binding_matches)) && ! host_is_active; then")
readiness_probe_at = controller.index("if ! session_is_ready; then")
if inactive_binding_at >= readiness_probe_at:
    raise AssertionError("inactive existing binding is not quiesced before readiness probes")
existing_binding_branch = controller.split("\n  if ((binding_matches)); then\n", 1)[1].split(
    "\n  ((quiesced)) || {", 1
)[0]
require(existing_binding_branch, "elif quiesce; then", "stopped existing-binding cleanup")
forbid(existing_binding_branch, "start_host", "direct existing-binding restart")
quiesce_body = controller.split("\nquiesce() {\n", 1)[1].split("\n}\n\nnext_generation()", 1)[0]
quiesce_normalized = " ".join(quiesce_body.split())
quiesce_order = (
    quiesce_normalized.index('close_broker_admission "$deadline" || return 1'),
    quiesce_normalized.index('stop_host "$deadline" || success=1'),
    quiesce_normalized.index('stop_broker_instances "$deadline" || success=1'),
    quiesce_normalized.index('stop_bound_session_apps "$deadline" || success=1'),
    quiesce_normalized.rindex('stop_broker_instances "$deadline" || success=1'),
    quiesce_normalized.index("((success == 0)) || return 1"),
    quiesce_normalized.index("remove_session_record || success=1"),
)
if tuple(sorted(quiesce_order)) != quiesce_order:
    raise AssertionError("controller quiesce does not close admission, drain every exact cgroup, then revoke state")
for broker_stop_proof in (
    "local deadline=$((SECONDS + quiesce_timeout)) success=0",
    "local -A stop_requested=()",
    "broker_unit_is_stopped",
    "--property=ControlGroup",
    "session_app_control_group_is_empty",
    "required_clean_passes=10",
):
    require(controller, broker_stop_proof, "bounded broker cgroup quiescence")
for forced_stop in ("kill --kill-whom=all --signal=KILL",):
    forbid(controller, forced_stop, "controller graceful cgroup quiescence")
shutdown_body = controller.split("\nshutdown_controller() {\n", 1)[1].split("\n}\n\nrun_controller()", 1)[0]
forbid(shutdown_body, "\n  quiesce", "single ExecStopPost quiesce transaction")
for forbidden_connector_authority in (
    "connector_request",
    "release_connectors",
    "vkms-control.sock",
    "vkms-leases",
):
    forbid(controller, forbidden_connector_authority, "controller display ownership")
forbid(controller_unit, "SupplementaryGroups=vibeshine vibeshine-vkms", "controller display ownership")
require(controller, "next_generation", "generation-bound session records")
require(controller, 'chmod 0640 "$temporary"', "trusted session record mode")
require(controller, "xauthority_is_private", "private owner-only Xauthority")
require(controller, "(8#$mode & 0077) == 0", "private owner-only Xauthority mode")
require(controller, '"$path" != */../* && "$path" != */..', "Xauthority path confinement")
for forbidden_acl in ("setfacl", "getfacl", "runtime-acl"):
    forbid(controller, forbidden_acl, "controller compositor/home ACL grant")
forbid(controller, '[[ -S "$candidate_runtime/bus" ]]', "capability-bounded runtime access")

# The network host owns machine state but no login lifecycle. Its only
# privilege expansion is the existing KMS binary plus the narrow session shim.
require(sysusers, 'u vibepollo - "Vibepollo machine host" /var/lib/vibepollo /usr/bin/nologin', "machine account")
require(sysusers, "g vibepollo-uinput - -", "virtual input group the host unit joins")
require(host_unit, "SupplementaryGroups=video render vibepollo-uinput vibeshine-vkms", "restricted virtual input membership")
require(host, '"$home/.config/"{vibepollo,vibeshine,sunshine}/{vibeshine_state,sunshine_state}.json',
        "pairing profile discovery across all three host names")
require(controller, 'desktop_service_supported() { [[ "$1" =~ ^(plasmalogin|plasmalogin-autologin|sddm|sddm-autologin)$ ]]; }',
        "SDDM and Plasma Login Manager desktop sessions")
uinput_rules = (linux / "70-vibepollo-uinput.rules").read_text()
for rule in (
    'KERNEL=="uinput", SUBSYSTEM=="misc", GROUP="vibepollo-uinput", MODE="0660"',
    'KERNEL=="uhid", SUBSYSTEM=="misc", GROUP="vibepollo-uinput", MODE="0660"',
):
    require(uinput_rules, rule, "dedicated virtual input device group")
for native_asset in (
    "%{_udevrulesdir}/70-vibepollo-uinput.rules",
    "%{_prefix}/lib/firewalld/services/vibepollo.xml",
    "%{_sysconfdir}/ufw/applications.d/vibepollo",
    "%{_datadir}/pipewire/pipewire.conf.d/50-vibepollo-audio.conf",
):
    require(rpm, native_asset, "RPM native setup asset manifest")
require(host_unit, "Requires=vibepollo-session-exec.socket", "broker-loss host revocation")
require(host_unit, "Wants=vibeshine-vkms.service", "machine host unit")
require(host_unit, "After=vibepollo-session-controller.service vibepollo-session-exec.socket", "ordered machine-host startup")
for unit_text, label in (
    (broker_socket_unit, "session broker socket"),
    (broker_unit, "session broker connection"),
):
    require(unit_text, "After=vibepollo-session-controller.service", f"{label} startup ordering")
    require(unit_text, "Before=vibepollo.service", f"{label} shutdown drain ordering")
    forbid(unit_text, "BindsTo=vibepollo-session-controller.service", f"{label} ordered controller cleanup")
    forbid(unit_text, "PartOf=vibepollo-session-controller.service", f"{label} ordered controller cleanup")
require(host_unit, "KillMode=control-group", "machine host process-tree shutdown")
require(host_unit, "SendSIGKILL=no", "GPU-owner graceful shutdown")
require(host_unit, "Type=notify", "encoder-gated machine host readiness")
require(host_unit, "NotifyAccess=all", "encoder-gated machine host readiness")
require(host_unit, "TimeoutStartSec=45", "encoder-gated machine host readiness")
require(host_unit, "User=vibepollo", "machine host unit")
require(host_unit, "Group=vibepollo", "machine host unit")
require(host_unit, "StateDirectory=vibepollo", "machine host unit")
require(host_unit, "CapabilityBoundingSet=CAP_SYS_ADMIN CAP_SYS_NICE", "machine host unit")
require(host_unit, "AmbientCapabilities=", "machine host unit")
require(host_unit, "NoNewPrivileges=no", "machine host unit")
require(host_unit, "DevicePolicy=closed", "machine host unit")
require(host_unit, "ProtectSystem=strict", "machine host unit")
require(host_unit, "ProtectHome=yes", "machine host unit")
require(host_unit, "PrivateTmp=yes", "machine host unit")
require(host_unit, "RestrictSUIDSGID=yes", "machine host unit")
for forbidden in (
    "User=root", "CAP_DAC_OVERRIDE", "machine-prepare", "ExecStopPost=", "WantedBy=", "KillMode=mixed",
    "BindsTo=vibepollo-session-controller.service",
):
    forbid(host_unit_directives, forbidden, "machine host unit")
require(host, "trap mark_host_shutdown TERM INT HUP", "single service signal delivery")
forbid(host, "trap 'forward_host_signal", "duplicate service signal delivery")
require(host, "((shutting_down)) || terminate_host", "single child termination request")

# API restart of the private child exits back to the readiness-gating wrapper.
# Ordinary Linux launches retain the historical atexit self-reexec path.
restart_body = linux_misc.split("\n  void restart() {\n", 1)[1].split("\n  }\n", 1)[0]
for invariant in (
    'std::getenv("VIBEPOLLO_MACHINE_HOST")',
    "machine_host[0] == '1'",
    "machine_host[1] == '\\0'",
    "lifetime::exit_sunshine(0, true);",
    "atexit(restart_on_exit);",
):
    require(restart_body, invariant, "machine-host restart delegation")
machine_guard_at = restart_body.index('std::getenv("VIBEPOLLO_MACHINE_HOST")')
machine_return_at = restart_body.index("return;", machine_guard_at)
self_reexec_at = restart_body.index("atexit(restart_on_exit);")
if not machine_guard_at < machine_return_at < self_reexec_at:
    raise AssertionError("machine-host restart guard does not precede ordinary self-reexec registration")
require(linux_misc, "execv(executable, lifetime::get_argv())", "ordinary Linux self-reexec")

# A supervised machine child first requests display preservation and attempts
# ordered GPU teardown. Bound hung joins before systemd's 20-second timeout.
for invariant in (
    'std::getenv("VIBEPOLLO_MACHINE_HOST")',
    "machine_host_environment[0] == '1'",
    "machine_host_environment[1] == '\\0'",
    "shutdown_deadline_t shutdown_deadline {&shutdown_signal_requested, supervised_machine_host}",
    "if (supervised_machine_host_)",
    "supervised_machine_host_ ? std::chrono::seconds(15) : std::chrono::seconds(10)",
    "std::_Exit(lifetime::desired_exit_code)",
    "pthread_sigmask(SIG_BLOCK",
    "sigwait(&termination_signal_set",
    "request_process_shutdown_preserve()",
):
    require(main_source, invariant, "supervised machine-host shutdown ownership")
forbid(main_source, "std::_Exit(EXIT_FAILURE);", "supervised machine-host shutdown ownership")
require(private_display, "process_shutdown_preserve_requested()", "shutdown display handoff fence")
require(nvhttp, "process_shutdown_preserve_requested()", "shutdown display handoff fence")
require(private_display, "std::shared_mutex topology_mutation_gate", "shutdown topology mutation barrier")
request_shutdown_body = private_display.split(
    "void request_process_shutdown_preserve() noexcept {", 1
)[1].split("\n  bool process_shutdown_preserve_requested()", 1)[0]
shutdown_store_at = request_shutdown_body.index(
    "preserve_for_process_shutdown.store(true, std::memory_order_release)"
)
shutdown_barrier_at = request_shutdown_body.index(
    "std::unique_lock mutation_barrier {topology_mutation_gate}"
)
if shutdown_store_at >= shutdown_barrier_at:
    raise AssertionError("shutdown must close topology admission before draining an admitted mutation")
for mutation_name, end_marker, preserved_result in (
    ("connect_managed_output", "bool disconnect_managed_output", "return false;"),
    ("disconnect_managed_output", "std::string connector_sysfs_path", "return true;"),
):
    mutation_body = private_display.split(f"bool {mutation_name}", 1)[1].split(end_marker, 1)[0]
    if mutation_body.count("process_shutdown_preserve_requested()") < 2:
        raise AssertionError(
            f"{mutation_name} must check shutdown before and after entering the shared mutation gate"
        )
    first_fence_at = mutation_body.index("process_shutdown_preserve_requested()")
    shared_gate_at = mutation_body.index("std::shared_lock")
    require(mutation_body, "topology_mutation_gate", f"{mutation_name} shared mutation gate")
    second_fence_at = mutation_body.index("process_shutdown_preserve_requested()", first_fence_at + 1)
    if not first_fence_at < shared_gate_at < second_fence_at:
        raise AssertionError(f"{mutation_name} does not close post-shutdown reader admission")
    require(mutation_body, preserved_result, f"{mutation_name} shutdown result")
doctor_body = private_display.split("command_result_t run_doctor", 1)[1].split(
    "std::optional<json> query_configuration", 1
)[0]
for invariant in (
    "if (mutating)",
    "process_shutdown_preserve_requested()",
    "std::shared_lock {topology_mutation_gate}",
    "g_subprocess_newv(",
    "mutation_admission.unlock()",
    "g_subprocess_communicate_utf8(",
):
    require(doctor_body, invariant, "bounded KScreen mutation admission")
if not (
    doctor_body.index("std::shared_lock {topology_mutation_gate}")
    < doctor_body.index("g_subprocess_newv(")
    < doctor_body.index("mutation_admission.unlock()")
    < doctor_body.index("g_subprocess_communicate_utf8(")
):
    raise AssertionError("KScreen mutation admission remains held across its unbounded response")
execute_body = private_display.split("bool execute_configuration", 1)[1].split(
    "double floating_point", 1
)[0]
require(execute_body, "run_doctor(arguments, true)", "fenced KScreen mutation spawn")
forbid(execute_body, "std::shared_lock", "KScreen response outside mutation admission")
initialize_body = private_display.split("\n  bool initialize() {", 1)[1].split(
    "\n  prepare_result_t prepare_session", 1
)[0]
require(initialize_body, "process_shutdown_preserve_requested()", "private-display startup shutdown fence")

# Capture must not steal modesetting ownership from KWin during resume, on
# either physical primary nodes or the virtual DRM card.
require(kmsgrab, "if (drmIsMaster(fd.el) && drmDropMaster(fd.el) != 0)", "all-GPU capture master release")
forbid(kmsgrab, 'driver_name == "vibeshine_drm" && drmIsMaster', "physical GPU resume ownership")
require(broker, '!strcmp(argv[1], "display-power") && argc == 2', "fixed power operation")
execute_request = broker.split("static int execute_request(", 1)[1]
if execute_request.index("drop_to_session(identity)") > execute_request.index('execv("/usr/libexec/vibeshine/vibepollo-display-power"'):
    raise AssertionError("display power must execute only after permanent capability/UID drop")
for forbidden in ("chvt", "ActivateSession", "SwitchTo", "RestartUnit", "SetBrightness"):
    forbid(display_power, forbidden, "passive display power recovery")
require(display_power, 'setenv("QT_QPA_PLATFORM", "wayland", 1)', "Wayland DPMS transport")
require(display_power, 'write(STDOUT_FILENO, "R", 1)', "power readiness handshake")
require(display_power_client, 'token == \'R\'', "power readiness consumption")
require(display_power_client, "std::chrono::seconds(8)", "bounded power readiness")
require(display_power_client, "std::weak_ptr<lease_t> shared_lease", "shared power broker admission")
require(display_power_client, 'start_ready("display-wake")', "new launch wakes retained display")
require(packaging, "vibepollo_steam_launch vibepollo_display_power", "capability-free power helper installation")
require(rpm, "%attr(0755,root,root) %{_prefix}/libexec/vibeshine/vibepollo-display-power", "COPR power helper payload")
prepare = private_display.split("prepare_result_t prepare_session(", 1)[1]
if prepare.index("display_power::acquire()") > prepare.index("session.virtual_display = false"):
    raise AssertionError("power recovery must precede display topology preparation")
if prepare.index("return result;") > prepare.index("cancel_scheduled_revert()"):
    raise AssertionError("failed power admission must preserve scheduled display cleanup")
require(rtsp, "snapshot->display_power_guard = source.display_power_guard", "pending-to-startup power handoff")
require(stream, "session->display_power_guard = launch_session.display_power_guard", "active capture power ownership")
require(stream, "session.display_power_guard.reset()", "capture teardown releases display power")
forbid(linux_misc, "display_power::acquire()", "retained shared runtime must not inhibit sleep")

require(host, "readonly machine_profile=/var/lib/vibepollo", "machine host")
require(host, '"HOME=$machine_profile"', "machine host HOME isolation")
require(host, '"XDG_CONFIG_HOME=/var/lib"', "machine host configuration isolation")
require(host, "command_manifest_header='# vibepollo-session-commands-v2'", "versioned command authorization")
require(host, "migrate_legacy_command_manifest", "command authorization migration")
require(host, '/usr/bin/grep -Fqx -- "$authorization_command" "$command_manifest"', "no-new-command migration")
require(host, "split_authorization_line", "lossless authorization field parsing")
require(host, "extract_desktop_authorizations", "desktop-only command authorization")
require(host, "remove_pam_hook", "exact PAM cleanup migration")
for forbidden in ("install-pam", "activate_greeter", "systemctl --user", "/usr/bin/runuser", "session-handoffs", "session-restores"):
    forbid(host, forbidden, "machine host")

if len(prelogin_apps.get("apps", [])) != 1:
    raise AssertionError("pre-login catalog must expose exactly the passive desktop stream")
prelogin = prelogin_apps["apps"][0]
if prelogin.get("name") != "Desktop" or "cmd" in prelogin or "prep-cmd" in prelogin or "detached" in prelogin:
    raise AssertionError("pre-login app must not execute or restart greeter/session commands")

# The capability-free client speaks only the bounded broker protocol. The
# root-only, permitted-capability broker validates trusted records/endpoints
# and launches approved desktop commands through the user's existing manager.
for invariant in (
    "O_NOFOLLOW",
    "attributes.st_uid != 0",
    "attributes.st_gid != service_gid",
    "command_manifest_header",
    "validate_session_endpoints",
    "validate_xauthority",
    "setgroups(identity->group_count",
    "setgid(identity->gid)",
    "setuid(identity->uid)",
    "cap_init()",
    "PR_SET_NO_NEW_PRIVS",
    "clearenv()",
    '"display-query"',
    '"display-apply"',
    '"audio-get-default"',
    '"audio-list-sinks"',
    '"audio-set-default"',
    '"audio-create-null"',
    '"audio-remove-null"',
    '"audio-capture"',
    '"steam"',
    '"lutris"',
    '"app"',
    '"/usr/bin/systemd-run"',
    '"--user"',
    '"--service-type=exec"',
    '"--expand-environment=no"',
    '"--property=KillMode=control-group"',
    '"--property=TimeoutStopSec=5s"',
    'const uid_t service_uid',
    'const gid_t service_gid',
    'supervise_user_service',
    'stop_user_service',
    'waitpid(',
    'SIGTERM',
    'SIGINT',
    'SIGHUP',
    '"stop", (char *) unit',
    'parse_first_shell_word',
    'executable_parent_directory',
    'fixed_path',
    '"USER=%s"',
    '"LOGNAME=%s"',
    '"PATH=%s"',
    '"XDG_CONFIG_HOME=%s/.config"',
    '"XDG_DATA_HOME=%s/.local/share"',
    '"PIPEWIRE_RUNTIME_DIR=%s"',
    '"DBUS_SESSION_BUS_ADDRESS=unix:path=%s/bus"',
    '"XDG_SESSION_TYPE=wayland"',
    'parse_channel_mapping',
    'format_channel_mapping',
):
    require(session_execution, invariant, "session execution path")
for forbidden in ("execvp(", "execlp(", '"kscreen" &&', '"pactl" &&', '"parec" &&'):
    forbid(session_execution, forbidden, "session execution path")
require(broker, '!strcmp(identity->role, "desktop")', "desktop-only application launch")
require(broker, 'execv("/usr/bin/steam", arguments);', "direct desktop Steam handoff")

require(private_display, '? "display-query" : "display-apply"', "KScreen semantic routing")
forbidden_broker_poll = "query_connector(output_name)"
forbid(private_display, forbidden_broker_poll, "passive display readiness")
for verb in ("audio-set-default", "audio-list-sinks", "audio-get-default", "audio-create-null", "audio-remove-null", "audio-capture"):
    require(audio, verb, "audio semantic routing")
forbid(audio, 'arguments.insert(arguments.begin(), "pactl")', "audio semantic routing")
# Pairing itself is the client authorization decision. Every enabled paired
# certificate is valid before login, while unknown, disabled, malformed, or
# ambiguous records fail closed. No duplicated root/session UUID policy is
# allowed to drift from the shared machine pairing database.
for text, label in ((controller, "controller"), (broker, "broker")):
    forbid(text, "allowed_clients", f"{label} paired-client authorization")
for forbidden in ("VIBESHINE_ALLOWED_CLIENT_UUIDS", "session_allowed_clients", "read_allowed_clients"):
    forbid(host, forbidden, "machine host paired-client authorization")
for forbidden in ("VIBESHINE_ALLOWED_CLIENT_UUIDS", "greeter_client_allowed", "administrator allowlist"):
    forbid(nvhttp, forbidden, "TLS paired-client authorization")
forbid(nvhttp, "tl_peer_certificate", "request-bound TLS identity")
for invariant in (
    "exact_certificate_identity",
    "build_paired_client_records",
    "resolve_client_identity_from_peer_cert_locked",
    "Client certificate is not one exact enabled pairing record",
    "revoke_paired_client_access",
    "forget_tls_client_identities_for_uuid",
    "clear_tls_client_identities",
    "tls_client_identity_by_endpoint.size() >= pairing_policy::max_paired_clients * 8",
    "client_root.named_devices.size() >= pairing_policy::max_paired_clients",
    "if (!add_authorized_client",
):
    require(nvhttp, invariant, "exact enabled-pairing TLS authorization")
for invariant in ("paired_client_state_valid", "resolve_paired_client", "invalid_state", "disabled"):
    require(pairing_policy, invariant, "paired-client authorization policy")
# Apollo represents enabled authority with its permission bitmask. Inspect the
# permission-edit overload, retaining fail-closed persistence and revocation.
disable_body = nvhttp.rsplit("bool update_device_info(", 1)[1].split("bool unpair_client", 1)[0]
require(disable_body, "if (!effective || (previous->perm & effective) != previous->perm)", "partial permission revocation")
require(nvhttp, "(*record)->perm == expected_permissions", "pending admission permission snapshot revalidation")
require(disable_body, "revoke_paired_client_access(uuid)", "paired-client disable revocation")
enable_fresh_state_position = disable_body.index("config::flag::FRESH_STATE")
enable_migration_position = disable_body.index("statefile::migrate_recent_state_keys()")
enable_state_lock_position = disable_body.index("statefile::state_mutex()")
enable_client_lock_position = disable_body.index("client_mutex")
enable_persist_position = disable_body.index("save_state_snapshot_locked(client_root)")
enable_rollback_position = disable_body.index("named_cert_p->perm = restricted", enable_persist_position)
require(disable_body, "previous->perm & newPerm", "failed permission grant rollback")
if not (
    enable_fresh_state_position
    < enable_migration_position
    < enable_state_lock_position
    < enable_client_lock_position
    < enable_persist_position
    < enable_rollback_position
):
    raise AssertionError("client enabling must persist and roll back under state-to-client locking")
unpair_body = nvhttp.split("bool unpair_client", 1)[1].split("}  // namespace nvhttp", 1)[0]
require(unpair_body, "revoke_paired_client_access(uuid)", "paired-client removal revocation")
forbid(unpair_body, "load_state()", "paired-client removal durability")

save_body = nvhttp.split("bool save_state()", 1)[1].split("std::string get_server_cert", 1)[0]
fresh_state_position = save_body.index("config::flag::FRESH_STATE")
migration_position = save_body.index("statefile::migrate_recent_state_keys()")
state_lock_position = save_body.index("statefile::state_mutex()")
client_snapshot_position = save_body.index("client_root_snapshot()")
if not (
    fresh_state_position
    < migration_position
    < state_lock_position
    < client_snapshot_position
):
    raise AssertionError("pairing persistence must lock state before taking the client snapshot")

add_client_body = nvhttp.split("bool add_authorized_client", 1)[1].split(
    "struct resolved_client_identity_t", 1
)[0]
add_fresh_state_position = add_client_body.index("config::flag::FRESH_STATE")
add_migration_position = add_client_body.index("statefile::migrate_recent_state_keys()")
add_state_lock_position = add_client_body.index("statefile::state_mutex()")
add_client_lock_position = add_client_body.index("client_mutex")
add_persist_position = add_client_body.index("save_state_snapshot_locked(client_root)")
add_rollback_position = add_client_body.rindex("client_root.named_devices.pop_back()")
if not (
    add_fresh_state_position
    < add_migration_position
    < add_state_lock_position
    < add_client_lock_position
    < add_persist_position
    < add_rollback_position
):
    raise AssertionError("pairing grants must persist and roll back under state-to-client locking")

remembered_identity_body = nvhttp.split(
    "std::optional<resolved_client_identity_t> get_remembered_tls_client_identity", 1
)[1].split("std::string get_client_uuid_from_request", 1)[0]
require(
    remembered_identity_body,
    "paired_client_uuid_enabled_locked(it->second.uuid)",
    "cached TLS identity live authorization",
)

erase_all_body = nvhttp.split("bool erase_all_clients()", 1)[1].split("bool update_device_info", 1)[0]
require(erase_all_body, "clear_tls_client_identities()", "bulk paired-client revocation")
require(erase_all_body, "acquire_stream_start_lifecycle_lock()", "bulk paired-client admission barrier")
require(erase_all_body, "return save_state()", "bulk paired-client revocation durability")
unpair_all_api_body = confighttp.split("void unpairAll", 1)[1].split("void getConfig", 1)[0]
require(
    unpair_all_api_body,
    "const bool persisted = nvhttp::erase_all_clients()",
    "bulk paired-client revocation result",
)
require(unpair_all_api_body, 'output_tree["status"] = persisted', "bulk paired-client revocation result")

revoke_body = nvhttp.split("void revoke_paired_client_access", 1)[1].split("bool set_client_enabled", 1)[0]
barrier_position = revoke_body.index("acquire_stream_start_lifecycle_lock()")
disconnect_position = revoke_body.index("disconnect_client_sessions")
if barrier_position >= disconnect_position:
    raise AssertionError("paired-client revocation must cross admission before disconnecting sessions")

launch_admission_rechecks = re.findall(
    r"paired_client_uuid_enabled\(launch_session->client_uuid, verified_client->perm\).*?launch_session_raise\(launch_session\)",
    nvhttp,
    re.DOTALL,
)
if len(launch_admission_rechecks) != 3:
    raise AssertionError("every launch/resume admission path must revalidate live pairing authorization")
launch_body = nvhttp.split("void launch(", 1)[1].split("void resume(", 1)[0]
forbid(launch_body, "proc::proc.terminate();", "lifecycle-owned launch termination")
if len(re.findall(r"proc::proc\.terminate\(false, true(?:, false, true)?\)", launch_body)) < 3:
    raise AssertionError("every lifecycle-owned launch termination must transfer the held gate")
require(host, "configure) [[ $# == 2 ]]", "desktop-user-only machine policy")
require(host, "capability_clean_exec /usr/bin/env -i", "private host capability-clean exec")
require(host, "/usr/bin/setpriv --inh-caps=-all --ambient-caps=-all", "private host capability-clean exec")

# Native packages install and enable the controller, retain only the exact PAM
# removal migration, and do not ship any obsolete login-path component.
require(packaging, "vibepollo-session-controller", "native packaging")
require(packaging, "vibepollo_kwin_session_environment", "native packaging")
require(packaging, "vibepollo-kwin-session-environment.conf", "native packaging")
if packaging.count("vibepollo-kwin-session-environment.conf") != 1:
    raise AssertionError("native packaging must define one shared KWin environment drop-in install rule")
for kwin_unit in ("plasma-kwin_wayland", "plasma-login-kwin_wayland"):
    require(packaging, kwin_unit, "desktop and greeter KWin environment packaging")
require(
    packaging,
    '"${SYSTEMD_USER_UNIT_INSTALL_DIR}/${vibeshine_kwin_unit}.service.d"',
    "desktop and greeter KWin environment packaging",
)
require(packaging, "vibepollo-session-controller.service", "native packaging")
require(packaging, "install(TARGETS vibepollo_session_broker", "native packaging")
require(packaging, "set(CPACK_DEB_COMPONENT_INSTALL OFF)", "monolithic native DEB")
for deb_arch_contract in (
    'VIBESHINE_PACKAGE_PROCESSOR MATCHES "^(x86_64|amd64)$"',
    'set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")',
    'VIBESHINE_PACKAGE_PROCESSOR MATCHES "^(aarch64|arm64)$"',
    'set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "arm64")',
):
    require(packaging, deb_arch_contract, "deterministic DEB architecture")
rpm_filelist_match = re.search(
    r"set\(CPACK_RPM_USER_FILELIST\s+(.*?)\n\s*\)", packaging, re.DOTALL
)
if rpm_filelist_match is None:
    raise AssertionError("native packaging is missing the deterministic CPack RPM file list")
rpm_filelist = rpm_filelist_match.group(1)
rpm_entries = (
    "%attr(0755,root,root) ${CMAKE_INSTALL_FULL_BINDIR}/vibepollo",
    "%attr(0755,root,root) ${VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR}/vibepollo-session-exec",
    "%attr(0700,root,root) %caps(cap_kill,cap_setgid,cap_setuid+p) ${VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR}/vibepollo-session-broker",
    "%attr(0755,root,root) ${VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR}/vibepollo-app-supervisor",
    "%attr(0755,root,root) ${VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR}/vibepollo-steam-launch",
    "%attr(0755,root,root) ${VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR}/vibepollo-kwin-session-environment",
    "%attr(0750,root,vibepollo) %caps(cap_sys_admin,cap_sys_nice+p) ${VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR}/vibepollo-host",
)
for rpm_entry in rpm_entries:
    require(rpm_filelist, rpm_entry, "deterministic CPack RPM file list")
if rpm_filelist.count("%caps(") != 2:
    raise AssertionError("CPack RPM metadata must grant capabilities only to the broker and private host")
for capability_free_path in (
    "${CMAKE_INSTALL_FULL_BINDIR}/vibepollo",
    "${VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR}/vibepollo-session-exec",
    "${VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR}/vibepollo-app-supervisor",
    "${VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR}/vibepollo-steam-launch",
    "${VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR}/vibepollo-kwin-session-environment",
):
    filelist_line = next(line for line in rpm_filelist.splitlines() if capability_free_path in line)
    forbid(filelist_line, "%caps(", f"capability-free RPM file {capability_free_path}")
for forbidden in ("pam_vibepollo", "vibepollo-machine-prepare", "vibepollo-session-handoff", "vibepollo-session-restore"):
    forbid(packaging, forbidden, "native packaging")
for dependency_file, label in ((arch_pkgbuild, "Arch dependencies"), (rpm, "RPM dependencies"), (packaging, "native dependencies")):
    forbid(dependency_file, "pam-devel", label)
    forbid(dependency_file, "libpam0g", label)
for lifecycle, label in ((arch_install, "Arch lifecycle"), (rpm, "RPM lifecycle"), (postinst, "native lifecycle")):
    require(lifecycle, "enable vibepollo-session-controller.service", label)
    require(lifecycle, "disable vibepollo.service", label)
    require(lifecycle, "remove-pam", label)
for lifecycle, label in ((arch_install, "Arch removal"), (rpm, "RPM removal"), (prerm, "native removal")):
    require(lifecycle, "vibepollo-session-controller", label)

# Every replacement/removal compatibility path uses one systemd snapshot plus
# the exact cgroup.events state, and converges over strictly validated broker
# instance names.  State labels and MainPID=0 alone are not revocation proof.
for lifecycle, label in (
    (postinst, "DEB postinst compatibility quiesce"),
    (prerm, "DEB removal quiesce"),
    (arch_install, "Arch lifecycle quiesce"),
    (rpm_post, "RPM post compatibility quiesce"),
    (rpm_preun, "RPM removal quiesce"),
):
    for invariant in (
        "--property=SubState",
        "--property=ControlGroup",
        "cgroup.events",
        "populated 0",
        "broker_unit_is_safe",
        "privileged_helper_is_safe",
        "ulimit -f 128",
        "65536",
        "mask --runtime",
        "vibepollo-session-exec.socket",
        "vibepollo.service",
    ):
        require(lifecycle, invariant, label)
    if "-lt 20" not in lifecycle and "attempt < 20" not in lifecycle:
        raise AssertionError(f"{label} lacks bounded broker convergence")
    if "-lt 5" not in lifecycle and "clean_passes < 5" not in lifecycle:
        raise AssertionError(f"{label} lacks sustained clean broker enumeration")
    forbid(lifecycle, "systemctl stop 'vibepollo-session-exec@*.service'", label)
    forbid(lifecycle, 'systemctl stop "vibepollo-session-exec@*.service"', label)
    forbid(lifecycle, "systemctl kill --kill-whom=all --signal=KILL", label)

# Pre-replacement hooks must quiesce both the immediately prior architecture
# and the controller/broker architecture without trusting systemctl globs.
for lifecycle, label in (
    (preinst, "DEB pre-replacement"),
    (arch_pre, "Arch pre-replacement"),
    (rpm_pre, "RPM pre-replacement"),
):
    normalized = " ".join(lifecycle.replace("\\\n", " ").split())
    for invariant in (
        "vibepollo_broker_socket_is_masked",
        "vibepollo_stop_brokers",
        "vibepollo_broker_unit_is_safe",
        "vibeshine-vkms-control.socket",
        "vibepollo_control_unit_is_safe",
        "vibepollo_control_instances_are_quiescent",
        "vibepollo_select_upgrade_kill_mode",
        "vibepollo_prepare_host_upgrade_fence",
        "vibepollo_activate_host_upgrade_fence",
        "RefuseManualStart=yes",
        "vibepollo_upgrade_kill_mode=process",
        "vibepollo_upgrade_kill_mode=control-group",
        "SendSIGKILL=no",
        "--property=Job",
        "systemctl freeze",
        "--kill-after=2 60 systemctl stop",
        "FreezerState=frozen",
        "frozen 1",
        "vibepollo_controller_remains_frozen",
        "vibepollo_thaw_controller",
        "systemctl thaw",
        "FreezerState=running",
        "vibepollo-session-controller.service",
        "vibepollo.service",
        "vibepollo-prelogin.service",
        "vibepollo-machine-prepare.service",
        "vibepollo_run_optional_legacy_command cleanup",
        "vibepollo_run_optional_legacy_command remove-pam",
        "mask --runtime",
        "vibepollo-session-restore@.service",
        "vibepollo_restore_template_is_masked",
        "vibepollo_host_unit_is_masked",
        "vibepollo_restore_unit_is_safe",
        "vibepollo_stop_restore_instances",
        "vibepollo_restore_instances_are_quiescent",
        "--property=ControlGroup",
        "populated 0",
        "vibepollo_unit_is_disabled",
        "/run/vibepollo/session-broker.sock",
        "/run/vibepollo/session-handoffs",
        "/run/vibepollo/session-restores",
        "/run/vibepollo/session-handoff.lock",
        "/run/vibepollo/prelogin-handoff-complete",
        "vibepollo_disable_legacy_handoff",
        "chmod 000",
        "grep -Fzxq",
        "vibepollo_wait_for_legacy_handoff",
        "flock --exclusive",
        "vibepollo_cleanup_legacy_transition_state",
        "LoadState=masked",
        "privileged_helper_is_safe",
        "ulimit -f 128",
        "65536",
    ):
        require(lifecycle, invariant, label)
    forbid(lifecycle, "systemctl stop 'vibepollo-session-exec@*.service'", label)
    forbid(lifecycle, 'systemctl stop "vibepollo-session-exec@*.service"', label)
    forbid(lifecycle, "systemctl stop 'vibepollo-session-restore@*.service'", label)
    forbid(lifecycle, 'systemctl stop "vibepollo-session-restore@*.service"', label)
    forbid(lifecycle, "systemctl unmask", label)
    forbid(lifecycle, "systemctl kill --kill-whom=all --signal=KILL", label)
    forbid(lifecycle, "vibepollo_session_record=", label)
    forbid(lifecycle, "vibepollo_legacy_acl=", label)
    function_name = "do_quiesce_machine_host" if label.startswith("Arch") else "vibepollo_quiesce_machine_host"
    quiesce_source = lifecycle.rsplit(f"\n{function_name}() {{\n", 1)[1].split("\n}\n", 1)[0]
    quiesce = " ".join(quiesce_source.replace("\\\n", " ").split())
    prepare_at = quiesce.index("vibepollo_prepare_host_upgrade_fence || return 1")
    freeze_at = quiesce.index("vibepollo_freeze_controller || return 1", prepare_at)
    activate_at = quiesce.index("vibepollo_activate_host_upgrade_fence || return 1", freeze_at)
    control_mask_at = quiesce.index("systemctl mask --runtime vibeshine-vkms-control.socket", activate_at)
    control_stop_at = quiesce.index("vibepollo_stop_exact_unit vibeshine-vkms-control.socket", control_mask_at)
    control_drain_at = quiesce.index("vibepollo_control_instances_are_quiescent || return 1", control_stop_at)
    restore_mask_at = quiesce.index("systemctl mask --runtime 'vibepollo-session-restore@.service'", activate_at)
    helper_close_at = quiesce.index("vibepollo_disable_legacy_handoff || return 1", restore_mask_at)
    broker_admission_at = quiesce.index("systemctl mask --runtime vibepollo-session-exec.socket", control_drain_at)
    broker_stop_at = quiesce.index("vibepollo_stop_exact_unit vibepollo-session-exec.socket", broker_admission_at)
    host_stop_at = quiesce.index("vibepollo_stop_exact_unit vibepollo.service", broker_stop_at)
    host_mask_at = quiesce.index("systemctl mask --runtime vibepollo.service", host_stop_at)
    first_restore_stop = quiesce.index("vibepollo_stop_restore_instances || return 1", host_mask_at)
    broker_drain_at = quiesce.index("vibepollo_stop_brokers || return 1", first_restore_stop)
    frozen_proof_at = quiesce.index("vibepollo_controller_remains_frozen || return 1", broker_drain_at)
    thaw_at = quiesce.index("vibepollo_thaw_controller || return 1", frozen_proof_at)
    controller_stop_at = quiesce.index("vibepollo_stop_exact_unit vibepollo-session-controller.service", thaw_at)
    remove_pam_at = normalized.index("vibepollo_run_optional_legacy_command remove-pam", first_restore_stop)
    last_restore_stop = normalized.rindex("vibepollo_stop_restore_instances || return 1")
    legacy_cleanup_at = normalized.rindex("vibepollo_cleanup_legacy_transition_state || return 1")
    if not (
        prepare_at
        < freeze_at
        < activate_at
        < restore_mask_at
        < helper_close_at
        < control_mask_at
        < control_stop_at
        < control_drain_at
        < broker_admission_at
        < broker_stop_at
        < host_stop_at
        < host_mask_at
        < first_restore_stop
        < broker_drain_at
        < frozen_proof_at
        < thaw_at
        < controller_stop_at
    ):
        raise AssertionError(f"{label} does not fence the old controller/host and drain GPU ownership in fail-closed order")
    if not remove_pam_at < last_restore_stop < legacy_cleanup_at:
        raise AssertionError(f"{label} does not finish legacy admission and state cleanup in fail-closed order")
    if "loaded | masked" not in lifecycle and "loaded|masked" not in lifecycle:
        raise AssertionError(f"{label} rejects masked legacy restore instances during exact cgroup cleanup")

for function_name in (
    "vibepollo_control_instances_are_quiescent",
    "vibepollo_stop_brokers",
    "vibepollo_stop_restore_instances",
    "vibepollo_restore_instances_are_quiescent",
):
    require(rpm_pre, f"{function_name}() (", "RPM pre-replacement subshell isolation")
require(rpm_post, "vibepollo_stop_brokers() (", "RPM post-replacement subshell isolation")
require(rpm_preun, "vibepollo_preun_stop_brokers() (", "RPM preun subshell isolation")

# Pacman runs the same install script before and after replacement. The second
# invocation must accept the fully masked, quiescent state left by the first
# instead of attempting to activate a drop-in through the runtime host mask.
for invariant in (
    "vibepollo_preserved_quiesce_is_safe",
    "vibepollo_runtime_root_is_safe_or_absent",
    "vibepollo_control_socket_is_masked",
    "vibepollo_broker_socket_is_masked",
    "vibepollo_host_unit_is_masked",
    "vibepollo_control_instances_are_quiescent",
    "vibepollo_brokers_are_quiescent",
    "vibepollo_restore_instances_are_quiescent",
    "! -e \"$vibepollo_broker_socket\"",
    "! -e \"$vibepollo_control_socket\"",
):
    require(arch_pre, invariant, "Arch preserved quiesce detection")
arch_quiesce = arch_pre.rsplit("\ndo_quiesce_machine_host() {\n", 1)[1].split("\n}\n", 1)[0]
preserved_at = arch_quiesce.index("vibepollo_preserved_quiesce_is_safe && return 0")
select_at = arch_quiesce.index("vibepollo_select_upgrade_kill_mode || return 1")
if not preserved_at < select_at:
    raise AssertionError("Arch post-upgrade does not recognize preserved quiesce before fence activation")
for command in (
    "systemctl daemon-reload || exit 1",
    ") || exit 1",
    "grep -qx 'RefuseManualStart=yes' || exit 1",
    'grep -qx "KillMode=$vibepollo_upgrade_kill_mode" || exit 1',
    "grep -qx 'SendSIGKILL=no' || exit 1",
):
    require(rpm_pre, command, "RPM upgrade-fence fail-closed activation")

# The host mask survives replacement until the new controller/socket are the
# next components activated. The obsolete restore template is never unmasked.
for lifecycle, label in (
    (postinst, "DEB post-replacement"),
    (arch_install, "Arch post-replacement"),
    (rpm_post, "RPM post-replacement"),
):
    for invariant in (
        "vibepollo_unmask_host_for_controller",
        "90-vibepollo-safe-upgrade.conf",
        "RefuseManualStart=no",
        "KillMode=control-group",
        "SendSIGKILL=no",
        "unmask --runtime vibeshine-vkms-control.socket",
        "start vibeshine-vkms-control.socket",
        "unmask --runtime vibepollo-session-exec.socket",
        "unmask --runtime vibepollo.service",
        "LoadState=loaded",
        "systemctl enable vibepollo-session-controller.service",
        "systemctl start vibepollo-session-controller.service",
        "cap_sys_admin,cap_sys_nice=p",
        "cap_kill,cap_setgid,cap_setuid=p",
        "vibepollo-host",
        "vibepollo-session-broker",
        "distinct inodes",
        "root:vibepollo",
        "0750",
        "0700",
    ):
        require(lifecycle, invariant, label)
    forbid(lifecycle, "enable vibepollo-session-exec.socket", label)
    forbid(lifecycle, "start vibepollo-session-exec.socket", label)
    forbid(lifecycle, "unmask --runtime vibepollo-session-restore@.service", label)
    normalized = " ".join(lifecycle.replace("\\\n", " ").split())
    enable_at = normalized.rindex("systemctl enable vibepollo-session-controller.service")
    unmask_at = normalized.rindex("! vibepollo_unmask_host_for_controller")
    start_at = normalized.rindex("systemctl start vibepollo-session-controller.service")
    if not enable_at < unmask_at < start_at:
        raise AssertionError(f"{label} does not unmask the host immediately before controller activation")

# Reset is a credential-revocation boundary too: it masks exact activation
# units and accepts shutdown only after the five-property snapshot and every
# exact broker cgroup report populated 0.
for invariant in (
    "quiesce_machine_for_reset",
    "control_group_is_empty",
    "system_unit_is_quiescent",
    "broker_instances_are_quiescent",
    "--property=SubState",
    "--property=ControlGroup",
    "cgroup.events",
    "populated 0",
    "systemctl mask --runtime \"$unit\"",
    "head -c 65537",
):
    require(host, invariant, "machine-host reset quiescence")
for unsafe_stop in (
    "systemctl stop 'vibepollo-session-exec@*.service'",
    'systemctl stop "vibepollo-session-exec@*.service"',
):
    forbid(host, unsafe_stop, "machine-host reset quiescence")

require(rpm, "%{_bindir}/vibepollo-mangohud", "RPM deterministic manifest")
require(rpm, "%attr(0755,root,root) %{_prefix}/libexec/vibeshine/vibepollo-app-supervisor", "RPM deterministic manifest")
require(rpm, "%attr(0755,root,root) %{_prefix}/libexec/vibeshine/vibepollo-steam-launch", "RPM deterministic manifest")
require(rpm, "%attr(0755,root,root) %{_prefix}/libexec/vibeshine/vibepollo-kwin-session-environment", "RPM deterministic manifest")
require(rpm, "%attr(0750,root,vibepollo) %caps(cap_sys_admin,cap_sys_nice+p) %{_prefix}/libexec/vibeshine/vibepollo-host", "RPM deterministic manifest")
require(rpm, "%attr(0700,root,root) %caps(cap_kill,cap_setgid,cap_setuid+p) %{_prefix}/libexec/vibeshine/vibepollo-session-broker", "RPM deterministic manifest")
require(rpm, "%global build_semver %{lua:", "RPM application SemVer")
require(rpm, "%global rpm_version %{lua:", "RPM native version normalization")
require(rpm, "Version: %{rpm_version}", "RPM native version normalization")
require(rpm, "export BUILD_VERSION=%{build_semver}", "RPM application SemVer")
for obsolete_version_contract in ("Version: %{build_version}", "BUILD_VERSION=v%{build_version}"):
    forbid(rpm, obsolete_version_contract, "RPM split application/package version")
forbid(rpm, "%{_userunitdir}/*.service", "RPM deterministic manifest")
for dropin in (
    "%{_userunitdir}/plasma-kwin_wayland.service.d/vibeshine-kwin-gpu.conf",
    "%{_userunitdir}/plasma-kwin_wayland.service.d/vibepollo-kwin-session-environment.conf",
    "%{_userunitdir}/plasma-login-kwin_wayland.service.d/vibeshine-kwin-gpu.conf",
    "%{_userunitdir}/plasma-login-kwin_wayland.service.d/vibepollo-kwin-session-environment.conf",
):
    require(rpm, dropin, "RPM deterministic manifest")
require(rpm, "%attr(4755,root,root) %{_libdir}/libvibeshine-kwin-gpu.so", "shared trusted GPU bridge manifest")
for lifecycle in (arch_install, postinst):
    require(lifecycle, "user.vibeshine.cap_sys_nice_removed", "shared previous GPU bridge marker")
    require(lifecycle, "vibepollo-kwin-capability.path vibeshine-kwin-capability.path", "legacy watcher retirement")
require(special_packaging, 'SUNSHINE_SERVICE_READINESS_COMMAND "ExecStartPre=/bin/sleep 5"', "ordinary user service")
forbid(special_packaging, "vibepollo-session-ready", "ordinary user service")

print("PASS: least-privilege Linux machine-session controller contract")
