#!/usr/bin/env python3
"""Late cleanup failure attribution with production quiescers and private paths."""
import gzip
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[4]
BRAND = sys.argv[2] if len(sys.argv) > 2 else 'vibepollo'
paths = [f'packaging/linux/Arch/{BRAND}.install', 'packaging/linux/vibeshine-preinst.in', f'packaging/linux/{BRAND}-postinst.in', 'packaging/linux/copr/Sunshine.spec']
contexts = []
for path in paths:
    source = (ROOT / path).read_text().replace('%%', '%')
    for match in re.finditer(r'^(?:do_quiesce_machine_host|' + BRAND + r'_quiesce_machine_host)\(\) \{\n.*?^\}', source, re.M | re.S):
        contexts.append((path, source, match.group()))
assert len(contexts) == 5


def exercise(source, quiescer, failed):
    name = quiescer.split('()', 1)[0]
    helpers = set(re.findall(r'^(' + BRAND + r'_[a-z0-9_]+)\(\)', source, re.M))
    helpers.discard(name)
    vq = re.search(r'^vq\(\) \{\n.*?^\}', source, re.M | re.S).group()
    with tempfile.TemporaryDirectory(prefix=BRAND + '-quiesce-label-') as tmp:
        stage = Path(tmp)
        (stage / 'systemd').mkdir()
        code = '\n'.join(helper + '() { return 0; }' for helper in sorted(helpers)) + '\n' + vq + '\n' + quiescer
        code = code.replace('/run/systemd/system', str(stage / 'systemd')).replace('/run/' + BRAND, str(stage / 'runtime'))
        globals = ['controller', 'legacy_host', 'legacy_handoff', 'runtime_root', 'broker_socket', 'control_socket', 'legacy_handoff_directory', 'legacy_restore_directory', 'legacy_transition_lock', 'legacy_prelogin_marker']
        code += '\n' + '\n'.join(BRAND + '_' + field + '=' + shlex.quote(str(stage / ('absent-' + field))) for field in globals)
        code += '\n' + r'''
systemctl() { echo 'unexpected manager invocation' >&2; return 99; }
product_preserved_quiesce_is_safe() { return 1; }
product_package_preserved_preflight_is_safe() { return 1; }
product_controller_was_frozen=0
product_quiesce_step=''
controller_checked=0
product_unit_is_quiescent() { if [ "$1" = product-session-controller.service ]; then controller_checked=1; fi; return 0; }
'''.replace('product', BRAND)
        if failed == 'legacy-state':
            (stage / 'absent-legacy_transition_lock').write_text('retained legacy state')
        elif failed == 'record':
            record = stage / 'runtime/session.env'
            record.parent.mkdir()
            record.write_text('retained trusted record')
        else:
            code += f'\n{BRAND}_{failed}() {{ [ "$controller_checked" = 1 ] || return 99; return 1; }}\n'
        code += f'\n{name}\nstatus=$?\nprintf \'status=%s checked=%s step=%s\\n\' "$status" "$controller_checked" "${{{BRAND}_quiesce_step}}"\n'
        result = subprocess.run(['bash', '-c', code], capture_output=True, text=True, timeout=5)
        assert result.returncode == 0 and 'unexpected manager' not in result.stderr, result.stderr
        return result.stdout


old = gzip.decompress((ROOT / f'tests/unit/platform/linux/fixtures/package-2.0.2-fuse/{BRAND}.install.gz').read_bytes()).decode()
quiescer = re.search(r'^do_quiesce_machine_host\(\) \{\n.*?^\}', old, re.M | re.S).group()
result = exercise(old, quiescer, 'cleanup_legacy_transition_state')
assert 'status=1 checked=1 step=' + BRAND + '_unit_is_quiescent ' + BRAND + '-session-controller.service' in result, result
count = 0
for path, source, quiescer in contexts:
    for failed, expected in (
        ('runtime_root_is_safe_or_absent', BRAND + '_runtime_root_is_safe_or_absent'),
        ('run_optional_legacy_command', BRAND + '_run_optional_legacy_command remove-pam'),
        ('cleanup_legacy_transition_state', BRAND + '_cleanup_legacy_transition_state'),
        ('record', 'verify trusted session record is absent'),
        ('legacy-state', 'verify broker and legacy runtime state is absent'),
    ):
        result = exercise(source, quiescer, failed)
        assert 'status=1 checked=1 step=' + expected in result, (path, failed, result)
        count += 1
print(f'{BRAND}: reproduced stale controller label in qualified old hook; {count} late-failure attribution cases passed across all five install quiescers')
