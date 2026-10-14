#!/usr/bin/env python3
"""Incoming guard versus real historical selector; explicit force retry policy.

Only private archives, hooks, functions, and simulated package operations run.
"""
import pathlib
import os
import re
import shlex
import signal
import subprocess
import sys
import tempfile
import time

root = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[4]
brand = sys.argv[2] if len(sys.argv) > 2 else "vibepollo"
assert brand in ("vibepollo", "vibeshine")
installer = root / "scripts/linux_install.sh"
migration = root / "scripts/linux/migrations/upgrade-2.0-lifecycle.sh"


def run(body):
    completed = subprocess.run(["bash", "-c", f'source {shlex.quote(str(installer))}\n' + body], text=True, capture_output=True)
    assert completed.returncode == 0, completed.stdout + completed.stderr


with tempfile.TemporaryDirectory(prefix=f"{brand}-installer-compat-") as directory:
    base = pathlib.Path(directory)
    # Historical published selector, not a fabricated always-failing guard.
    historical = (root / "tests/unit/platform/linux/fixtures/package-2.0.0/arch-selector.sh").read_text().replace("vibepollo", brand)
    outgoing = base / "outgoing"
    helper = base / "unknown-helper"
    helper.write_text("#!/bin/bash\n# historical unrecognized helper\n")
    helper.chmod(0o755)
    outgoing.write_text(historical + f"\n{brand}_legacy_host={shlex.quote(str(helper))}\n" +
                        "stat() { printf '%s\\n' '0:0:755:1:regular file'; }\n" +
                        f"{brand}_unit_is_quiescent() {{ return 0; }}\n" +
                        f"{brand}_quiesce_or_abort() {{ {brand}_select_upgrade_kill_mode; }}\n" +
                        "# old root metadata comparison: %F regular file\n")
    old_archive = base / "old.pkg.tar"
    subprocess.run(["bsdtar", "-cf", str(old_archive), "-C", str(base), "outgoing"], check=True)
    outgoing.write_bytes(subprocess.check_output(["bsdtar", "-xOf", str(old_archive), "outgoing"]))
    # It really reproduces the old idle rejection before incoming override.
    old = subprocess.run(["bash", "-c", f'. {shlex.quote(str(outgoing))}; {brand}_quiesce_or_abort'])
    assert old.returncode == 1
    archive_tree = base / "archive"
    migration_entry = archive_tree / f"usr/share/{brand}/installer-migrations/upgrade-2.0-lifecycle.sh"
    migration_entry.parent.mkdir(parents=True)
    migration_entry.write_text(migration.read_text())
    library = archive_tree / ".INSTALL"
    incoming_source = (root / f"packaging/linux/Arch/{brand}.install").read_text()
    incoming_functions = '\n'.join(re.search(rf"^{brand}_{suffix}\(\) \{{\n.*?^\}}$", incoming_source, re.M | re.S).group()
                                   for suffix in ['unit_is_quiescent', 'select_upgrade_kill_mode'])
    library.write_text("export LC_ALL=C\n" + incoming_functions + f'''
{brand}_cgroup_is_quiescent() {{ [ "$1" = /fixture.slice/{brand}.service ]; }}
systemctl() {{
  [ "$1" = show ] && [ "$2" = {brand}.service ] || return 99
  case "${{FIXTURE_HOST_ACTIVE:-0}}" in
    0) printf '%s\\n' LoadState=loaded ActiveState=inactive SubState=dead MainPID=0 ControlGroup=/fixture.slice/{brand}.service ;;
    *) printf '%s\\n' LoadState=loaded ActiveState=active SubState=running MainPID=321 ControlGroup=/fixture.slice/{brand}.service ;;
  esac
}}
timeout() {{ shift 2; "$@"; }}
{brand}_quiesce_or_abort() (
  {brand}_unit_is_quiescent {brand}.service || return 1
  {brand}_select_upgrade_kill_mode || return 1
  if [ -n "${{REMOVAL_TRACE:-}}" ]; then printf 'incoming proof\\n' >> "$REMOVAL_TRACE"; fi
)
{brand}_package_run_is_owned() {{ [ -n "${{{brand.upper()}_PACKAGE_RUN_ID:-}}" ]; }}
{brand}_package_recover_failed_transaction() {{
  if [ -n "${{REMOVAL_TRACE:-}}" ] && grep -qx remove "$REMOVAL_TRACE"; then
    printf 'release owned barriers\\n' >> "$REMOVAL_TRACE"
  fi
}}
''')
    candidate = base / "candidate.pkg.tar"
    subprocess.run(["bsdtar", "-cf", str(candidate), "-C", str(archive_tree), ".INSTALL", "usr"], check=True)
    # Installed library is read-only fixture; source only the trusted incoming
    # migration. The private same-name hook executes incoming safety authority.
    run(f'''
workdir=$(mktemp -d {shlex.quote(str(base))}/stage.XXXXXX)
pacman() {{ [ "$*" = '-Q {brand}' ] && printf '%s\\n' '{brand} 2.0.0-1'; }}
stage_upgrade_guard {shlex.quote(str(candidate))} {shlex.quote(str(outgoing))}
grep -Fxq AbortOnFail "$workdir/hooks/00-{brand}-quiesce.hook"
"$workdir/preflight"
source {shlex.quote(str(migration))}
first=$({brand}_upgrade_2_0_lifecycle_compat '{brand} 2.0.0-1' {shlex.quote(str(outgoing))} "$workdir/arch-package-hooks")
second=$({brand}_upgrade_2_0_lifecycle_compat '{brand} 2.0.0-1' {shlex.quote(str(outgoing))} "$workdir/arch-package-hooks")
[[ "$first" == "$second" && "$first" == *upgrade-2.0-lifecycle-v1* ]]
epoch=$({brand}_upgrade_2_0_lifecycle_compat '{brand} 1:2.0.0-1' {shlex.quote(str(outgoing))} "$workdir/arch-package-hooks")
[[ "$epoch" == *upgrade-2.0-lifecycle-v1* ]]
[[ -z "$({brand}_upgrade_2_0_lifecycle_compat '{brand} 2.1.0-1' {shlex.quote(str(outgoing))} "$workdir/arch-package-hooks")" ]]
[[ -z "$({brand}_upgrade_2_0_lifecycle_compat '{brand} 2.0.0-1' "$workdir/arch-package-hooks" "$workdir/arch-package-hooks")" ]]
# Modern/unknown versions get a distinct incoming hook, retaining the installed
# hook's security gate. Re-stage must remove the previous legacy-only override.
pacman() {{ [ "$*" = '-Q {brand}' ] && printf '%s\\n' '{brand} 2.1.0-1'; }}
stage_upgrade_guard {shlex.quote(str(candidate))} {shlex.quote(str(outgoing))}
[[ ! -e "$workdir/hooks/00-{brand}-quiesce.hook" ]]
[[ -f "$workdir/hooks/00-{brand}-incoming-quiesce.hook" ]]
"$workdir/preflight"
if (source {shlex.quote(str(outgoing))}; {brand}_quiesce_or_abort); then exit 1; fi
pacman() {{ [ "$*" = '-Q {brand}' ] && printf '%s\\n' '{brand} 1:2.0.0-1'; }}
stage_upgrade_guard {shlex.quote(str(candidate))} {shlex.quote(str(outgoing))}
[[ -f "$workdir/hooks/00-{brand}-quiesce.hook" && ! -e "$workdir/hooks/00-{brand}-incoming-quiesce.hook" ]]
# Normal failure is still fatal; an incoming guard can request explicit force.
printf '%s\\n' '{brand}_quiesce_or_abort() {{ [ "${{{brand.upper()}_PACKAGE_FORCE:-0}}" = 1 ]; }}' > "$workdir/arch-package-hooks"
if "$workdir/preflight"; then exit 1; fi
{brand.upper()}_PACKAGE_FORCE=1 "$workdir/preflight"
''')
    # Removal takes the explicit incoming archive as policy only. The outgoing
    # selector above remains broken, while the exact overriding guard runs
    # first and verifies real unit-property parsing before any removal step.
    removal_trace = base / "removal-trace"
    run(f'''
REMOVAL_TRACE={shlex.quote(str(removal_trace))}
export REMOVAL_TRACE
require_root() {{ return 0; }}
pacman-conf() {{ printf '%s\\n' /etc/pacman.d/hooks; }}
require_pacman() {{ return 0; }}
run_checks() {{ die 'removal must not run installation checks'; }}
install_kernel_headers() {{ die 'removal must not install headers'; }}
pacman() {{
  case "$1" in
    -Qp) printf '%s\\n' '{brand} 2.1.0-1' ;;
    -Q) printf '%s\\n' '{brand} 2.0.0-1' ;;
    -R)
      shift
      while [ "$1" != --hookdir ]; do shift; done
      shift 2
      while [ "$1" != --hookdir ]; do shift; done
      hook_dir=$2
      preflight=$(sed -n 's/^Exec = //p' "$hook_dir/00-{brand}-incoming-quiesce.hook")
      "$preflight" || return
      printf 'remove\\n' >> "$REMOVAL_TRACE"
      ;;
    *) return 99 ;;
  esac
}}
main --remove --yes --package {shlex.quote(str(candidate))}
[[ $(cat "$REMOVAL_TRACE") == $'incoming proof\\nremove\\nrelease owned barriers' ]]
''')
    # A live host fails the incoming proof and cannot reach simulated removal.
    run(f'''
REMOVAL_TRACE={shlex.quote(str(removal_trace))}
: > "$REMOVAL_TRACE"
export REMOVAL_TRACE FIXTURE_HOST_ACTIVE=1
require_root() {{ return 0; }}
pacman-conf() {{ printf '%s\\n' /etc/pacman.d/hooks; }}
require_pacman() {{ return 0; }}
pacman() {{
  case "$1" in
    -Qp) printf '%s\\n' '{brand} 2.1.0-1' ;;
    -Q) printf '%s\\n' '{brand} 2.0.0-1' ;;
    -R)
      shift
      while [ "$1" != --hookdir ]; do shift; done
      shift 2
      while [ "$1" != --hookdir ]; do shift; done
      preflight=$(sed -n 's/^Exec = //p' "$2/00-{brand}-incoming-quiesce.hook")
      "$preflight" || return
      printf 'remove\\n' >> "$REMOVAL_TRACE"
      ;;
    *) return 99 ;;
  esac
}}
if main --remove --yes --package {shlex.quote(str(candidate))}; then exit 1; fi
[[ ! -s "$REMOVAL_TRACE" ]]
''')
    run(f'''
require_root() {{ return 0; }}
pacman-conf() {{ printf '%s\\n' /etc/pacman.d/hooks; }}
require_pacman() {{ return 0; }}
pacman() {{ [ "$1" = -Qp ] && printf '%s\\n' 'unrelated 2.1.0-1'; }}
if (main --remove); then exit 1; fi
if (main --remove --package {shlex.quote(str(candidate))}); then exit 1; fi
''')

for name, flags, tty, response, expected in [
    ("unattended-default", "", 0, "", "0"),
    ("yes-refuses", "--yes", 1, "y", "0"),
    ("explicit-force", "--yes --force-stop", 0, "", "1"),
    ("interactive-refuses", "", 1, "n", "0"),
    ("interactive-retry", "", 1, "y", "0 1"),
]:
    run(f'''
parse_args {flags}
workdir=$(mktemp -d /tmp/{brand}-force-fixture.XXXXXX)
trace="$workdir/recovery-trace"
calls="$workdir/calls"
printf '%s\\n' '{brand}_package_run_is_owned() {{ [ -n "${{{brand.upper()}_PACKAGE_RUN_ID:-}}" ]; }}' '{brand}_package_force_is_applicable() {{ return 0; }}' '{brand}_package_recover_failed_transaction() {{ printf "recover\\n" >> "$trace"; }}' > "$workdir/arch-package-hooks"
run_from_root() {{ "$@"; }}
pacman() {{ printf '%s\\n' "${{{brand.upper()}_PACKAGE_FORCE:-0}}" >> "$calls"; [ "${{{brand.upper()}_PACKAGE_FORCE:-0}}" = 1 ]; }}
can_offer_package_force() {{ (({tty})) && ((${{#pacman_confirm[@]}} == 0)); }}
read() {{ printf -v answer '%s' {shlex.quote(response)}; }}
if run_package_transaction pacman -U -- fixture.pkg; then status=0; else status=$?; fi
[[ $(paste -sd ' ' "$calls") == {shlex.quote(expected)} ]]
case {shlex.quote(name)} in
  explicit-force|interactive-retry) [[ $status == 0 && ! -e "$trace" ]] ;;
  *) [[ $status == 1 && $(cat "$trace") == recover ]] ;;
esac
''')
run(f'''
workdir=$(mktemp -d /tmp/{brand}-cancel-fixture.XXXXXX)
trace="$workdir/recovery-trace"
printf '%s\\n' '{brand}_package_run_is_owned() {{ [ -n "${{{brand.upper()}_PACKAGE_RUN_ID:-}}" ]; }}' '{brand}_package_force_is_applicable() {{ return 0; }}' '{brand}_package_recover_failed_transaction() {{ printf "recover\\n" >> "$trace"; }}' > "$workdir/arch-package-hooks"
new_package_transaction_run_id
package_transaction_pending=1
package_transaction_returned=0
recover_failed_package_transaction
[[ ! -e "$trace" && "$package_transaction_pending" == 1 ]]
package_transaction_returned=1
recover_failed_package_transaction
[[ $(cat "$trace") == recover && "$package_transaction_pending" == 0 ]]
''')
print(f"{brand}: historical outgoing rejection, incoming override, migration scope/idempotency, removal ordering/cleanup/live-host refusal, and five force-policy scenarios passed")

# Cancel the actual transaction wrapper while its exact external child stays
# alive long enough to prove that recovery waits for acknowledgement and reap.
for cancel_signal, expected_status, tty in [(signal.SIGTERM, 143, False), (signal.SIGINT, 130, True), (signal.SIGHUP, 129, False)]:
    with tempfile.TemporaryDirectory(prefix=f"{brand}-cancel-wait-") as directory:
        fixture = pathlib.Path(directory)
        child = fixture / "package-client"
        child.write_text("#!/usr/bin/env python3\nimport os, pathlib, signal, time\n" +
                         f"root=pathlib.Path({str(fixture)!r})\n" +
                         "for number in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP): signal.signal(number, lambda *_: (root/'cancel.ack').write_text('ack'))\n" +
                         "(root/'child.pid').write_text(str(os.getpid()))\n" +
                         "while not (root/'release').exists(): time.sleep(.01)\n")
        child.chmod(0o755)
        stage = fixture / "stage"
        stage.mkdir()
        (stage / "arch-package-hooks").write_text(
            f"{brand}_package_run_is_owned() {{ [ -n \"${{{brand.upper()}_PACKAGE_RUN_ID:-}}\" ]; }}\n" +
            f"{brand}_package_recover_failed_transaction() {{\n" +
            '  [ ! -e "/proc/$(cat "$CANCEL_FIXTURE/child.pid")" ] || return 77\n' +
            '  printf "reaped\\n" > "$CANCEL_FIXTURE/recovered"\n}\n')
        body = f'''
    source {shlex.quote(str(installer))}
    workdir={shlex.quote(str(stage))}
    CANCEL_FIXTURE={shlex.quote(str(fixture))}
    eval "$(declare -f wait_package_transaction_child | sed '1s/wait_package_transaction_child/wait_package_transaction_child_original/')"
    wait_package_transaction_child() {{ printf '%s' "$BASHPID" > "$CANCEL_FIXTURE/wrapper.pid"; wait_package_transaction_child_original; }}
    if run_package_transaction {shlex.quote(str(child))}; then status=0; else status=$?; fi
    exit "$status"
    '''
        master, slave = os.openpty() if tty else (None, None)
        process = subprocess.Popen(["bash", "-c", body], stdin=slave if tty else subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if tty: os.close(slave)

        def await_file(name):
            deadline = time.monotonic() + 5
            while not (fixture / name).exists():
                assert time.monotonic() < deadline, f"timeout awaiting {name}"
                assert process.poll() is None, process.returncode
                time.sleep(.01)

        try:
            await_file("wrapper.pid")
            await_file("child.pid")
            assert int((fixture / "wrapper.pid").read_text()) == process.pid, "tracked wait runs outside outer installer"
            os.kill(process.pid, cancel_signal)
            await_file("cancel.ack")
            assert not (fixture / "recovered").exists(), "recovery ran before package child exit"
            (fixture / "release").touch()
            stdout, stderr = process.communicate(timeout=5)
            assert process.returncode == expected_status, (process.returncode, stdout, stderr)
            assert (fixture / "recovered").read_text() == 'reaped\n'
            assert "recovery could not prove" not in stderr, stderr
        finally:
            if process.poll() is None:
                (fixture / "release").touch()
                process.communicate(timeout=5)
            if tty: os.close(master)
print(f"{brand}: outer installer TERM, tty INT, and HUP signal only its owned child, wait for exit/reap, then recover")

# Deterministically inject TERM in the fork/$! assignment gap while a distinct
# fixture job is alive. The pending signal must wait for the exact new PID.
run(f'''
workdir=$(mktemp -d /tmp/{brand}-spawn-gap.XXXXXX)
trace="$workdir/recovered"
printf '%s\\n' '{brand}_package_run_is_owned() {{ [ -n "${{{brand.upper()}_PACKAGE_RUN_ID:-}}" ]; }}' '{brand}_package_force_is_applicable() {{ return 0; }}' '{brand}_package_recover_failed_transaction() {{ printf "reaped\\n" > "$trace"; }}' > "$workdir/arch-package-hooks"
sleep 2 &
unrelated=$!
eval "$(declare -f run_package_transaction | sed 's/package_transaction_pid=$!/kill -TERM "$BASHPID"; package_transaction_pid=$!/')"
if run_package_transaction /usr/bin/sleep 2; then status=0; else status=$?; fi
[[ $status == 143 && "$package_transaction_pid" == '' && "$package_transaction_returned" == 1 ]]
[[ $(cat "$trace") == reaped ]]
kill -0 "$unrelated"
kill -TERM "$unrelated"
wait "$unrelated" || true
''')
print(f"{brand}: cancellation in fork/PID-record gap reaps its child and preserves unrelated fixture job")

# Refuse cancellation when the recorded identity is stale or pidfds unavailable.
# An outsider is owned by this Python fixture, not the installer; it must live.
for identity_case in ('changed-start', 'different-parent', 'missing-pidfd'):
    with tempfile.TemporaryDirectory(prefix=f'{brand}-cancel-identity-') as directory:
        fixture = pathlib.Path(directory)
        child = fixture / 'package-client'
        child.write_text('#!/usr/bin/env python3\nimport os, pathlib, signal, time\n' +
                         f'root=pathlib.Path({str(fixture)!r})\n' +
                         'signal.signal(signal.SIGTERM, lambda *_: (root/"unexpected-signal").touch())\n' +
                         '(root/"child.pid").write_text(str(os.getpid()))\n' +
                         'while not (root/"release").exists(): time.sleep(.01)\n')
        child.chmod(0o755)
        stage = fixture / 'stage'
        stage.mkdir()
        (stage / 'arch-package-hooks').write_text(
            f'{brand}_package_run_is_owned() {{ [ -n "${{{brand.upper()}_PACKAGE_RUN_ID:-}}" ]; }}\n' +
            f'{brand}_package_recover_failed_transaction() {{\n' +
            '  [ ! -e "/proc/$(cat "$CANCEL_FIXTURE/child.pid")" ] || return 77\n' +
            '  : > "$CANCEL_FIXTURE/recovered"\n}\n')
        outsider = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'])
        outsider_start = pathlib.Path(f'/proc/{outsider.pid}/stat').read_text().rpartition(') ')[2].split()[19]
        replacement = ''
        if identity_case == 'changed-start':
            replacement = 'package_transaction_start_ticks=$((package_transaction_start_ticks + 1))'
        elif identity_case == 'different-parent':
            replacement = f'package_transaction_pid={outsider.pid}; package_transaction_start_ticks={outsider_start}'
        else:
            signal_definition = re.search(r'^package_transaction_signal\(\) \{\n.*?^\}$', installer.read_text(), re.M | re.S).group()
            replacement = signal_definition.replace('import os\n', 'import os\ndel os.pidfd_open\n')
        body = f'''
source {shlex.quote(str(installer))}
workdir={shlex.quote(str(stage))}
CANCEL_FIXTURE={shlex.quote(str(fixture))}
new_package_transaction_run_id
package_transaction_pending=1
package_transaction_exec {shlex.quote(str(child))} &
package_transaction_pid=$!
record_package_transaction_identity
original_pid=$package_transaction_pid
original_ticks=$package_transaction_start_ticks
{replacement}
package_transaction_signal TERM 143
package_transaction_pid=$original_pid
package_transaction_start_ticks=$original_ticks
: > "$CANCEL_FIXTURE/attempted"
if wait_package_transaction_child; then status=0; else status=$?; fi
exit "$status"
'''
        process = subprocess.Popen(['bash', '-c', body], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic() + 5
            while not (fixture / 'attempted').exists() or not (fixture / 'child.pid').exists():
                assert time.monotonic() < deadline and process.poll() is None, identity_case
                time.sleep(.01)
            assert outsider.poll() is None, 'PID identity mismatch signalled unrelated process'
            assert not (fixture / 'unexpected-signal').exists(), identity_case
            assert not (fixture / 'recovered').exists(), 'uncertain signal released barriers before child exit'
            (fixture / 'release').touch()
            stdout, stderr = process.communicate(timeout=5)
            assert process.returncode == 143, (identity_case, process.returncode, stdout, stderr)
            assert (fixture / 'recovered').exists(), identity_case
            assert 'could not safely signal' in stderr, stderr
            assert outsider.poll() is None, 'unrelated process exited during recovery'
        finally:
            if process.poll() is None:
                (fixture / 'release').touch()
                process.communicate(timeout=5)
            if outsider.poll() is None:
                outsider.terminate()
            outsider.wait(timeout=5)
print(f'{brand}: stale start-time, outsider PPid, and missing pidfd reject signals, preserve sibling, wait/reap before recovery')

# Pacman v7.0.0 primary source installs soft interruption for INT/HUP only.
# Model a native client whose default TERM would orphan a running scriptlet.
# Deferral must keep the client and scriptlet alive, retaining outer status 143.
with tempfile.TemporaryDirectory(prefix=f'{brand}-pacman-soft-cancel-') as directory:
    fixture = pathlib.Path(directory)
    mutator = fixture / 'scriptlet'
    mutator.write_text('#!' + sys.executable + '\nimport pathlib, time\n' +
                      f'root=pathlib.Path({str(fixture)!r})\n' +
                      'while not (root/"release").exists():\n (root/"mutating").write_text("pending")\n time.sleep(.01)\n' +
                      '(root/"scriptlet-health").write_text("complete")\n')
    mutator.chmod(0o755)
    pacman = fixture / 'pacman'
    pacman.write_text('#!' + sys.executable + '\nimport os, pathlib, signal, subprocess\n' +
                      f'root=pathlib.Path({str(fixture)!r})\n' +
                      'for number in (signal.SIGINT, signal.SIGHUP): signal.signal(number, lambda number, _: (root/"soft.ack").write_text(str(number)))\n' +
                      '(root/"client.pid").write_text(str(os.getpid()))\n' +
                      f'child=subprocess.Popen([{str(mutator)!r}])\n' +
                      '(root/"scriptlet.pid").write_text(str(child.pid))\n' +
                      'child.wait()\n(root/"package-health").write_text("complete")\n')
    pacman.chmod(0o755)
    stage = fixture / 'stage'
    stage.mkdir()
    (stage / 'arch-package-hooks').write_text(
        f'{brand}_package_run_is_owned() {{ [ -n "${{{brand.upper()}_PACKAGE_RUN_ID:-}}" ]; }}\n' +
        f'{brand}_package_recover_failed_transaction() {{\n' +
        '  [ ! -e "/proc/$(cat "$CANCEL_FIXTURE/client.pid")" ] &&\n' +
        '    [ ! -e "/proc/$(cat "$CANCEL_FIXTURE/scriptlet.pid")" ] &&\n' +
        '    [ "$(cat "$CANCEL_FIXTURE/package-health")" = complete ] || return 77\n' +
        '  : > "$CANCEL_FIXTURE/recovered"\n}\n')
    body = f'''
source {shlex.quote(str(installer))}
workdir={shlex.quote(str(stage))}
CANCEL_FIXTURE={shlex.quote(str(fixture))}
PATH={shlex.quote(str(fixture))}:/usr/bin:/bin
if run_package_transaction pacman -U fixture; then status=0; else status=$?; fi
exit "$status"
'''
    process = subprocess.Popen(['bash', '-c', body], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        deadline = time.monotonic() + 5
        while not (fixture / 'mutating').exists():
            assert time.monotonic() < deadline and process.poll() is None
            time.sleep(.01)
        os.kill(process.pid, signal.SIGTERM)
        time.sleep(.05)
        assert process.poll() is None
        assert not (fixture / 'soft.ack').exists(), 'Pacman cancellation was forwarded before all hooks finished'
        assert pathlib.Path('/proc/' + (fixture / 'scriptlet.pid').read_text()).exists()
        assert not (fixture / 'recovered').exists(), 'recovery overtook the outgoing scriptlet'
        (fixture / 'release').touch()
        stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == 143, (process.returncode, stdout, stderr)
        assert (fixture / 'scriptlet-health').read_text() == 'complete'
        assert (fixture / 'package-health').read_text() == 'complete'
        assert (fixture / 'recovered').exists()
    finally:
        if process.poll() is None:
            (fixture / 'release').touch()
            process.communicate(timeout=5)
print(f'{brand}: installer TERM is deferred while Pacman/scriptlet complete and reap; health proof precedes recovery')

# Real PTY line-discipline Ctrl-C targets the installer's foreground group.
# Native clients live in a distinct session in the same recorded PID, including
# a pretransaction mutator that has not yet acquired any recovery lock.
import fcntl
import termios
for phase in ('pretransaction-before-lock', 'outgoing-scriptlet'):
    with tempfile.TemporaryDirectory(prefix=f'{brand}-tty-pacman-cancel-') as directory:
        fixture = pathlib.Path(directory)
        mutator = fixture / 'mutator'
        mutator.write_text('#!' + sys.executable + '\nimport os, pathlib, time\n' +
                          f'root=pathlib.Path({str(fixture)!r})\n' +
                          '(root/"mutator.pid").write_text(str(os.getpid()))\n' +
                          ('(root/"lock").touch()\n' if phase == 'outgoing-scriptlet' else '') +
                          '(root/"mutating").touch()\n' +
                          'while not (root/"release").exists(): time.sleep(.01)\n' +
                          '(root/"lock").touch()\n(root/"health").write_text("complete")\n')
        mutator.chmod(0o755)
        client = fixture / 'pacman'
        client.write_text('#!' + sys.executable + '\nimport os, pathlib, signal, subprocess\n' +
                          f'root=pathlib.Path({str(fixture)!r})\n' +
                          'def unexpected(number, _):\n (root/"unexpected-signal").write_text(str(number))\n raise SystemExit(128+number)\n' +
                          'for number in (signal.SIGINT, signal.SIGHUP, signal.SIGTERM): signal.signal(number, unexpected)\n' +
                          '(root/"client.pid").write_text(str(os.getpid()))\n' +
                          '(root/"client.pg").write_text(str(os.getpgrp()))\n' +
                          f'child=subprocess.Popen([{str(mutator)!r}])\nchild.wait()\n')
        client.chmod(0o755)
        stage = fixture / 'stage'
        stage.mkdir()
        (stage / 'arch-package-hooks').write_text(
            f'{brand}_package_run_is_owned() {{ [ -n \"${{{brand.upper()}_PACKAGE_RUN_ID:-}}\" ]; }}\n' +
            f'{brand}_package_force_is_applicable() {{ return 1; }}\n' +
            f'{brand}_package_recover_failed_transaction() {{\n' +
            '  [ ! -e "/proc/$(cat "$TTY_FIXTURE/client.pid")" ] &&\n' +
            '    [ ! -e "/proc/$(cat "$TTY_FIXTURE/mutator.pid")" ] &&\n' +
            '    [ "$(cat "$TTY_FIXTURE/health")" = complete ] || return 77\n' +
            '  : > "$TTY_FIXTURE/recovered"\n}\n')
        body = f'''
source {shlex.quote(str(installer))}
workdir={shlex.quote(str(stage))}
TTY_FIXTURE={shlex.quote(str(fixture))}
package_database_lock="$TTY_FIXTURE/no-db-lock"
PATH={shlex.quote(str(fixture))}:/usr/bin:/bin
# Exercise the job-control-enabled caller too; launch must temporarily disable
# monitor mode without causing setsid to fork away from the tracked PID.
set -m
eval "$(declare -f wait_package_transaction_child | sed '1s/wait_package_transaction_child/wait_original/')"
wait_package_transaction_child() {{ printf '%s' "$package_transaction_pid" > "$TTY_FIXTURE/recorded.pid"; wait_original; }}
if run_package_transaction pacman -U fixture; then status=0; else status=$?; fi
exit "$status"
'''
        master, slave = os.openpty()
        def controlling_tty():
            os.setsid()
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)
            os.tcsetpgrp(0, os.getpgrp())
        process = subprocess.Popen(['bash', '-c', body], stdin=slave, stdout=slave, stderr=slave, preexec_fn=controlling_tty)
        os.close(slave)
        try:
            deadline = time.monotonic() + 5
            while not (fixture / 'mutating').exists() or not (fixture / 'recorded.pid').exists():
                assert time.monotonic() < deadline and process.poll() is None, phase
                time.sleep(.01)
            assert (fixture / 'client.pid').read_text() == (fixture / 'recorded.pid').read_text(), 'setsid forked away from the recorded package client'
            assert int((fixture / 'client.pg').read_text()) != process.pid
            assert (fixture / 'lock').exists() == (phase == 'outgoing-scriptlet'), 'wrong hook phase/lock boundary'
            os.write(master, b'\x03')
            time.sleep(.05)
            assert process.poll() is None
            assert not (fixture / 'unexpected-signal').exists(), 'tty signal bypassed cancellation deferral'
            assert not (fixture / 'recovered').exists(), 'recovery overtook native client/mutator'
            (fixture / 'release').touch()
            assert process.wait(timeout=5) == 130, phase
            assert (fixture / 'recovered').exists()
        finally:
            if process.poll() is None:
                (fixture / 'release').touch()
                process.wait(timeout=5)
            os.close(master)
print(f'{brand}: real foreground PTY Ctrl-C isolates native same-PID client and pre-lock/outgoing mutator until completion/reap/recovery')
