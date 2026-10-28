#!/usr/bin/env python3
"""Removal orchestration fixture: no host services, signals or packages are touched."""
import pathlib
import re
import subprocess
import shutil
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[4]
MOCKS = r'''
# Synthetic root metadata retains real file types, modes and inode identities.
stat() {
  case "$2" in
    %u:%g:*) /usr/bin/stat "$@" | sed -E 's/^[0-9]+:[0-9]+:/0:0:/' ;;
    '%d:%i:%u:%g:'*) /usr/bin/stat "$@" | sed -E 's/^([0-9]+:[0-9]+):[0-9]+:[0-9]+:/\1:0:0:/' ;;
    *) /usr/bin/stat "$@" ;;
  esac
}
sleep() { :; }
timeout() {
  [ "$scenario" != missing_timeout ] || return 127
  while [ "${1#--}" != "$1" ]; do shift; done
  shift
  "$@"
}
systemctl() {
  printf '%s\n' "$*" >> "$fixture/events"
  action=$1; shift
  case "$action" in
    show)
      unit=$1; shift
      phase=$(cat "$fixture/host")
      for prop do
        key=${prop#--property=}
        case "$key" in
          Id) printf 'Id=%s\n' "$unit" ;;
          InvocationID) if [ -e "$fixture/new-owner" ]; then printf 'InvocationID=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n'; else printf 'InvocationID=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n'; fi ;;
          MainPID) if [ "$unit" = vibepollo.service ] && [ "$phase" = active ]; then printf 'MainPID=123\n'; else printf 'MainPID=0\n'; fi ;;
          ControlGroup) if [ "$unit" = vibepollo.service ] && [ "$phase" = active ]; then printf 'ControlGroup=/system.slice/vibepollo.service\n'; else printf 'ControlGroup=\n'; fi ;;
          LoadState) if [ -L "$fixture/systemd/$unit" ]; then printf 'LoadState=masked\n'; else printf 'LoadState=loaded\n'; fi ;;
          ActiveState) if [ "$unit" = vibepollo.service ] && [ "$phase" = active ]; then printf 'ActiveState=active\n'; else printf 'ActiveState=inactive\n'; fi ;;
          SubState) if [ "$unit" = vibepollo.service ] && [ "$phase" = active ]; then printf 'SubState=running\n'; else printf 'SubState=dead\n'; fi ;;
          Job) printf 'Job=\n' ;;
          FreezerState) printf 'FreezerState=running\n' ;;
          Restart) printf 'Restart=no\n' ;;
          RefuseManualStart) printf 'RefuseManualStart=yes\n' ;;
          KillMode) printf 'KillMode=%s\n' "$vibepollo_upgrade_kill_mode" ;;
          SendSIGKILL) printf 'SendSIGKILL=no\n' ;;
          *) return 1 ;;
        esac
      done ;;
    is-enabled) if [ -L "$fixture/systemd/$1" ]; then printf 'masked-runtime\n'; else printf 'disabled\n'; fi ;;
    mask)
      [ "$1" = --runtime ] || return 1; shift
      for unit do [ -L "$fixture/systemd/$unit" ] || ln -s /dev/null "$fixture/systemd/$unit" || return 1; done
      if [ "$scenario" = real_partial_failure ] && [ "$unit" = vibeshine-vkms-control.socket ]; then return 1; fi ;;
    stop)
      if [ "$1" = vibepollo.service ] && [ "$(cat "$fixture/host")" = active ]; then
        case "$scenario" in default_timeout|force_timeout) return 124 ;; failed_stop) return 1 ;; real_new_owner_cancel) touch "$fixture/new-owner"; return 1 ;; esac
        printf stopped > "$fixture/host"
      fi ;;
    kill)
      [ "$*" = '--signal=KILL vibepollo.service' ] || return 1
      printf stopped > "$fixture/host" ;;
    list-units|daemon-reload|disable) : ;;
    *) return 1 ;;
  esac
}
vibepollo_package_capture_host_executables() { printf '[]\n'; }
vibepollo_package_unmanaged_host_absent() {
  printf 'unmanaged proof\n' >> "$fixture/events"
  [ ! -e "$fixture/block-post-proof" ]
}
vibepollo_cgroup_is_quiescent() { [ -z "$1" ]; }
vibepollo_select_upgrade_kill_mode() {
  vibepollo_upgrade_kill_mode=mixed
  [ "$(cat "$fixture/host")" = active ] || vibepollo_upgrade_kill_mode=control-group
}
install() {
  mode=0755; directory=0
  while [ "$#" -gt 0 ]; do
    case "$1" in
      -d) directory=1; shift ;;
      -o|-g) shift 2 ;;
      -m) mode=$2; shift 2 ;;
      --) shift; break ;;
      *) break ;;
    esac
  done
  if [ "$directory" -eq 1 ]; then
    /usr/bin/install -d -m "$mode" -- "$@"
  else
    /usr/bin/install -m "$mode" -- "$@"
  fi
}
vibepollo_activate_host_upgrade_fence() { :; }
vibepollo_freeze_controller() {
  vibepollo_controller_was_frozen=0
  case "$scenario" in early_failure) return 1 ;; early_cancel) exit 143 ;; esac
}
vibepollo_controller_remains_frozen() { :; }
vibepollo_thaw_controller() { :; }
vibepollo_package_log_force_outcome() { printf "force-outcome %s\n" "$1" >> "$fixture/events"; }
vibepollo_package_backup_for_force() { printf backup >> "$fixture/events"; }
vibepollo_host_stop_now() {
  vibepollo_stop_now=$(cat "$fixture/clock")
  printf '%s' "$((vibepollo_stop_now + 70))" > "$fixture/clock"
}
vibepollo_stop_restore_instances() { :; }
vibepollo_legacy_handoff_process_is_running() { return 1; }
'''



def assert_local_calls_defined(text, label):
    """Prevent namespace drift between independently executed scriptlets."""
    definitions=set(re.findall(r'^(vibepollo_[a-z_]+)\(\)', text, re.M))
    calls=set(re.findall(r'(?:^|[;&|]|\bif\b|\bthen\b|\bdo\b|\bwhile\b|!)\s*(vibepollo_[a-z_]+)(?=\s*(?:$|[;&|"\x27$]|\s))', text, re.M))
    assert not calls-definitions, (label, sorted(calls-definitions))


def source(section):
    if section == 'DEB':
        text = (ROOT / 'packaging/linux/vibepollo-prerm.in').read_text().split('# Debian passes', 1)[0]
    else:
        text = (ROOT / 'packaging/linux/copr/Sunshine.spec').read_text().split('\n%preun\n', 1)[1].split('if [ "$1" -eq 0 ]; then', 1)[0]
        text = text.replace('%%', '%').replace('%{_prefix}/libexec/vibeshine', '@VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR@')
    assert_local_calls_defined(text, section)
    return text


def main(scenarios=None):
    native = (ROOT/'cmake/packaging/linux.cmake').read_text()
    assert 'set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE "${CMAKE_CURRENT_BINARY_DIR}/postrm")' in native
    assert 'remove | purge | disappear | 0)' in (ROOT/'packaging/linux/vibeshine-postrm.in').read_text()
    count = 0
    for context in ('DEB', 'RPM'):
        for shell in tuple(['sh', 'bash'] + ([shutil.which('dash')] if shutil.which('dash') else [])):
            for scenario in (scenarios or ('idle', 'graceful', 'default_timeout', 'force_timeout', 'failed_stop', 'cleanup_failure', 'preexisting_mask', 'no_systemd', 'retry_cleanup', 'early_failure', 'early_cancel', 'late_cancel', 'stale_record', 'unsafe_transaction', 'missing_timeout', 'retry_timeout', 'legacy_success', 'legacy_late_cancel', 'real_activation', 'real_partial_failure', 'real_activation_cancel', 'real_new_owner_cancel', 'real_admin_barrier_cancel')):
                with tempfile.TemporaryDirectory(prefix='removal-fixture-') as tmp:
                    f = pathlib.Path(tmp)
                    (f/'systemd').mkdir()
                    (f/'runtime').mkdir()
                    (f/'host').write_text('stopped' if scenario in ('idle', 'preexisting_mask', 'no_systemd') else 'active')
                    (f/'events').touch()
                    (f/'clock').write_text('0')
                    if scenario == 'preexisting_mask':
                        (f/'systemd/vibepollo.service').symlink_to('/dev/null')
                    controller=f/'controller'
                    controller.write_text('#!/bin/sh\n[ ! -e "$fixture/fail-cleanup" ]\n')
                    controller.chmod(0o755)
                    handoff=f/'handoff'
                    if scenario.startswith('legacy_') or scenario in ('real_activation_cancel','real_admin_barrier_cancel'):
                        handoff.write_text('#!/bin/sh\nexit 0\n')
                        handoff.chmod(0o755)
                    admin_restore=f/'systemd/vibepollo-session-restore@.service'
                    if scenario == 'real_admin_barrier_cancel':
                        admin_restore.symlink_to('/dev/null')
                        admin_restore_inode=admin_restore.lstat().st_ino
                    if scenario in ('cleanup_failure', 'retry_cleanup'):
                        (f/'fail-cleanup').touch()
                    if scenario == 'stale_record':
                        (f/'runtime/session.env').write_text('stale binding\n')
                    if scenario == 'unsafe_transaction':
                        (f/'transaction').mkdir(mode=0o777)
                        (f/'transaction').chmod(0o777)
                    production=source(context)
                    text=production.replace('/run/systemd/system', str(f/'systemd')).replace('/run/vibepollo-package-transaction', str(f/'transaction')).replace('/run/vibepollo/', str(f/'runtime')+'/').replace('/run/vibeshine/', str(f/'runtime')+'/').replace('/run/vibepollo-restore-units.', str(f/'restore.')).replace('/run/vibepollo-broker-units.', str(f/'brokers.')).replace('/run/vibepollo-control-units.', str(f/'control.')).replace('/run/vibepollo-host-upgrade.', str(f/'fence-temp.'))
                    if scenario == 'no_systemd':
                        text=text.replace(str(f/'systemd'), str(f/'missing-systemd'))
                    mocks=MOCKS
                    if scenario.startswith('real_'):
                        # Run the actual fence activation and its real property
                        # validation; only the manager boundary is simulated.
                        mocks=mocks.replace('vibepollo_activate_host_upgrade_fence() { :; }\n','')
                    if context == 'RPM':
                        mocks=mocks.replace('vibepollo_cgroup_is_quiescent', 'vibepollo_preun_cgroup_is_quiescent').replace('vibepollo_select_upgrade_kill_mode', 'vibepollo_preun_select_upgrade_kill_mode')
                    script=text+mocks+f'\nvibepollo_controller="{controller}"\nvibepollo_machine_host="{f}/absent-machine"\nvibepollo_host_upgrade_dropin="{f}/fence"\nvibepollo_legacy_handoff="{handoff}"\n'
                    if scenario == 'no_systemd': script += 'vibepollo_controller="$fixture/absent-controller"\n'
                    if scenario == 'retry_timeout': script += 'scenario=default_timeout\n'
                    if scenario == 'force_timeout': script += 'VIBEPOLLO_PACKAGE_FORCE=1\n'
                    if scenario in ('late_cancel', 'legacy_late_cancel','real_activation_cancel','real_admin_barrier_cancel'):
                        script += ('vibepollo_preun_stop_brokers' if context == 'RPM' else 'vibepollo_stop_brokers') + '() { if [ ! -e "$fixture/cancelled" ]; then touch "$fixture/cancelled"; exit 143; fi; return 0; }\n'
                    script += 'vibepollo_remove_or_abort\nstatus=$?\n'
                    if scenario == 'retry_timeout':
                        script += '[ "$status" -ne 0 ] || exit 91\nscenario=graceful\nvibepollo_remove_or_abort\nstatus=$?\n'
                    if scenario == 'retry_cleanup':
                        script += '[ "$status" -ne 0 ] || exit 91\nrm "$fixture/fail-cleanup"\nvibepollo_remove_or_abort\nstatus=$?\n'
                    script += 'if [ "$status" -eq 0 ]; then [ -d "$fixture/transaction" ] || exit 92; fi\nexit "$status"\n'
                    result=subprocess.run([shell,'-c',script],env={'PATH':'/usr/bin:/bin','fixture':tmp,'scenario':scenario},capture_output=True,text=True)
                    expected_failure=scenario in ('default_timeout','failed_stop','cleanup_failure','early_failure','early_cancel','late_cancel','stale_record','unsafe_transaction','missing_timeout','legacy_late_cancel','real_partial_failure','real_activation_cancel','real_new_owner_cancel','real_admin_barrier_cancel')
                    assert (result.returncode != 0) == expected_failure, (context,shell,scenario,result.returncode,result.stderr)
                    assert 'command not found' not in result.stderr and 'not found' not in result.stderr, (context,shell,scenario,result.stderr)
                    events=(f/'events').read_text()
                    if scenario.startswith('real_'):
                        assert 'daemon-reload' in events and '--property=KillMode' in events, (context,shell,scenario,events)
                    if scenario == 'real_new_owner_cancel':
                        assert events.splitlines().count('stop vibepollo.service') == 1
                        assert not (f/'systemd/vibepollo.service').exists()
                        assert (f/'fence').exists() and (f/'transaction').exists()

                    assert 'user' not in events and 'other.service' not in events
                    assert ('kill --signal=KILL' in events) == (scenario == 'force_timeout'), (scenario, events)
                    if expected_failure:
                        assert 'disable ' not in events
                        if scenario == 'legacy_late_cancel': assert handoff.stat().st_mode & 0o777 == 0o755
                        assert (f/'transaction').is_dir() == (scenario in ('default_timeout','failed_stop','cleanup_failure','stale_record','unsafe_transaction','real_new_owner_cancel')), (context,shell,scenario,result.stderr)
                        if scenario in ('early_failure','early_cancel','late_cancel','legacy_late_cancel','real_partial_failure','real_activation_cancel','real_admin_barrier_cancel'):
                            assert not (f/'fence').exists(), (context,shell,scenario,'owned fence remains')
                            for unit in ('vibepollo.service','vibepollo-session-exec.socket','vibeshine-vkms-control.socket','vibepollo-session-restore@.service'):
                                if scenario == 'real_admin_barrier_cancel' and unit == 'vibepollo-session-restore@.service':
                                    assert admin_restore.is_symlink() and admin_restore.lstat().st_ino == admin_restore_inode
                                else:
                                    assert not (f/'systemd'/unit).is_symlink(), (context,shell,scenario,'owned barrier remains',unit)

                    else:
                        assert (f/'transaction').is_dir(), (context,shell,scenario,result.stderr)
                        if scenario not in ('no_systemd','preexisting_mask'):
                            assert (f/'fence').exists(), (context,shell,scenario,'owned fence missing')
                        if scenario != 'no_systemd':
                            assert (f/'systemd/vibepollo-session-exec.socket').is_symlink()
                        assert 'unmanaged proof' in events
                        if context == 'DEB':
                            post_full=(ROOT/'packaging/linux/vibeshine-postrm.in').read_text()
                            post=post_full.split('\ncase "${1:-}" in\n',1)[0]
                            entry='case "${1:-}" in\n'+post_full.split('\ncase "${1:-}" in\n',1)[1].split('# Ordinary removal and upgrades',1)[0]
                            # Numeric0 is native CPack RPM post-uninstall.
                            entry=('set -- 0\n' if scenario == 'graceful' else 'set -- remove\n')+entry
                        else:
                            post_full=(ROOT/'packaging/linux/copr/Sunshine.spec').read_text().split('\n%postun\n',1)[1].split('\n%files\n',1)[0].replace('%%','%')
                            post=post_full.split('if [ "$1" -eq 0 ]; then',1)[0]
                            entry='set -- 0\nif [ "$1" -eq 0 ]; then'+post_full.split('if [ "$1" -eq 0 ]; then',1)[1]
                        assert_local_calls_defined(post, context+' post-removal')
                        post=post.replace('/run/systemd/system', str(f/'systemd')).replace('/run/vibepollo-package-transaction', str(f/'transaction')).replace('/run/vibepollo/', str(f/'runtime')+'/').replace('/run/vibeshine/', str(f/'runtime')+'/').replace('/run/vibepollo-restore-units.', str(f/'restore.')).replace('/run/vibepollo-control-units.', str(f/'control.'))
                        if scenario == 'no_systemd': post=post.replace(str(f/'systemd'),str(f/'missing-systemd'))
                        # %postun is a fresh interpreter: never inherit %preun
                        # functions. Package files are removed before release.
                        controller.unlink(missing_ok=True)
                        if scenario == 'legacy_success':
                            assert handoff.stat().st_mode & 0o777 == 0
                            handoff.unlink()
                        post += MOCKS + f'\nvibepollo_host_upgrade_dropin="{f}/fence"\n'+entry
                        if scenario == 'graceful':
                            # A newly observed old executable blocks post-file
                            # release; a retry succeeds once that scope drains.
                            (f/'block-post-proof').touch()
                            refused=subprocess.run([shell,'-c',post],env={'PATH':'/usr/bin:/bin','fixture':tmp,'scenario':scenario},capture_output=True,text=True)
                            assert refused.returncode != 0
                            assert (f/'transaction').is_dir() and (f/'fence').exists()
                            (f/'block-post-proof').unlink()
                        before_post=(f/'events').read_text()
                        final=subprocess.run([shell,'-c',post],env={'PATH':'/usr/bin:/bin','fixture':tmp,'scenario':scenario},capture_output=True,text=True)
                        post_events=(f/'events').read_text()[len(before_post):]
                        assert not any(line.startswith(('start ', 'stop ', 'kill ', 'mask ')) for line in post_events.splitlines()), post_events
                        assert final.returncode == 0, (context,shell,scenario,final.stderr)
                        assert not (f/'transaction').exists(), (scenario,final.stderr)
                        assert not (f/'fence').exists()
                        assert (f/'systemd/vibepollo.service').is_symlink() == (scenario == 'preexisting_mask')
                        assert not (f/'systemd/vibepollo-session-exec.socket').exists()
                    count += 1
    print(f'{count} removal orchestration scenarios passed in unmodified DEB/RPM pre/post removal shells; no live services touched.')

if __name__ == '__main__': main()
