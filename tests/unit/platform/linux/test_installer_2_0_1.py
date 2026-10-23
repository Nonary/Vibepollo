#!/usr/bin/env python3
"""Known 2.0.1 selector/fence rejection and version-scoped ALPM override.

Uses verbatim tagged full hooks as read-only fixtures, extracting only the
selector, metadata checker and fence consumer into private paths. Then run
the complete incoming guard followed by the complete old removal scriptlet
against a strict private systemctl model. Native ALPM execution is not claimed.
"""
import os
import pathlib
import re
import shlex
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[4]
brand = sys.argv[2] if len(sys.argv) > 2 else 'vibepollo'
baseline = root / f'tests/unit/platform/linux/fixtures/package-2.0.1/{brand}.install'
old_source = baseline.read_text()
definitions = []
for suffix in ('privileged_helper_is_safe', 'select_upgrade_kill_mode', 'prepare_host_upgrade_fence'):
    opener, closer = ('(', ')') if suffix == 'prepare_host_upgrade_fence' else ('{', '}')
    match = re.search(r'^' + brand + '_' + suffix + r'\(\) ' + re.escape(opener) + r'\n.*?^' + re.escape(closer) + '$', old_source, re.M | re.S)
    assert match, suffix
    definitions.append(match.group())

with tempfile.TemporaryDirectory(prefix=f'{brand}-old-fence-consumer-') as temporary:
    fixture = pathlib.Path(temporary)
    helper = fixture / 'known-helper'
    helper.write_text('  trap request_host_shutdown TERM INT HUP\n')
    helper.chmod(0o755)
    dropin = fixture / 'host.service.d'
    dropin.mkdir(mode=0o755)
    fence = dropin / '90-safe-upgrade.conf'
    expected = '[Unit]\nRefuseManualStart=yes\n\n[Service]\nKillMode=control-group\nSendSIGKILL=no\nRestart=no\n'
    fence.write_text(expected)
    fence.chmod(0o644)
    functions = '\n'.join(definitions).replace('0:0:', f'{os.getuid()}:{os.getgid()}:')
    functions = functions.replace(f'/run/{brand}-host-upgrade.', str(fixture / 'host-upgrade.'))
    body = functions + f'''
{brand}_legacy_host={shlex.quote(str(helper))}
{brand}_host_upgrade_dropin_dir={shlex.quote(str(dropin))}
{brand}_host_upgrade_dropin={shlex.quote(str(fence))}
{brand}_unit_is_quiescent() {{ return 0; }}
# Preserve real install/type/mode/link/inode checks; map only root ownership.
install() {{
  local -a arguments=()
  while (($#)); do case "$1" in -o|-g) shift 2 ;; *) arguments+=("$1"); shift ;; esac; done
  command install "${{arguments[@]}}"
}}
{brand}_select_upgrade_kill_mode
[[ "${{{brand}_upgrade_kill_mode}}" == mixed ]]
if {brand}_prepare_host_upgrade_fence; then exit 1; fi
'''
    completed = subprocess.run(['bash', '-c', body], capture_output=True, text=True)
    assert completed.returncode == 0, completed.stdout + completed.stderr
    assert fence.read_text() == expected, 'old consumer must not overwrite the mismatched incoming fence'

    archive = fixture / 'archive'
    archive.mkdir()
    (archive / '.INSTALL').write_text((root / f'packaging/linux/Arch/{brand}.install').read_text())
    directory = archive / f'usr/share/{brand}/installer-migrations'
    directory.mkdir(parents=True)
    for name in ('upgrade-2.0-lifecycle.sh', 'upgrade-2.0.1-lifecycle.sh'):
        (directory / name).write_text((root / 'scripts/linux/migrations' / name).read_text())
    candidate = fixture / 'incoming.pkg.tar'
    subprocess.run(['bsdtar', '-cf', str(candidate), '-C', str(archive), '.INSTALL', 'usr'], check=True)
    fixed_outgoing = fixture / 'fixed-outgoing'
    fixed_outgoing.write_text((root / f'packaging/linux/Arch/{brand}.install').read_text())
    for identity, outgoing, override in [
        (f'{brand} 2.0.1-1', baseline, True),
        (f'{brand} 1:2.0.1-1', baseline, True),
        (f'{brand} 2.1.0-1', baseline, False),
        (f'{brand} 2.0.1-1', fixed_outgoing, False),
    ]:
        body = f'''
source {shlex.quote(str(root / 'scripts/linux_install.sh'))}
workdir=$(mktemp -d {shlex.quote(str(fixture))}/installer.XXXXXX)
pacman() {{ printf '%s\\n' {shlex.quote(identity)}; }}
stage_upgrade_guard {shlex.quote(str(candidate))} {shlex.quote(str(outgoing))}
[[ -f "$workdir/hooks/00-{brand}-{'quiesce' if override else 'incoming-quiesce'}.hook" ]]
[[ ! -e "$workdir/hooks/00-{brand}-{'incoming-quiesce' if override else 'quiesce'}.hook" ]]
grep -Fxq AbortOnFail "$workdir/hooks/00-{brand}-{'quiesce' if override else 'incoming-quiesce'}.hook"
'''
        completed = subprocess.run(['bash', '-c', body], capture_output=True, text=True)
        assert completed.returncode == 0, (identity, completed.stdout, completed.stderr)
# Reaching prepare directly is deliberately rejected above. It does not mean
# the outgoing scriptlet reaches prepare after the incoming guard has proved
# complete quiescence: old preserved_quiesce reads only the first SIX lines.
with tempfile.TemporaryDirectory(prefix=f'{brand}-old-scriptlet-sequence-') as temporary:
    fixture = pathlib.Path(temporary)
    runtime = fixture / 'run'
    systemd = runtime / 'systemd/system'
    systemd.mkdir(parents=True)
    proc = fixture / 'proc'
    proc.mkdir()
    commands = fixture / 'bin'
    commands.mkdir()
    trace = fixture / 'trace'
    fence_dir = systemd / f'{brand}.service.d'
    fence_dir.mkdir(mode=0o755)
    fence = fence_dir / f'90-{brand}-safe-upgrade.conf'
    expected = '[Unit]\nRefuseManualStart=yes\n\n[Service]\nKillMode=control-group\nSendSIGKILL=no\nRestart=no\n'
    fence.write_text(expected)
    fence.chmod(0o644)
    control = 'vibeshine-vkms-control.socket'
    masks = (f'{brand}.service', f'{brand}-session-exec.socket', control,
             f'{brand}-session-restore@.service')
    for unit in masks:
        (systemd / unit).symlink_to('/dev/null')
    systemctl = commands / 'systemctl'
    systemctl.write_text(f'''#!/usr/bin/python3
import os, pathlib, sys
args = sys.argv[1:]
with pathlib.Path({str(trace)!r}).open('a') as log:
    log.write('systemctl ' + ' '.join(args) + '\\n')
masks = {masks!r}
controller = {f'{brand}-session-controller.service'!r}
known = set(masks) | {{controller, {f'{brand}-prelogin.service'!r}, {f'{brand}-machine-prepare.service'!r}, {f'{brand}-session-restore@mask-probe.service'!r}}}
if args[0] == 'list-units':
    sys.exit(0)
if args[0] == 'is-enabled':
    if args[1] in masks: print('masked-runtime')
    elif args[1] == controller: print('enabled')
    else: print('disabled')
    sys.exit(0)
if args[0] == 'show' and args[1] in known:
    unit = args[1]
    values = {{'LoadState': 'masked' if unit in masks or '@mask-probe.' in unit else 'loaded',
              'ActiveState': 'inactive', 'SubState': 'dead', 'MainPID': '0',
              'ControlGroup': '', 'FreezerState': 'running', 'InvocationID': '', 'Job': ''}}
    for arg in args[2:]:
        if not arg.startswith('--property=') or arg[11:] not in values: sys.exit(91)
        key = arg[11:]
        print(key + '=' + values[key])
    sys.exit(0)
# Only driver/KWin cleanup operations are allowed after the old proof.
if args in (["disable", "--now", {f'{brand}-prelogin.service'!r}],
            ["disable", "--now", "vibeshine-kwin-capability.path"],
            ["stop", "vibeshine-vkms.service"], ["stop", "vibeshine-drm-setup.service"]):
    sys.exit(0)
sys.exit(92)
''')
    systemctl.chmod(0o755)

    def redirect(text):
        text = text.replace('0:0:', f'{os.getuid()}:{os.getgid()}:')
        text = text.replace('/run/', str(runtime) + '/')
        text = text.replace('/usr/libexec/vibeshine/', str(fixture / 'libexec') + '/')
        text = text.replace(f'/usr/bin/{brand}', str(fixture / 'absent-host'))
        text = text.replace('/usr/bin/kwin_wayland', str(fixture / 'absent-kwin'))
        text = text.replace('/etc/systemd/', str(fixture / 'etc-systemd') + '/')
        text = text.replace('/sys/fs/cgroup', str(fixture / 'cgroup'))
        text = text.replace('pathlib.Path("/proc")', f'pathlib.Path({str(proc)!r})')
        text = text.replace('/proc/[0-9]*', str(proc) + '/[0-9]*')
        return text

    incoming = fixture / 'incoming.install'
    incoming.write_text(redirect((root / f'packaging/linux/Arch/{brand}.install').read_text()))
    outgoing = fixture / 'outgoing.install'
    outgoing.write_text(redirect(old_source))
    environment = dict(os.environ, PATH=str(commands) + ':' + os.environ['PATH'])
    body = f'''source {shlex.quote(str(incoming))}
export {brand.upper()}_PACKAGE_RUN_ID=22222222222222222222222222222222
{brand}_quiesce_or_abort
[[ $(cat {shlex.quote(str(runtime / (brand + '-package-transaction/phase')))}) == preflight ]]
'''
    completed = subprocess.run(['bash', '-c', body], env=environment, capture_output=True, text=True)
    assert completed.returncode == 0, completed.stdout + completed.stderr
    assert fence.read_text() == expected
    helpers = fixture / 'libexec'
    helpers.mkdir()
    # Outgoing installer-owned cleanup helpers only append to the private trace.
    for name in (f'{brand}-machine-host', 'vibeshine-ds5-install', 'vibeshine-drm-install'):
        helper = helpers / name
        helper.write_text('#!/bin/bash\nprintf "%s\\n" "' + name + ' $*" >> ' + shlex.quote(str(trace)) + '\n')
        helper.chmod(0o755)
    trace.write_text('')
    body = f'''source {shlex.quote(str(outgoing))}
{brand}_preserved_quiesce_is_safe || exit 81
# The enabled (quiescent) controller must not force the old late branch.
pre_remove 2.0.1-1 || exit 82
'''
    completed = subprocess.run(['bash', '-c', body], env=environment, capture_output=True, text=True)
    assert completed.returncode == 0, completed.stdout + completed.stderr + trace.read_text()
    lines = trace.read_text().splitlines()
    for cleanup in (f'{brand}-machine-host remove-pam', 'vibeshine-ds5-install remove', 'vibeshine-drm-install remove'):
        assert cleanup in lines, (cleanup, lines)
    assert fence.read_text() == expected, 'legacy shortcut must preserve full incoming fence'
    assert all((systemd / unit).is_symlink() for unit in masks)
    assert f'systemctl is-enabled {brand}-session-controller.service' not in lines
    assert not any('daemon-reload' in line or 'mask --runtime' in line for line in lines)
print(f'{brand}: known 2.0.1 direct prepare mismatch reproduced; actual incoming full proof → verbatim old pre_remove succeeds, keeps seven-line fence/four masks, reaches driver cleanup with enabled quiescent controller')
