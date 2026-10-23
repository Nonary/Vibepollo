#!/usr/bin/env python3
"""Real socket property shapes and timed-out freeze state, without host calls."""
import pathlib
import re
import shlex
import shutil
import subprocess
import sys

root = pathlib.Path(sys.argv[1]).resolve()
brand = sys.argv[2] if len(sys.argv) > 2 else 'vibepollo'
assert brand in ('vibepollo', 'vibeshine')
rpm = (root / 'packaging/linux/copr/Sunshine.spec').read_text()
contexts = [('Arch', (root / f'packaging/linux/Arch/{brand}.install').read_text(), brand)]
contexts += [(kind, (root / 'packaging/linux' / ('vibeshine-preinst.in' if kind == 'preinst' else 'vibeshine-postrm.in' if kind == 'postrm' else f'{brand}-{kind}.in')).read_text(), brand) for kind in ('preinst', 'postinst', 'prerm', 'postrm')]
for section, following in [('pre', 'post'), ('post', 'preun'), ('preun', 'postun'), ('postun', 'files')]:
    body = rpm.split(f'\n%{section}\n', 1)[1].split(f'\n%{following}\n', 1)[0].replace('%%', '%')
    contexts.append(('RPM ' + section, body, brand + '_preun' if section == 'preun' else brand))

def function(source, name):
    match = re.search(r'^' + name + r'\(\) \{\n.*?^\}$', source, re.M | re.S)
    assert match, name
    return match.group()

count = 0
for label, source, prefix in contexts:
    shells = ['bash'] if label == 'Arch' else ['sh', 'bash'] + ([shutil.which('dash')] if shutil.which('dash') else [])
    definition = function(source, prefix + '_unit_is_quiescent')
    for shell in shells:
        for load in ('loaded', 'masked', 'not-found'):
            props = f'LoadState={load}\nActiveState=inactive\nSubState=dead\nControlGroup=\n'
            for unit, extra, expected in [('socket', '', 0), ('socket', 'MainPID=0\n', 0), ('socket', 'MainPID=1\n', 1), ('socket', 'MainPID=0\nMainPID=0\n', 1), ('service', '', 1), ('service', 'MainPID=0\n', 0)]:
                code = definition + f'\n{prefix}_cgroup_is_quiescent() {{ [ -z "$1" ]; }}\ntimeout() {{ printf "%s" {shlex.quote(props + extra)}; }}\n{prefix}_unit_is_quiescent {brand}.{unit}\n'
                result = subprocess.run([shell, '-c', code], capture_output=True, text=True)
                assert (result.returncode == 0) == (expected == 0), (label, shell, unit, extra, result.stderr)
                count += 1
        if label in ('Arch', 'preinst', 'postinst', 'RPM pre', 'RPM post'):
            definitions = function(source, brand + '_freeze_controller') + '\n' + function(source, brand + '_package_properties_are_unique')
            props = f'ActiveState=active\nSubState=running\nMainPID=123\nControlGroup=/system.slice/{brand}-session-controller.service\nFreezerState=running\nInvocationID=' + 'a' * 32 + '\n'
            code = definitions + f'''
{brand}_controller_was_frozen=0
{brand}_unit_is_quiescent() {{ return 1; }}
timeout() {{
  case "$*" in *'systemctl freeze '*) return 124 ;; esac
  printf '%s' {shlex.quote(props)}
}}
if {brand}_freeze_controller; then exit 90; fi
[ "${{{brand}_controller_was_frozen}}" = 1 ]
'''
            result = subprocess.run([shell, '-c', code], capture_output=True, text=True)
            assert result.returncode == 0, (label, shell, result.stderr)
            count += 1
print(f'{brand}: {count} real socket-shape and timed-out freeze metadata cases passed across standalone package contexts')
