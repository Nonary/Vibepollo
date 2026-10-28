#!/usr/bin/env python3
"""Preserved masks: actual package helper logic with private filesystem/systemctl fixtures."""
import argparse
import pathlib
import re
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('root', type=pathlib.Path)
parser.add_argument('brand', choices=('vibepollo', 'vibeshine'), nargs='?', default='vibeshine')
args = parser.parse_args()
ROOT = args.root.resolve()
BRAND = args.brand
CONTEXTS = [
    ('Arch', f'packaging/linux/Arch/{BRAND}.install', 0),
    ('DEB-pre', 'packaging/linux/vibeshine-preinst.in', 0),
    ('DEB-post', f'packaging/linux/{BRAND}-postinst.in', 0),
    ('DEB-remove', f'packaging/linux/{BRAND}-prerm.in', 0),
    ('RPM-pre', 'packaging/linux/copr/Sunshine.spec', 0),
    ('RPM-post', 'packaging/linux/copr/Sunshine.spec', 1),
    ('RPM-remove', 'packaging/linux/copr/Sunshine.spec', 2),
]
NAMES = ['package_open_owned_host_for_stop', 'prepare_host_upgrade_fence',
         'activate_host_upgrade_fence', 'host_unit_is_masked',
         'unit_is_quiescent', 'cgroup_is_quiescent', 'package_path_identity',
         'package_record_is_safe', 'package_remove_owned', 'package_no_jobs',
         'stop_host_for_package', 'package_properties_are_unique', 'unmask_host_for_controller', 'package_preserve_unowned_host_mask',
         'package_prepare_postinstall_start', 'package_complete_quiescence_is_safe']

def extract(text, name, index):
    found = re.findall(r'^vibeshine_' + name + r'\(\) [({]\n.*?^[)}]$', text, re.M | re.S)
    if not found:
        return ''
    return found[min(index, len(found) - 1)]

def helpers(path, index):
    text = path.read_text().replace('%%', '%').replace(BRAND, 'vibeshine').replace(BRAND.upper(), 'VIBESHINE').replace(BRAND.capitalize(), 'Vibeshine')
    if index == 2:
        # RPM preun scopes the read-only unit/cgroup probes by name.
        text = text.replace('vibeshine_preun_unit_is_quiescent', 'vibeshine_unit_is_quiescent').replace('vibeshine_preun_cgroup_is_quiescent', 'vibeshine_cgroup_is_quiescent')
    definitions = '\n'.join(extract(text, name, index) for name in NAMES)
    return definitions

def postinstall_caller(path, label, fixture, configure_failed=False):
    text = path.read_text().replace('%%', '%').replace(BRAND, 'vibeshine').replace(BRAND.upper(), 'VIBESHINE').replace(BRAND.capitalize(), 'Vibeshine')
    if label == 'Arch':
        function = re.search(r'^do_install_prelogin\(\) \{\n.*?^\}', text, re.M | re.S).group()
        if configure_failed:
            helper = fixture/'machine-host'
            helper.write_text('#!/bin/sh\n[ "$1" != configure-auto ]\n')
            helper.chmod(0o755)
            return 'vibeshine_retire_obsolete_boot_links() { return 0; }\n' + function.replace('/usr/libexec/vibeshine/vibeshine-machine-host', str(helper)) + '\ndo_install_prelogin'
        branch = function.split('if "$helper" configure-auto; then\n', 1)[1].split('\n  else\n', 1)[0]
    elif label == 'DEB-post':
        branch = text[text.index('if [ "$vibeshine_machine_configured" -eq 1 ]; then\n  vibeshine_package_prepare_postinstall_start'):]
    else:
        branch = text.split('if "$vibeshine_machine_helper" configure-auto; then\n', 1)[1].split('\n  else\n    if ! vibeshine_package_complete_quiescence_is_safe', 1)[0]
    # Execute the complete actual configure-success branch, including the
    # caller's deferred-start conditional and its real enable/start commands.
    return 'masked_postinstall_caller() {\n' + branch + '\n}\nvibeshine_machine_configured=1\nmasked_postinstall_caller'

MOCKS = r"""
# UID mapping is the sole metadata boundary: the executor's fixture files are
# not root-owned, while actual inode/mode/type/link checks remain intact.
stat() {
  case "$2" in
    %u:%g:*) /usr/bin/stat "$@" | sed -E 's/^[0-9]+:[0-9]+:/0:0:/' ;;
    %d:%i:%u:%g:*) if [ "$scenario" = unsafe-mask ]; then printf '1:2:65534:65534:symbolic link\n'; else /usr/bin/stat "$@" | sed -E 's/^([0-9]+:[0-9]+):[0-9]+:[0-9]+:/\1:0:0:/'; fi ;;
    *) /usr/bin/stat "$@" ;;
  esac
}
install() {
  # Preserve install's real byte/mode behavior without chowning fixture files.
  if [ "$1" = -d ]; then shift; fi
  while [ "$#" -gt 0 ]; do
    case "$1" in -o|-g) shift 2 ;; -m) shift 2 ;; --) shift; break ;; *) break ;; esac
  done
  # Used only by baseline prepare; failing preparation is expected for a
  # differing unowned fence, so no mutation should reach install's copy path.
  [ "$#" -eq 1 ] && [ -d "$1" ]
}
timeout() {
  while [ "${1#--}" != "$1" ]; do shift; done
  shift
  "$@"
}
systemctl() {
  printf '%s\n' "$*" >> "$fixture/events"
  action=$1; shift
  case "$action" in
    is-enabled) if [ "$scenario" = invalid_mask_state ]; then printf disabled; else printf 'masked-runtime\n'; fi ;;
    daemon-reload|disable) : ;;
    show)
      unit=$1; shift
      [ "$scenario" != uncertain ] || return 1
      for property do
        case "$property" in
          --property=LoadState) printf 'LoadState=masked\n' ;;
          --property=ActiveState) if [ "$scenario" = active ]; then printf 'ActiveState=active\n'; else printf 'ActiveState=inactive\n'; fi ;;
          --property=SubState) if [ "$scenario" = active ]; then printf 'SubState=running\n'; else printf 'SubState=dead\n'; fi ;;
          --property=MainPID) if [ "$scenario" = active ]; then printf 'MainPID=123\n'; else printf 'MainPID=0\n'; fi ;;
          --property=ControlGroup) printf 'ControlGroup=/system.slice/vibeshine.service\n' ;;
          --property=Job) if [ "$scenario" = job ]; then printf 'Job=7 stop\n'; else printf 'Job=\n'; fi ;;
          --property=RefuseManualStart) printf 'RefuseManualStart=no\n' ;;
          --property=KillMode) printf 'KillMode=control-group\n' ;;
          --property=SendSIGKILL) printf 'SendSIGKILL=yes\n' ;;
          --property=Restart) printf 'Restart=no\n' ;;
          *) return 1 ;;
        esac
      done ;;
    stop|kill|unmask|start|enable) printf 'UNEXPECTED_MUTATION %s\n' "$action" >&2; return 91 ;;
    *) return 1 ;;
  esac
}
"""

def run(context, scenario, mode='chain', force='0', shell='bash'):
    label, relative, index = context
    with tempfile.TemporaryDirectory(prefix='masked-host-proof-') as directory:
        fixture = pathlib.Path(directory)
        for name in ['run/systemd/system/vibeshine.service.d', 'etc/systemd/system', 'transaction', 'cgroup/system.slice/vibeshine.service']:
            (fixture/name).mkdir(parents=True)
        location = 'etc' if 'persistent' in scenario else 'run'
        unit = 'vibeshine-session-exec.socket' if 'broker' in scenario else 'vibeshine-vkms-control.socket' if 'control' in scenario else 'vibeshine-session-controller.service' if 'controller' in scenario else 'vibeshine.service'
        mask = fixture / location / 'systemd/system' / unit
        mask.symlink_to('/dev/null')
        fence = fixture/'run/systemd/system/vibeshine.service.d/90-package.conf'
        fence.write_text('[Unit]\nRefuseManualStart=yes\n\n[Service]\nKillMode=mixed\nSendSIGKILL=no\n')
        fence.chmod(0o644)
        records = fixture/'transaction'
        # Exact preexisting root-private records: remove_owned must preserve.
        (records/'vibeshine.service.before').write_text('preexisting-mask')
        (records/'fence.before').write_text('preexisting-fence')
        for record in records.iterdir(): record.chmod(0o600)
        cgroup = fixture/'cgroup/system.slice/vibeshine.service/cgroup.events'
        cgroup.write_text('populated 1\n' if scenario == 'populated' else 'populated 0\n')
        before = (mask.lstat(), fence.stat(), fence.read_bytes(), mask.readlink())
        body = helpers(ROOT/relative, index)
        body = body.replace('/run/systemd/system', str(fixture/'run/systemd/system')).replace('/etc/systemd/system', str(fixture/'etc/systemd/system')).replace('/sys/fs/cgroup', str(fixture/'cgroup')).replace('/run/vibeshine/', str(fixture/'runtime') + '/').replace('/run/vibeshine-host-upgrade.', str(fixture/'temporary.'))
        variables = '\n'.join([
            'fixture=' + shlex.quote(directory), 'scenario=' + shlex.quote(scenario),
            'vibeshine_package_transaction=' + shlex.quote(str(records)),
            'vibeshine_host_upgrade_dropin=' + shlex.quote(str(fence)),
            'vibeshine_host_upgrade_dropin_dir=' + shlex.quote(str(fence.parent)),
            'vibeshine_upgrade_kill_mode=control-group'] + [
                name + '=' + shlex.quote(str(fixture/name)) for name in (
                    'vibeshine_broker_socket', 'vibeshine_control_socket',
                    'vibeshine_legacy_handoff_directory', 'vibeshine_legacy_restore_directory',
                    'vibeshine_legacy_transition_lock', 'vibeshine_legacy_prelogin_marker')])
        if mode.startswith('post'):
            # Inventory/old-executable and release boundaries are controlled;
            # complete_quiescence_is_safe itself, unit/cgroup/job checks and the
            # actual postinstall caller remain extracted production code.
            body += '''
vibeshine_package_unmanaged_host_absent() {
  printf 'full-generation-proof\n' >> "$fixture/events"
  [ "$scenario" != generation_dirty ]
}
vibeshine_control_instances_are_quiescent() { return 0; }
vibeshine_brokers_are_quiescent() { return 0; }
vibeshine_restore_instances_are_quiescent() { return 0; }
vibeshine_package_release() {
  printf 'release-owned-barriers\n' >> "$fixture/events"
  vibeshine_unit_is_quiescent vibeshine.service && vibeshine_package_no_jobs
}
'''
            # Real private identity record needed for the unowned fence.
            (records/'fence.before').write_text(f'{fence.stat().st_dev}:{fence.stat().st_ino}:0:0:regular file')
            if scenario == 'changed-fence':
                (records/'fence.before').write_text(f'{fence.stat().st_dev}:{fence.stat().st_ino+1}:0:0:regular file')
            if scenario.startswith(('broker', 'control', 'controller')):
                # A transaction-owned fence must not hide missing recognition
                # of administrator socket/controller masks.
                (records/'fence.before').write_text('absent')
                (records/'fence.owned').write_text(f'{fence.stat().st_dev}:{fence.stat().st_ino}:0:0:regular file')
                (records/'fence.owned').chmod(0o600)
        # Each stage is an actual extracted production helper; no activation,
        # cgroup, no-jobs or host-stop helper is replaced by a success stub.
        if mode == 'chain':
            calls = 'vibeshine_prepare_host_upgrade_fence && vibeshine_activate_host_upgrade_fence && vibeshine_stop_host_for_package && '
            if scenario == 'repopulated':
                calls += 'printf \"populated 1\\n\" > ' + shlex.quote(str(cgroup)) + ' && '
            calls += 'vibeshine_unit_is_quiescent vibeshine.service && vibeshine_package_no_jobs'
        elif mode == 'activation':
            calls = 'vibeshine_activate_host_upgrade_fence'
        elif mode == 'post-existing':
            calls = 'vibeshine_unmask_host_for_controller'
        else:
            calls = postinstall_caller(ROOT/relative, label, fixture, configure_failed=(mode == 'post-config-failed'))

        result = subprocess.run([shell,'-c',body+'\n'+MOCKS+'\n'+variables+'\n'+calls], capture_output=True, text=True, env={'PATH':'/usr/bin:/bin','VIBESHINE_PACKAGE_FORCE':force})
        after = (mask.lstat(), fence.stat(), fence.read_bytes(), mask.readlink())
        assert [(x.st_dev,x.st_ino,x.st_uid,x.st_gid,x.st_mode,x.st_mtime_ns) for x in before[:2]] == [(x.st_dev,x.st_ino,x.st_uid,x.st_gid,x.st_mode,x.st_mtime_ns) for x in after[:2]], (label,scenario,'metadata changed')
        assert before[2:] == after[2:], (label,scenario,'mask/fence changed')
        events = (fixture/'events').read_text() if (fixture/'events').exists() else ''
        assert 'UNEXPECTED_MUTATION' not in result.stderr, (label,scenario,result.stderr)
        return result, events

count=0
shells = ['bash', 'sh']
dash = pathlib.Path('/tmp/upgrade-dash/bin/dash')
if dash.is_file(): shells.append(str(dash))
for context in CONTEXTS:
    for shell in (['bash'] if context[0] == 'Arch' else shells):
        for scenario in ('idle','persistent','active','populated','uncertain','job','invalid_mask_state','repopulated','unsafe-mask'):
            for force in ('0', '1'):
                result, events=run(context, scenario, force=force, shell=shell)
                assert (result.returncode==0) == (scenario in ('idle','persistent')), (context,shell,scenario,result.returncode,result.stderr)
                if result.returncode and scenario != 'repopulated':
                    assert ('unsafe host mask' if scenario == 'unsafe-mask' else 'unowned Vibeshine host mask') in result.stderr, (context,scenario,result.stderr)
                elif result.returncode == 0:
                    assert 'show vibeshine-session-controller.service --property=Job' in events
                    assert 'show vibeshine-session-exec.socket --property=Job' in events
                    assert 'show vibeshine-vkms-control.socket --property=Job' in events
                count+=1
        if context[0] in ('Arch', 'DEB-post', 'RPM-post'):
            for scenario in ('idle','persistent','broker-runtime','broker-persistent','control-runtime','control-persistent','controller-runtime','controller-persistent','generation_dirty','changed-fence'):
                result, events = run(context, scenario, 'post-current', shell=shell)
                assert (result.returncode == 0) == (scenario not in ('generation_dirty', 'changed-fence')), (context,shell,scenario,result.returncode,result.stderr)
                assert ('full-generation-proof' in events) == (scenario != 'changed-fence')
                assert ('release-owned-barriers' in events) == (scenario not in ('generation_dirty', 'changed-fence'))
                if result.returncode == 0:
                    assert 'configuration completed' in result.stderr
                count+=1
            if context[0] == 'Arch':
                result, events = run(context, 'idle', 'post-config-failed', shell=shell)
                assert result.returncode == 1 and 'ACTION REQUIRED' in result.stderr, result.stderr
                assert 'full-generation-proof' in events and 'release-owned-barriers' in events
                count += 1
print(f'PASS: {BRAND} actual preserved-mask preparation/activation/stop/job gates, blocked active/uncertain owner, deferred startup protocol and unchanged metadata ({count} cases)')
