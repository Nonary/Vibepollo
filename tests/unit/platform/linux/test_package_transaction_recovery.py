"""Private filesystem/model tests for real package recovery shell functions.

No package entrypoint is executed and every systemctl call is a shell model.
"""
import os
from pathlib import Path
import re
import signal
import shutil
import sys
import subprocess
import tempfile
import time
import unittest

ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[4]
HOOKS = ("packaging/linux/Arch/vibepollo.install", "packaging/linux/vibeshine-preinst.in", "packaging/linux/copr/Sunshine.spec")


def function(source, name, subshell=False):
    opener, closer = ("(", ")") if subshell else ("{", "}")
    match = re.search(r"^" + name + r"\(\) " + re.escape(opener) + r"\n.*?^" + re.escape(closer), source, re.M | re.S)
    assert match, name
    return match.group()


class PackageRecovery(unittest.TestCase):
    def model(self, hook, scenario, signal_name=None):
        with tempfile.TemporaryDirectory() as temp:
            stage = Path(temp)
            systemd = stage / "systemd"
            systemd.mkdir()
            runtime = stage / "runtime"
            runtime.mkdir()
            controller = stage / "freezer"
            controller.write_text("frozen" if scenario == "admin-frozen" else "running")
            (stage / "invocation").write_text("1" * 32)
            admin = systemd / "vibeshine-vkms-control.socket"
            admin.symlink_to("/dev/null")
            original_admin = admin.lstat().st_ino
            handoff = stage / "absent-handoff"
            if scenario.startswith("handoff-"):
                handoff.write_text("legacy fixture\n")
                handoff.chmod(0o755)
            original_handoff = handoff.stat().st_ino if handoff.exists() else None
            source = (ROOT / hook).read_text().replace("%%", "%")
            helpers = "# Package barriers are" + source.split("# Package barriers are", 1)[1].split("vibepollo_quiesce_step=''", 1)[0]
            # Preserve production validation and use this unprivileged fixture UID/GID.
            helpers = helpers.replace("0:0:", f"{os.getuid()}:{os.getgid()}:")
            helpers = helpers.replace("/run/systemd/system", str(systemd)).replace("/run/vibepollo-package-transaction", str(stage / "transaction")).replace("/run/vibepollo/session.env", str(runtime / "session.env"))
            wrapper = function(source, "vibepollo_quiesce_or_abort", True)
            abort = function(source, "vibepollo_abort_quiesce")
            quiesce_name = "do_quiesce_machine_host" if "/Arch/" in hook else "vibepollo_quiesce_machine_host"
            code = helpers + wrapper + "\n" + abort + "\n" + r'''
vibepollo_host_upgrade_dropin="$stage/fence"
vibepollo_controller_was_frozen=0
vibepollo_package_capture_host_executables() { echo "[]"; }
vibepollo_package_unmanaged_host_absent() { [ "$scenario" != unmanaged ]; }
vibepollo_legacy_handoff="$stage/absent-handoff"
vibepollo_legacy_handoff_directory="$stage/absent-handoffs"
vibepollo_legacy_restore_directory="$stage/absent-restores"
vibepollo_legacy_transition_lock="$stage/absent-lock"
vibepollo_legacy_prelogin_marker="$stage/absent-marker"
vibepollo_control_instances_are_quiescent() { return 0; }
vibepollo_brokers_are_quiescent() { return 0; }
vibepollo_restore_instances_are_quiescent() { return 0; }
vibepollo_broker_socket="$stage/absent-broker"
vibepollo_control_socket="$stage/absent-control"
vq() { vibepollo_quiesce_step=$*; "$@"; }
systemctl() {
  printf '%s\n' "$*" >>"$stage/actions"
  case "$*" in
    *--property=Job*) if [ "$scenario" = job ]; then echo Job=123; else echo Job=; fi ;;
    *--property=MainPID*) printf 'MainPID=111\nControlGroup=/controller\nFreezerState=%s\nInvocationID=%s\n' "$(cat "$stage/freezer")" "$(cat "$stage/invocation")"; [ ! -e "$stage/duplicate" ] || echo MainPID=111 ;;
    'mask --runtime '*) shift 2; for unit do [ -L "$stage/systemd/$unit" ] || ln -s /dev/null "$stage/systemd/$unit"; done ;;
    'thaw '*) echo running >"$stage/freezer" ;;
  esac
}
timeout() { shift; shift; "$@"; }
vibepollo_unit_is_quiescent() { [ "$scenario" != populated ]; }
vibepollo_host_is_stable_or_quiescent() { [ "$scenario" != unstable ]; }
vibepollo_thaw_controller() { systemctl thaw vibepollo-session-controller.service; vibepollo_controller_was_frozen=0; }
vibepollo_stop_exact_unit() { printf 'stop %s\n' "$*" >>"$stage/actions"; }
QUIESCE() {
  printf '[Unit]\nRefuseManualStart=yes\n' >"$stage/fence"
  chmod 0644 "$stage/fence"
  vibepollo_package_record_owned fence "$stage/fence" || return 1
  vibepollo_package_mask vibepollo.service vibeshine-vkms-control.socket 'vibepollo-session-restore@.service' || return 1
  echo frozen >"$stage/freezer"
  vibepollo_controller_was_frozen=1
  case "$scenario" in handoff-*) chmod 000 "$vibepollo_legacy_handoff" ;; esac
  case "$scenario" in
    handoff-replaced | handoff-replaced-early)
      printf 'replacement\n' >"$stage/replacement-handoff"
      chmod 000 "$stage/replacement-handoff"
      mv -f "$stage/replacement-handoff" "$vibepollo_legacy_handoff"
      ;;
    handoff-absent) rm -f "$vibepollo_legacy_handoff" ;;
  esac
  case "$scenario" in
    handoff-early | handoff-replaced-early) return 1 ;;
    success | cancelled | stale-record | populated | unmanaged | handoff-cancelled | handoff-replaced | handoff-failed | handoff-success | handoff-absent) vibepollo_package_mark_late; return 0 ;;
    reused) printf "22222222222222222222222222222222" >"$stage/invocation"; return 1 ;;
    duplicate) : >"$stage/duplicate"; return 1 ;;
    corrupt) chmod 0666 "$vibepollo_package_transaction/fence.owned"; return 1 ;;
    late) vibepollo_package_mark_late; vibepollo_shutdown_started=1; echo retained >"$stage/runtime/session.env"; return 1 ;;
    exit) exit 7 ;;
    signal) : >"$stage/ready"; sleep 1; return 1 ;;
    *) return 1 ;;
  esac
}
trap ':' HUP INT TERM
vibepollo_quiesce_or_abort
result=$?
case "$scenario" in
  cancelled | handoff-cancelled | handoff-replaced | handoff-absent) vibepollo_package_recover_failed_transaction; result=$? ;;
  handoff-failed) echo retained >"$stage/runtime/session.env"; vibepollo_package_recover_failed_transaction; result=$? ;;
esac
if [ "$scenario" = success ] || [ "$scenario" = stale-record ] || [ "$scenario" = populated ] || [ "$scenario" = handoff-success ]; then
  [ "$scenario" != stale-record ] || echo retained >"$stage/runtime/session.env"
  vibepollo_package_release
  result=$?
fi
exit "$result"
'''.replace("QUIESCE()", quiesce_name + "()")
            if scenario == "prepare-retry":
                prepare = function(source, "vibepollo_prepare_host_upgrade_fence", True)
                prepare = prepare.replace("0:0:", f"{os.getuid()}:{os.getgid()}:").replace("-o root -g root", "").replace("/run/vibepollo-host-upgrade.", str(stage / "fence-tmp."))
                preparation = r'''
QUIESCE() {
  vibepollo_host_upgrade_dropin_dir="$stage/fence-dir"
  vibepollo_host_upgrade_dropin="$vibepollo_host_upgrade_dropin_dir/fence"
  # Snapshot was captured before overriding this fixture path; it was absent.
  vibepollo_upgrade_kill_mode=mixed
  vibepollo_prepare_host_upgrade_fence || return 1
  first=$(stat -c '%d:%i' "$vibepollo_host_upgrade_dropin")
  vibepollo_prepare_host_upgrade_fence || return 1
  [ "$first" = "$(stat -c '%d:%i' "$vibepollo_host_upgrade_dropin")" ] || return 1
  vibepollo_upgrade_kill_mode=control-group
  vibepollo_prepare_host_upgrade_fence || return 1
  grep -qx KillMode=control-group "$vibepollo_host_upgrade_dropin" || return 1
  grep -qx Restart=no "$vibepollo_host_upgrade_dropin" || return 1
  vibepollo_package_mark_late
  printf 'PREPARE_RETRY_VERIFIED\n'
  return 0
}
'''.replace("QUIESCE()", quiesce_name + "()")
                code = code.replace("trap ':' HUP INT TERM\nvibepollo_quiesce_or_abort", prepare + "\n" + preparation + "trap ':' HUP INT TERM\nvibepollo_quiesce_or_abort")
            if scenario == "no-flock":
                code = code.replace("trap ':' HUP INT TERM\nvibepollo_quiesce_or_abort", "flock() { return 127; }\ntrap ':' HUP INT TERM\nvibepollo_quiesce_or_abort")
            env = dict(os.environ, stage=str(stage), scenario=scenario)
            shell = "bash" if "/Arch/" in hook else (shutil.which("dash") or "sh")
            if signal_name:
                proc = subprocess.Popen([shell, "-c", code], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
                for _ in range(100):
                    if (stage / "ready").exists():
                        break
                    if proc.poll() is not None:
                        break
                    time.sleep(.02)
                self.assertTrue((stage / "ready").exists())
                os.killpg(proc.pid, signal_name)
                out, err = proc.communicate(timeout=5)
                status = proc.returncode
            else:
                result = subprocess.run([shell, "-c", code], env=env, capture_output=True, text=True, timeout=5)
                out, err, status = result.stdout, result.stderr, result.returncode
            return dict(handoff_mode=(handoff.stat().st_mode & 0o777) if handoff.exists() else None, handoff_same=handoff.exists() and handoff.stat().st_ino == original_handoff, status=status, output=out + err, fence=(stage / "fence").exists(), own_mask=(systemd / "vibepollo.service").is_symlink(), admin_preserved=admin.is_symlink() and admin.lstat().st_ino == original_admin, transaction=(stage / "transaction").exists(), records=[(x.name, oct(x.stat().st_mode & 0o777), x.read_text()) for x in (stage / "transaction").glob("*")] if (stage / "transaction").exists() else [], freezer=controller.read_text().strip(), actions=(stage / "actions").read_text() if (stage / "actions").exists() else "", legacy=(systemd / "vibepollo-session-restore@.service").is_symlink())

    def test_early_failure_restores_owned_barriers_and_preserves_admin_mask(self):
        for hook in HOOKS:
            with self.subTest(hook=hook):
                result = self.model(hook, "early")
                self.assertNotEqual(result["status"], 0, result["output"])
                self.assertFalse(result["fence"], result["output"])
                self.assertFalse(result["own_mask"])
                self.assertTrue(result["admin_preserved"])
                self.assertFalse(result["transaction"])
                self.assertEqual(result["freezer"], "running")
                self.assertNotIn("stop vibepollo.service", result["actions"])

    def test_exit_and_catchable_signals_run_early_recovery(self):
        for hook in HOOKS:
            for sig in (None, signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
                with self.subTest(hook=hook, signal=sig):
                    result = self.model(hook, "signal" if sig else "exit", sig)
                    self.assertNotEqual(result["status"], 0)
                    self.assertFalse(result["fence"], result["output"])
                    self.assertFalse(result["own_mask"])
                    self.assertTrue(result["admin_preserved"])

    def test_uncertain_job_or_unstable_host_retains_owned_fences(self):
        for hook in HOOKS:
            for scenario in ("job", "unstable", "late", "reused", "duplicate"):
                with self.subTest(hook=hook, scenario=scenario):
                    result = self.model(hook, scenario)
                    self.assertTrue(result["fence"], result["output"])
                    self.assertTrue(result["transaction"])
                    self.assertTrue(result["admin_preserved"])

    def test_success_release_keeps_admin_mask_and_obsolete_admission_guard(self):
        for hook in HOOKS:
            with self.subTest(hook=hook):
                result = self.model(hook, "success")
                self.assertEqual(result["status"], 0, result["output"])
                self.assertFalse(result["own_mask"])
                self.assertFalse(result["fence"])
                self.assertFalse(result["transaction"])
                self.assertTrue(result["admin_preserved"])
                self.assertTrue(result["legacy"])
                self.assertNotIn("start ", result["actions"])

    def test_cancelled_completed_preflight_releases_owned_fences_without_starts(self):
        for hook in HOOKS:
            with self.subTest(hook=hook):
                result = self.model(hook, "cancelled")
                self.assertEqual(result["status"], 0, result["output"])
                self.assertFalse(result["fence"])
                self.assertFalse(result["own_mask"])
                self.assertFalse(result["transaction"])
                self.assertTrue(result["admin_preserved"])
                self.assertFalse(result["legacy"])
                self.assertNotIn("start ", result["actions"])

    def test_corrupt_ownership_record_blocks_automatic_removal(self):
        for hook in HOOKS:
            with self.subTest(hook=hook):
                result = self.model(hook, "corrupt")
                self.assertNotEqual(result["status"], 0)
                self.assertTrue(result["fence"])
                self.assertTrue(result["transaction"])
                self.assertTrue(result["admin_preserved"])

    def test_prepare_is_idempotent_and_reselects_mode_only_after_host_drain(self):
        for hook in HOOKS:
            with self.subTest(hook=hook):
                result = self.model(hook, "prepare-retry")
                self.assertEqual(result["status"], 0, result["output"])
                self.assertIn("PREPARE_RETRY_VERIFIED", result["output"])

    def test_preexisting_controller_freeze_is_preserved(self):
        for hook in HOOKS:
            with self.subTest(hook=hook):
                result = self.model(hook, "admin-frozen")
                self.assertNotEqual(result["status"], 0)
                self.assertFalse(result["fence"])
                self.assertEqual(result["freezer"], "frozen")
                self.assertNotIn("thaw ", result["actions"])

    def test_missing_transaction_lock_tool_makes_no_barrier_changes(self):
        for hook in HOOKS:
            with self.subTest(hook=hook):
                result = self.model(hook, "no-flock")
                self.assertNotEqual(result["status"], 0)
                self.assertFalse(result["fence"])
                self.assertFalse(result["own_mask"])
                self.assertFalse(result["transaction"])
                self.assertTrue(result["admin_preserved"])

    def test_absent_service_fence_activation_requires_quiescent_proof(self):
        for hook in HOOKS:
            source = (ROOT / hook).read_text().replace("%%", "%")
            activation = function(source, "vibepollo_activate_host_upgrade_fence")
            shell = "bash" if "/Arch/" in hook else (shutil.which("dash") or "sh")
            for quiet in (0, 1):
                with self.subTest(hook=hook, quiet=quiet):
                    code = activation + "\n" + r'''
vibepollo_package_open_owned_host_for_stop() { return 0; }
systemctl() { if [ "$1" = show ]; then printf 'LoadState=not-found\nRefuseManualStart=no\nKillMode=control-group\nSendSIGKILL=yes\nRestart=no\n'; fi; }
timeout() { shift 2; "$@"; }
vibepollo_unit_is_quiescent() { [ "$quiet" = 1 ]; }
vibepollo_activate_host_upgrade_fence
'''
                    result = subprocess.run([shell, "-c", code], env=dict(os.environ, quiet=str(quiet)), capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode == 0, quiet == 1, result.stderr)

    def test_failed_transactions_restore_only_the_original_handoff_inode(self):
        for hook in HOOKS:
            for scenario in ("handoff-early", "handoff-cancelled", "handoff-replaced", "handoff-replaced-early", "handoff-absent", "handoff-failed", "handoff-success"):
                with self.subTest(hook=hook, scenario=scenario):
                    result = self.model(hook, scenario)
                    if scenario in ("handoff-early", "handoff-cancelled"):
                        self.assertEqual(result["handoff_mode"], 0o755, result["output"])
                        self.assertTrue(result["handoff_same"])
                        self.assertFalse(result["transaction"])
                    elif scenario == "handoff-absent":
                        self.assertIsNone(result["handoff_mode"])
                        self.assertEqual(result["status"], 0, result["output"])
                    else:
                        self.assertEqual(result["handoff_mode"], 0, result["output"])
                    if scenario == "handoff-failed":
                        self.assertNotEqual(result["status"], 0)
                        self.assertTrue(result["transaction"])
                        self.assertTrue(result["fence"])
                    if scenario.startswith("handoff-replaced"):
                        self.assertFalse(result["handoff_same"])
                        self.assertFalse(result["transaction"])

    def record_fixture_code(self, hook, stage):
        source = (ROOT / hook).read_text().replace("%%", "%")
        helpers = "# Package barriers are" + source.split("# Package barriers are", 1)[1].split("vibepollo_quiesce_step=''", 1)[0]
        helpers = helpers.replace("0:0:", f"{os.getuid()}:{os.getgid()}:")
        helpers = helpers.replace("/run/systemd/system", str(stage / "systemd")).replace("/run/vibepollo-package-transaction", str(stage / "transaction")).replace("/run/vibepollo/session.env", str(stage / "runtime/session.env"))
        return helpers + r"""
vibepollo_host_upgrade_dropin="$stage/fence"
vibepollo_legacy_handoff="$stage/absent-handoff"
vibepollo_package_capture_host_executables() { echo '[]'; }
vibepollo_package_unmanaged_host_absent() { return 0; }
systemctl() {
  case "$*" in
    *--property=Job*) echo Job= ;;
    *--property=MainPID*) printf 'MainPID=0\nControlGroup=\nFreezerState=\nInvocationID=\n' ;;
  esac
}
timeout() { shift 2; "$@"; }
vibepollo_host_is_stable_or_quiescent() { return 0; }
"""

    def test_stale_record_temporaries_are_cleaned_under_the_exclusive_lock(self):
        for hook in HOOKS:
            for kind in ("empty", "partial", "malformed", "symlink", "mode", "owner", "hardlink", "unknown"):
                with self.subTest(hook=hook, kind=kind), tempfile.TemporaryDirectory() as temp:
                    stage = Path(temp)
                    (stage / "systemd").mkdir()
                    (stage / "runtime").mkdir()
                    sentinel = stage / "unrelated-data"
                    sentinel.write_bytes(b"must remain unchanged")
                    code = self.record_fixture_code(hook, stage)
                    env = dict(os.environ, stage=str(stage))
                    initial = subprocess.run(["bash", "-c", code + "\nvibepollo_package_begin\n"], env=env, capture_output=True, text=True, timeout=5)
                    self.assertEqual(initial.returncode, 0, initial.stderr)
                    record_dir = stage / "transaction"
                    original = {x.name: x.read_bytes() for x in record_dir.glob("*") if not x.name.startswith(".record.")}
                    name = ".record.bad" if kind == "malformed" else "unknown" if kind == "unknown" else ".record.Abc123"
                    stale = record_dir / name
                    if kind == "symlink":
                        stale.symlink_to(sentinel)
                    elif kind == "hardlink":
                        os.link(sentinel, stale)
                    else:
                        stale.write_bytes(b"" if kind == "empty" else b"partial unpublished record")
                        stale.chmod(0o644 if kind == "mode" else 0o600)
                    if kind == "owner":
                        code += r"""
stat() {
  case "$*" in
    *'%u:%g:%a:%h:%F'*'.record.Abc123'*) printf '9999:9999:600:1:regular file\n'; return 0 ;;
  esac
  command stat "$@"
}
"""
                    result = subprocess.run(["bash", "-c", code + "\nvibepollo_package_begin\n"], env=env, capture_output=True, text=True, timeout=5)
                    if kind in ("empty", "partial"):
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertFalse(stale.exists())
                    else:
                        self.assertNotEqual(result.returncode, 0, result.stderr)
                        self.assertTrue(stale.exists() or stale.is_symlink())
                    self.assertEqual(sentinel.read_bytes(), b"must remain unchanged")
                    for name, value in original.items():
                        self.assertEqual((record_dir / name).read_bytes(), value)

    def test_sigkill_during_real_record_publication_can_retry(self):
        for hook in HOOKS:
            for point in ("empty", "partial"):
                with self.subTest(hook=hook, point=point), tempfile.TemporaryDirectory() as temp:
                    stage = Path(temp)
                    (stage / "systemd").mkdir()
                    (stage / "runtime").mkdir()
                    code = self.record_fixture_code(hook, stage)
                    # The intercepted command runs after the real mktemp. The
                    # writer blocks in the shell's builtin read, so killing its
                    # private process group leaves no descendant holding fd 8.
                    pause_command = "chmod" if point == "empty" else "mv"
                    writer = code + r"""
vibepollo_package_begin || exit 1
mkfifo "$stage/pause"
exec 9<>"$stage/pause"
""" + pause_command + r"""() {
  : >"$stage/ready"
  IFS= read -r paused <"$stage/pause"
}
vibepollo_package_write_record phase late
"""
                    env = dict(os.environ, stage=str(stage))
                    proc = subprocess.Popen(["bash", "-c", writer], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
                    for _ in range(200):
                        if (stage / "ready").exists() or proc.poll() is not None:
                            break
                        time.sleep(.01)
                    try:
                        self.assertTrue((stage / "ready").exists())
                        record_dir = stage / "transaction"
                        original = {x.name: x.read_bytes() for x in record_dir.glob("*") if not x.name.startswith(".record.")}
                        temporary = list(record_dir.glob(".record.*"))
                        self.assertEqual(len(temporary), 1)
                        self.assertEqual(temporary[0].read_bytes(), b"" if point == "empty" else b"late\n")
                        # A live publisher (including its inherited fd 8) owns
                        # the transaction; another invocation cannot clear it.
                        blocked = subprocess.run(["bash", "-c", code + "\nvibepollo_package_begin\n"], env=env, capture_output=True, text=True, timeout=5)
                        self.assertNotEqual(blocked.returncode, 0)
                        self.assertTrue(temporary[0].exists())
                    finally:
                        if proc.poll() is None:
                            os.killpg(proc.pid, signal.SIGKILL)
                        proc.communicate(timeout=5)
                    result = subprocess.run(["bash", "-c", code + "\nvibepollo_package_begin\n"], env=env, capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(list(record_dir.glob(".record.*")), [])
                    self.assertEqual({x.name: x.read_bytes() for x in record_dir.glob("*") if not x.name.startswith(".record.")}, original)

    def test_retired_and_incomplete_initialization_retry(self):
        for hook in HOOKS:
            for kind in ("no-phase", "retired", "retired-no-lock", "retired-empty"):
                with self.subTest(hook=hook, kind=kind), tempfile.TemporaryDirectory() as tmp:
                    stage = Path(tmp)
                    (stage / "systemd").mkdir()
                    (stage / "runtime").mkdir()
                    code = self.record_fixture_code(hook, stage)
                    env = dict(os.environ, stage=tmp)
                    bootstrap = subprocess.run(["bash", "-c", code + "\nvibepollo_package_begin"], env=env, capture_output=True, text=True)
                    self.assertEqual(bootstrap.returncode, 0, bootstrap.stderr)
                    transaction = stage / "transaction"
                    if kind == "no-phase":
                        (transaction / "phase").unlink()
                    else:
                        retired = stage / "transaction.retired"
                        transaction.rename(retired)
                        if kind == "retired-no-lock": (retired / "lock").unlink()
                        if kind == "retired-empty":
                            for path in retired.iterdir(): path.unlink()
                    result = subprocess.run([shutil.which("dash") or "sh", "-c", code + "\nvibepollo_package_begin"], env=env, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertFalse((stage / "transaction.retired").exists())
                    self.assertEqual((transaction / "phase").read_text(), "early\n")

    def test_force_reader_matches_exact_operation_and_failure_class(self):
        for hook in HOOKS:
            with self.subTest(hook=hook), tempfile.TemporaryDirectory() as tmp:
                stage = Path(tmp)
                (stage / "systemd").mkdir()
                (stage / "runtime").mkdir()
                code = self.record_fixture_code(hook, stage) + r"""
VIBEPOLLO_PACKAGE_RUN_ID=11111111111111111111111111111111
vibepollo_package_begin || exit 91
vibepollo_package_force_is_applicable && exit 92
vibepollo_package_write_record phase late || exit 93
vibepollo_package_write_record failure-class host-stop-deadline || exit 94
vibepollo_package_force_is_applicable || exit 95
VIBEPOLLO_PACKAGE_RUN_ID=22222222222222222222222222222222
vibepollo_package_force_is_applicable && exit 96
vibepollo_package_write_record host-stop.before original-invocation || exit 97
vibepollo_package_begin || exit 98
[ ! -e "$stage/transaction/host-stop.before" ] || exit 99
[ "$(cat "$stage/transaction/phase")" = early ] || exit 100
vibepollo_package_write_record host-stop.before same-run-invocation || exit 101
vibepollo_package_begin || exit 102
[ "$(cat "$stage/transaction/host-stop.before")" = same-run-invocation ] || exit 103
exit 0
"""
                result = subprocess.run([shutil.which("dash") or "sh", "-c", code], env=dict(os.environ, stage=tmp), capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_control_quiescence_success_inside_dash_exit_trap(self):
        shell = shutil.which("dash")
        if not shell: self.skipTest("dash unavailable")
        for hook in HOOKS:
            source = (ROOT / hook).read_text().replace("%%", "%")
            original_helper = function(source, "vibepollo_control_instances_are_quiescent", True)
            for exit_status in (1, 129, 130, 143):
                with self.subTest(hook=hook, exit_status=exit_status), tempfile.TemporaryDirectory() as tmp:
                    stage = Path(tmp)
                    helper = original_helper.replace("/run/vibepollo-control-units.", str(stage / "units."))
                    code = helper + r"""
vibepollo_control_socket="$stage/absent"
vibepollo_capture_unit_list() { : >"$1"; }
vibepollo_bounded_unit_list() { return 0; }
sleep() { :; }
trap 'vibepollo_control_instances_are_quiescent; printf "%s\n" "$?" >"$stage/proof"' 0
exit "$initial"
"""
                    result = subprocess.run(["bash" if "/Arch/" in hook else shell, "-c", code], env=dict(os.environ, stage=tmp, initial=str(exit_status)), capture_output=True, text=True)
                    self.assertEqual(result.returncode, exit_status, result.stderr)
                    self.assertEqual((stage / "proof").read_text(), "0\n")

    def test_exact_drained_legacy_signature_can_be_adopted(self):
        for hook in HOOKS:
            for kind in ("exact", "changed", "populated"):
                with self.subTest(hook=hook, kind=kind), tempfile.TemporaryDirectory() as tmp:
                    stage = Path(tmp); (stage / "systemd").mkdir(); (stage / "runtime").mkdir()
                    fence = stage / "fence"
                    fence.write_text("[Unit]\nRefuseManualStart=yes\n\n[Service]\nKillMode=mixed\nSendSIGKILL=no\n" + ("# administrator\n" if kind == "changed" else ""))
                    fence.chmod(0o644)
                    for unit in ("vibepollo.service", "vibepollo-session-exec.socket", "vibeshine-vkms-control.socket", "vibepollo-session-restore@.service"):
                        (stage / "systemd" / unit).symlink_to("/dev/null")
                    code = self.record_fixture_code(hook, stage) + r"""
vibepollo_package_complete_quiescence_is_safe() { [ "$kind" != populated ]; }
vibepollo_package_begin || exit 91
if [ "$kind" = exact ]; then
  [ "$(cat "$stage/transaction/phase")" = preflight ] || exit 92
  for name in fence vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
    [ "$(cat "$stage/transaction/$name.before")" = absent ] && [ -f "$stage/transaction/$name.owned" ] || exit 93
  done
else
  [ ! -e "$stage/transaction/fence.owned" ] || exit 94
fi
"""
                    result = subprocess.run([shutil.which("dash") or "sh", "-c", code], env=dict(os.environ, stage=tmp, kind=kind), capture_output=True, text=True)
                    self.assertEqual(result.returncode == 0, kind != "populated", result.stderr)

    def test_missing_owned_barrier_can_retry_but_replaced_inode_blocks(self):
        for hook in HOOKS:
            for kind in ("missing", "replacement"):
                with self.subTest(hook=hook, kind=kind), tempfile.TemporaryDirectory() as tmp:
                    stage=Path(tmp); (stage / "systemd").mkdir(); (stage / "runtime").mkdir()
                    code=self.record_fixture_code(hook,stage)+r"""
vibepollo_package_begin || exit 91
printf 'owned fence\n' >"$stage/fence"
chmod 644 "$stage/fence"
vibepollo_package_record_owned fence "$stage/fence" || exit 92
# Keep an existing alternate inode, so inode reuse cannot hide replacement.
printf replacement >"$stage/replacement"
rm "$stage/fence"
[ "$kind" != replacement ] || mv "$stage/replacement" "$stage/fence"
vibepollo_package_begin
"""
                    result=subprocess.run([shutil.which("dash") or "sh","-c",code],env=dict(os.environ,stage=tmp,kind=kind),capture_output=True,text=True)
                    self.assertEqual(result.returncode==0,kind=="missing",result.stderr)

    def test_sigkill_during_atomic_forget_and_gc_can_retry(self):
        for hook in HOOKS:
            for point in ("renamed", "lock-removed", "empty"):
                with self.subTest(hook=hook, point=point), tempfile.TemporaryDirectory() as tmp:
                    stage=Path(tmp);(stage / "systemd").mkdir();(stage / "runtime").mkdir()
                    code=self.record_fixture_code(hook,stage)
                    writer=code+r"""
vibepollo_package_begin || exit 91
mkfifo "$stage/pause"
exec 9<>"$stage/pause"
pause_gc() { : >"$stage/ready"; IFS= read -r paused <"$stage/pause"; }
rm() {
  if [ "$point" = renamed ] && [ -d "$stage/transaction.retired" ]; then pause_gc; fi
  command rm "$@" || return 1
  case "$*" in *transaction.retired/lock) [ "$point" != lock-removed ] || pause_gc ;; esac
}
rmdir() { [ "$point" != empty ] || pause_gc; command rmdir "$@"; }
vibepollo_package_forget
"""
                    env=dict(os.environ,stage=tmp,point=point)
                    proc=subprocess.Popen(["bash","-c",writer],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
                    try:
                        for _ in range(200):
                            if (stage / "ready").exists() or proc.poll() is not None:break
                            time.sleep(.01)
                        self.assertTrue((stage / "ready").exists())
                        self.assertFalse((stage / "transaction").exists())
                        self.assertTrue((stage / "transaction.retired").is_dir())
                    finally:
                        if proc.poll() is None:os.killpg(proc.pid,signal.SIGKILL)
                        proc.communicate(timeout=5)
                    result=subprocess.run([shutil.which("dash") or "sh","-c",code+"\nvibepollo_package_begin"],env=env,capture_output=True,text=True)
                    self.assertEqual(result.returncode,0,result.stderr)
                    self.assertFalse((stage / "transaction.retired").exists())
                    self.assertEqual((stage / "transaction/phase").read_text(),"early\n")

    def test_stale_generation_record_or_populated_host_blocks_release(self):
        for hook in HOOKS:
            for scenario in ("stale-record", "populated", "unmanaged"):
                with self.subTest(hook=hook, scenario=scenario):
                    result = self.model(hook, scenario)
                    self.assertNotEqual(result["status"], 0)
                    self.assertTrue(result["fence"], result["output"])
                    self.assertTrue(result["own_mask"])
                    self.assertTrue(result["transaction"])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
