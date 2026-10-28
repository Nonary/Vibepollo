#!/usr/bin/env python3
"""Exercise exact-invocation package escalation using mocked systemd only."""
import argparse
import pathlib
import re
import shutil
import subprocess
import os
import signal
import sys
import threading
import time
import tempfile

NAMES = ('vibepollo_read_host_stop_identity', 'vibepollo_host_stop_now', 'vibepollo_stop_host_for_package', 'vibepollo_stop_exact_unit')
MOCKS = r'''
clock_value=0
vibepollo_package_transaction=$fixture
vibepollo_package_record_is_safe() { [ -f "$1" ] && [ ! -L "$1" ]; }
vibepollo_package_write_record() { printf '%s\n' "$2" > "$fixture/$1"; }
vibepollo_package_run_is_owned() { return 0; }
vibepollo_host_stop_now() {
  clock_value=$((clock_value + 10))
  vibepollo_stop_now=$clock_value
  printf '%s\n' clock >> "$fixture/events"
}
sleep() { [ "$1" = 0.1 ]; }
vibepollo_package_log_force_outcome() {
  printf 'outcome=%s\n' "$1" >> "$fixture/events"
  [ "$case_name" != logger_failed ]
}
vibepollo_package_backup_for_force() {
  printf '%s\n' backup >> "$fixture/events"
  [ "$case_name" != backup_failed ] || return 1
  [ "$case_name" != changed_during_backup ] || printf backup > "$fixture/phase"
}
vibepollo_preun_unit_is_quiescent() { vibepollo_unit_is_quiescent "$@"; }
vibepollo_unit_is_quiescent() {
  [ "$1" = vibepollo.service ] || return 1
  phase=$(cat "$fixture/phase")
  if [ "$case_name" = delayed_reap ] && [ "$phase" = killed ]; then
    count=$(cat "$fixture/reap-count" 2>/dev/null || printf 0)
    count=$((count + 1)); printf '%s\n' "$count" > "$fixture/reap-count"
    [ "$count" -lt 2 ] || printf stopped > "$fixture/phase"
    phase=$(cat "$fixture/phase")
  fi
  [ "$case_name" = absent ] || [ "$case_name" = idle ] || [ "$phase" = stopped ]
}
timeout() {
  [ "$case_name" != missing_timeout ] || return 127
  while [ "${1#--}" != "$1" ]; do shift; done
  budget=$1; shift
  printf '%s:%s\n' "$budget" "$*" >> "$fixture/events"
  "$@"
}
systemctl() {
  phase=$(cat "$fixture/phase")
  case "$1" in
    show)
      invocation=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
      pid=123
      group=/system.slice/vibepollo.service
      restart=no
      fenced=yes
      id=vibepollo.service
      if [ "$phase" = backup ] && [ "$case_name" = changed_during_backup ]; then
        invocation=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
      fi
      if [ "$phase" = grace ]; then
        case "$case_name" in
          changed_invocation|reused_pid) invocation=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb ;;
          changed_pid) pid=456 ;;
          changed_cgroup) group=/other.slice/vibepollo.service ;;
          owner_exited) pid=0 ;;
        esac
      fi
      case "$case_name" in
        resumed_changed_invocation) invocation=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb ;;
        failed_populated) pid=0 ;;
        restart_enabled) restart=always ;;
        unfenced) fenced=no ;;
        other_unit) id=other.service ;;
        invalid_invocation) invocation=xyz ;;
        invalid_pid) pid='1 2' ;;
        parent_cgroup) group=/system.slice/../vibepollo.service ;;
        root_cgroup) group=/ ;;
        missing_systemctl) return 127 ;;
        failed_show) return 1 ;;
      esac
      printf 'Id=%s\nInvocationID=%s\nMainPID=%s\nControlGroup=%s\nRestart=%s\nRefuseManualStart=%s\n' \
        "$id" "$invocation" "$pid" "$group" "$restart" "$fenced"
      [ "$case_name" != duplicate_property ] || printf 'MainPID=123\n'
      ;;
    stop)
      [ "$2" = vibepollo.service ] || return 1
      if [ "$phase" = initial ]; then
        printf grace > "$fixture/phase"
        case "$case_name" in
          graceful) printf stopped > "$fixture/phase"; return 0 ;;
          failed_stop) return 1 ;;
          early_client_kill) return 137 ;;
          successful_stop_populated) return 0 ;;
          *) return 124 ;;
        esac
      fi
      [ "$phase" = killed ] || return 1
      [ "$case_name" = still_populated ] || [ "$case_name" = delayed_reap ] || printf stopped > "$fixture/phase"
      return 0
      ;;
    kill)
      [ "$*" = 'kill --signal=KILL vibepollo.service' ] || return 1
      printf '%s\n' kill >> "$fixture/signals"
      [ "$case_name" != failed_kill ] || return 1
      printf killed > "$fixture/phase"
      ;;
    *) return 1 ;;
  esac
}
'''
CASES = {
    'absent': (0, False), 'idle': (0, False), 'graceful': (0, False),
    'timeout': (0, True), 'owner_exited': (0, True), 'failed_populated': (0, True), 'delayed_reap': (0, True), 'resumed_changed_invocation': (1, False),
    'changed_invocation': (1, False), 'reused_pid': (1, False),
    'changed_pid': (1, False), 'changed_cgroup': (1, False),
    'restart_enabled': (1, False), 'unfenced': (1, False), 'other_unit': (1, False),
    'invalid_invocation': (1, False), 'invalid_pid': (1, False),
    'parent_cgroup': (1, False), 'root_cgroup': (1, False),
    'duplicate_property': (1, False), 'missing_timeout': (1, False),
    'missing_systemctl': (1, False), 'failed_show': (1, False),
    'failed_stop': (0, True), 'early_client_kill': (0, True), 'successful_stop_populated': (0, True),
    'failed_kill': (1, True), 'still_populated': (1, True),
    'logger_failed': (1, False), 'missing_logger': (1, False), 'backup_failed': (1, False), 'missing_backup': (1, False), 'changed_during_backup': (1, False),
}

def definitions(source, context):
    text = source.read_text()
    results = []
    for name in NAMES:
        actual_name = 'vibepollo_preun_stop_exact_unit' if context == 2 and name == 'vibepollo_stop_exact_unit' else name
        matches = list(re.finditer(r'^' + actual_name + r'\(\) [({]\n.*?\n[)}]', text, re.M | re.S))
        assert matches, (source, name)
        index = context if len(matches) > 1 else 0
        results.append(matches[index].group().replace('vibepollo_preun_stop_exact_unit', 'vibepollo_stop_exact_unit'))
    return '\n\n'.join(results)


def private_process_fixture(root):
    """Only fixture-owned Popen children are signalled; no system manager runs."""
    with tempfile.TemporaryDirectory(prefix='package-stop-process-') as temporary:
        fixture = pathlib.Path(temporary)
        host_code = """import pathlib, signal, sys, time
root = pathlib.Path(sys.argv[1])
def term(*_):
    with (root / 'events').open('a') as output: output.write('host-term\\n')
signal.signal(signal.SIGTERM, term)
(root / 'ready').touch()
while True: time.sleep(0.05)
"""
        host = subprocess.Popen([sys.executable, '-c', host_code, temporary], start_new_session=True)
        sibling = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'], start_new_session=True)
        try:
            deadline = time.monotonic() + 5
            while not (fixture / 'ready').exists():
                assert time.monotonic() < deadline, 'private host did not start'
                time.sleep(0.01)
            host_start = pathlib.Path(f'/proc/{host.pid}/stat').read_text().split(') ')[1].split()[19]
            (fixture / 'owner').write_text(f'{host.pid} {host_start}')
            def reap_host():
                host.wait()
                (fixture / 'reaped').touch()
            reaper = threading.Thread(target=reap_host)
            reaper.start()
            systemctl = fixture / 'systemctl'
            systemctl.write_text('#!' + sys.executable + '\n' + r"""
import os, pathlib, signal, sys, time
root = pathlib.Path(os.environ['fixture'])
pid_text, start = (root / 'owner').read_text().split()
pid = int(pid_text)
args = sys.argv[1:]
if args[0] == 'show':
    print('Id=vibepollo.service\nInvocationID=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\nMainPID=' + pid_text + '\nControlGroup=/system.slice/vibepollo.service\nRestart=no\nRefuseManualStart=yes')
    sys.exit(0)
if args == ['stop', 'vibepollo.service'] and (root / 'reaped').exists(): sys.exit(0)
assert args in (['stop', 'vibepollo.service'], ['kill', '--signal=KILL', 'vibepollo.service']), args
fd = os.pidfd_open(pid)
try:
    observed = pathlib.Path(f'/proc/{pid}/stat').read_text().split(') ')[1].split()[19]
    assert observed == start, 'fixture PID identity changed'
    if args[0] == 'kill':
        assert (root / 'backup').exists(), 'force signal preceded backup'
        with (root / 'events').open('a') as output: output.write('fixture-kill\n')
        signal.pidfd_send_signal(fd, signal.SIGKILL)
        deadline = time.monotonic() + 3
        while not (root / 'reaped').exists():
            assert time.monotonic() < deadline, 'fixture child did not reap'
            time.sleep(0.01)
        sys.exit(0)
    signal.pidfd_send_signal(fd, signal.SIGTERM)
    # Model a stop job returning early at its own timeout while host remains.
    time.sleep(0.05)
    sys.exit(1)
finally: os.close(fd)
""")
            systemctl.chmod(0o700)
            mock = r"""
clock_value=0
vibepollo_host_stop_now() {
  clock_value=$((clock_value + 10)); vibepollo_stop_now=$clock_value
}
sleep() { [ "$1" = 0.1 ]; }
vibepollo_package_transaction=$fixture
vibepollo_package_record_is_safe() { [ -f "$1" ] && [ ! -L "$1" ]; }
vibepollo_package_write_record() { printf '%s\n' "$2" > "$fixture/$1"; }
vibepollo_unit_is_quiescent() { [ "$1" = vibepollo.service ] && [ -f "$fixture/reaped" ]; }
vibepollo_package_log_force_outcome() {
  printf 'outcome=%s\n' "$1" >> "$fixture/events"
}
vibepollo_package_backup_for_force() {
  printf '%s\n' backup >> "$fixture/events"
  : > "$fixture/backup"
}
vibepollo_stop_exact_unit vibepollo.service
"""
            script = definitions(root / 'packaging/linux/Arch/vibepollo.install', 0) + mock
            result = subprocess.run(['bash', '-c', script], env={'PATH': temporary + ':/usr/bin:/bin', 'fixture': temporary, 'VIBEPOLLO_PACKAGE_FORCE': '1'}, capture_output=True, text=True, timeout=10)
            assert result.returncode == 0, result.stderr
            reaper.join(timeout=3)
            assert host.returncode == -signal.SIGKILL, host.returncode
            assert sibling.poll() is None, 'unrelated private sibling was signalled'
            events = (fixture / 'events').read_text().splitlines()
            assert 'host-term' in events, events
            assert events.index('host-term') < events.index('backup') < events.index('outcome=pending') < events.index('fixture-kill') < events.index('outcome=complete'), events
        finally:
            for child in (host, sibling):
                if child.poll() is None:
                    child.kill()
                child.wait(timeout=3)
    print('PASS: private TERM-resistant host force/reap; unrelated sibling survives')


def late_retry_flow(root):
    for relative, name in (('packaging/linux/Arch/vibepollo.install', 'do_quiesce_machine_host'),
                           ('packaging/linux/vibeshine-preinst.in', 'vibepollo_quiesce_machine_host'),
                           ('packaging/linux/copr/Sunshine.spec', 'vibepollo_quiesce_machine_host')):
        source = (root / relative).read_text()
        body = re.search(r'^' + name + r'\(\) \{\n.*?^\}', source, re.M | re.S).group()
        for phase in ('early', 'late'):
            with tempfile.TemporaryDirectory(prefix='late-force-retry-') as temporary:
                fixture = pathlib.Path(temporary)
                (fixture / 'phase').write_text('initial')
                (fixture / 'signals').touch()
                definition = body.replace('-d /run/systemd/system', '-d "$fixture"').replace('/run/vibepollo', str(fixture / 'absent-runtime'))
                preserve = set(NAMES) | {'vibepollo_unit_is_quiescent', 'vibepollo_host_is_stable_or_quiescent', 'vibepollo_preserved_quiesce_is_safe', 'vibepollo_package_preserved_preflight_is_safe', 'vibepollo_run_optional_legacy_command'}
                names = set(re.findall(r'\bvibepollo_[A-Za-z_]+\b', definition)) - preserve
                mocks = '\n'.join(candidate + '() { return 0; }' for candidate in names)
                guards = '\n'.join((
                    'vibepollo_host_is_stable_or_quiescent() { return 1; }',
                    'vibepollo_preserved_quiesce_is_safe() { return 1; }',
                    'vibepollo_package_preserved_preflight_is_safe() { return 1; }',
                    'vibepollo_run_optional_legacy_command() { return 2; }',
                    'vibepollo_host_unit_is_masked() { return 0; }',
                    'vq() { "$@"; }',
                    'eval "$(declare -f vibepollo_unit_is_quiescent | sed 1s/vibepollo_unit_is_quiescent/original_host_quiescent/)"',
                    'vibepollo_unit_is_quiescent() { [ "$1" != vibepollo.service ] || original_host_quiescent "$@"; }',
                    'vibepollo_legacy_handoff=$fixture/absent-handoff',
                    'vibepollo_controller=$fixture/absent-controller',
                    'vibepollo_broker_socket=$fixture/absent-broker',
                    'vibepollo_control_socket=$fixture/absent-control',
                    'vibepollo_legacy_handoff_directory=$fixture/absent-handoffs',
                    'vibepollo_legacy_restore_directory=$fixture/absent-restores',
                    'vibepollo_legacy_transition_lock=$fixture/absent-lock',
                    'vibepollo_legacy_prelogin_marker=$fixture/absent-marker',
                    'vibepollo_package_prior_phase=' + phase,
                ))
                script = definitions(root / relative, 0) + MOCKS + mocks + '\n' + guards + '\n' + definition + '\n' + name
                result = subprocess.run(['bash', '-c', script], env={'PATH': '/usr/bin:/bin', 'fixture': temporary, 'case_name': 'failed_populated', 'VIBEPOLLO_PACKAGE_FORCE': '1'}, capture_output=True, text=True)
                assert result.returncode == (1 if phase == 'early' else 0), (relative, phase, result.returncode, result.stderr)
                assert bool((fixture / 'signals').read_text()) == (phase == 'late'), (relative, phase, result.stderr)
        # The failure trap cannot consume another budget or signal a replacement
        # invocation after the real shutdown helper has already returned.
        abort = re.search(r'^vibepollo_abort_quiesce\(\) \{\n.*?^\}', source, re.M | re.S).group()
        abort_test = '\n'.join((
            'vibepollo_shutdown_started=1', 'vibepollo_package_prior_phase=late',
            'vibepollo_unit_is_quiescent() { return 1; }',
            'vibepollo_stop_exact_unit() { echo unexpected-signal; }',
            abort, 'vibepollo_abort_quiesce',
        ))
        result = subprocess.run(['bash', '-c', abort_test], capture_output=True, text=True)
        assert result.returncode != 0 and 'unexpected-signal' not in result.stdout, (relative, result.stdout, result.stderr)
    print('PASS: late pending/populated retry reaches force, early instability blocks, abort never repeats shutdown')

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('root', type=pathlib.Path, nargs='?', default=pathlib.Path(__file__).resolve().parents[4])
    parser.add_argument('brand', choices=('vibepollo', 'vibeshine'), nargs='?', default='vibepollo')
    args = parser.parse_args()
    assert (args.root / 'packaging/linux/Arch' / (args.brand + '.install')).is_file()
    sources = (('packaging/linux/Arch/vibepollo.install', ('bash',), 0),
               ('packaging/linux/vibeshine-preinst.in', ('bash', 'sh'), 0),
               ('packaging/linux/vibepollo-prerm.in', ('bash', 'sh'), 0),
               ('packaging/linux/vibepollo-postinst.in', ('bash', 'sh'), 0),
               ('packaging/linux/copr/Sunshine.spec', ('bash', 'sh'), 0),
               ('packaging/linux/copr/Sunshine.spec', ('bash', 'sh'), 1),
               ('packaging/linux/copr/Sunshine.spec', ('bash', 'sh'), 2))
    dash = shutil.which('dash')
    if not dash and pathlib.Path('/tmp/upgrade-dash/bin/dash').is_file():
        dash = '/tmp/upgrade-dash/bin/dash'
    if dash:
        sources = tuple((relative, shells + ((dash,) if relative != 'packaging/linux/Arch/vibepollo.install' else ()), context) for relative, shells, context in sources)
    count = 0
    # Exercise the real clock parser too; the case matrix accelerates time.
    clock_source = definitions(args.root / sources[0][0], 0)
    clock_result = subprocess.run(['sh', '-c', clock_source + '\nvibepollo_host_stop_now && printf \"%s\\n\" \"$vibepollo_stop_now\"'], capture_output=True, text=True)
    assert clock_result.returncode == 0 and clock_result.stdout.strip().isdigit(), clock_result
    for relative, shells, context in sources:
        for shell in shells:
            for force in ('', '1', 'yes'):
                for scenario, (force_status, force_kill) in CASES.items():
                    expected_status = force_status
                    expected_kill = force_kill
                    if force != '1' and scenario not in ('absent', 'idle', 'graceful'):
                        expected_status, expected_kill = 1, False
                    with tempfile.TemporaryDirectory(prefix='package-stop-test-') as temporary:
                        fixture = pathlib.Path(temporary)
                        (fixture / 'phase').write_text('initial')
                        (fixture / 'signals').touch()
                        if scenario == 'resumed_changed_invocation':
                            (fixture / 'host-stop.before').write_text('Id=vibepollo.service\nInvocationID=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\nMainPID=123\nControlGroup=/system.slice/vibepollo.service\nRestart=no\nRefuseManualStart=yes\n')
                        script = definitions(args.root / relative, context) + MOCKS + '\n[ \"$case_name\" != missing_backup ] || unset -f vibepollo_package_backup_for_force\n[ \"$case_name\" != missing_logger ] || unset -f vibepollo_package_log_force_outcome\nvibepollo_stop_exact_unit vibepollo.service\n'
                        result = subprocess.run([shell, '-c', script], env={'PATH': '/usr/bin:/bin', 'fixture': temporary, 'case_name': scenario, 'VIBEPOLLO_PACKAGE_FORCE': force}, capture_output=True, text=True)
                        signaled = bool((fixture / 'signals').read_text())
                        assert (result.returncode, signaled) == (expected_status, expected_kill), (relative, context, shell, force, scenario, result.returncode, signaled, result.stderr)
                        if force != '1' and scenario == 'timeout':
                            assert 'VIBEPOLLO_PACKAGE_FORCE=1' in result.stderr, result.stderr
                        if expected_kill:
                            events = (fixture / 'events').read_text().splitlines()
                            graceful = events.index('60:systemctl stop vibepollo.service')
                            escalation = events.index('5:systemctl kill --signal=KILL vibepollo.service')
                            backup = events.index('backup')
                            pending = events.index('outcome=pending')
                            assert graceful < backup < pending < escalation, events
                            outcome = 'kill-failed' if scenario == 'failed_kill' else 'still-populated' if scenario == 'still_populated' else 'complete'
                            assert events.index('outcome=' + outcome) > escalation, events
                            assert events[:backup].count('clock') >= 7, events
                        count += 1
    print(f'PASS: package host stop identity, grace, escalation and emptiness ({count} cases)')
    trap_cases = 0
    for relative, shells, context in sources:
        for shell in shells:
            for scenario, expected in (('absent', 0), ('graceful', 0), ('timeout', 1)):
                with tempfile.TemporaryDirectory(prefix='package-stop-exit-trap-') as temporary:
                    fixture = pathlib.Path(temporary)
                    (fixture / 'phase').write_text('initial')
                    (fixture / 'signals').touch()
                    # dash inherits the interrupted status for a bare return;
                    # actual successful callbacks must explicitly yield success,
                    # while a genuinely incomplete stop must still reject.
                    body = definitions(args.root / relative, context) + MOCKS + '\n' + \
                        "trap 'vibepollo_stop_exact_unit vibepollo.service; callback_status=$?; trap - EXIT; exit \"$callback_status\"' EXIT\nexit 143\n"
                    result = subprocess.run([shell, '-c', body], env={'PATH': '/usr/bin:/bin', 'fixture': temporary, 'case_name': scenario, 'VIBEPOLLO_PACKAGE_FORCE': ''}, capture_output=True, text=True)
                    assert result.returncode == expected, (relative, shell, scenario, result.returncode, result.stderr)
                    trap_cases += 1
    print(f'PASS: host-stop EXIT-trap success and genuine failure statuses ({trap_cases} cases)')
    private_process_fixture(args.root)
    late_retry_flow(args.root)

if __name__ == '__main__':
    main()
