#!/usr/bin/env python3
"""Real cleanup helpers must distinguish missing cgroups from denied lookup.

Only private directories and subprocesses are used. Permission/I/O errors are
also injected at lstat so the regression remains useful under privileged CI.
"""
import errno
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
pattern = r'^(' + BRAND + r'(?:_preun)?_cgroup_is_quiescent|session_app_control_group_is_empty|control_group_is_empty)\(\) \{\n.*?^\}'
helpers = []
for path in (ROOT / 'packaging/linux').rglob('*'):
    if not path.is_file() or not (path.suffix in ('.install', '.in', '.spec') or path.name in (BRAND + '-session-controller', BRAND + '-machine-host')):
        continue
    for match in re.finditer(pattern, path.read_text().replace('%%', '%'), re.M | re.S):
        helpers.append((str(path.relative_to(ROOT)), match.group(1), match.group()))
assert len(helpers) == 11, len(helpers)
programs = [re.search(r"<<'PY'\n(.*?)\nPY\n", body, re.S).group(1) for _, _, body in helpers]
assert len(set(programs)) == 1, 'errno-aware absence proofs drifted'

count = 0
with tempfile.TemporaryDirectory(prefix=BRAND + '-cgroup-absence-') as tmp:
    stage = Path(tmp)
    wrapper = stage / 'python-fixture'
    wrapper.write_text('''#!/usr/bin/python3
import errno
import os
from pathlib import Path
import sys
program = sys.stdin.read()
target = sys.argv[-1]
sys.argv = ['-', target]
case = os.environ.get('CGROUP_FAULT', '')
if case in ('eacces', 'eperm', 'eio', 'enotdir'):
    value = {'eacces': errno.EACCES, 'eperm': errno.EPERM, 'eio': errno.EIO, 'enotdir': errno.ENOTDIR}[case]
    def denied(path):
        raise OSError(value, 'private cgroup lookup fault', path)
    os.lstat = denied
elif case == 'appeared':
    Path(target).mkdir(parents=True)
exec(compile(program, '<production cgroup absence proof>', 'exec'))
''')
    wrapper.chmod(0o755)
    sentinel = stage / 'administrator-policy'
    sentinel.write_bytes(b'preserve administrator masks and configuration\n')
    for index, (path, name, definition) in enumerate(helpers):
        base = stage / str(index)
        base.mkdir()
        (base / 'empty').mkdir()
        (base / 'empty/cgroup.events').write_text('populated 0\n')
        (base / 'populated').mkdir()
        (base / 'populated/cgroup.events').write_text('populated 1\n')
        (base / 'no-events').mkdir()
        (base / 'file-parent').write_text('not a directory')
        (base / 'alias').symlink_to(base / 'empty')
        (base / 'dangling').symlink_to(base / 'missing')
        denied = base / 'denied'
        (denied / 'worker.scope').mkdir(parents=True)
        (denied / 'worker.scope/cgroup.events').write_text('populated 1\n')
        code = definition.replace('/sys/fs/cgroup', str(base))
        shell = 'bash' if path.endswith(('.install', '-session-controller', '-machine-host')) else 'sh'
        cases = (
            ('', '', True), ('/missing', '', True), ('/missing-parent/child', '', True),
            ('/empty', '', True), ('/populated', '', False), ('/no-events', '', False),
            ('/alias', '', False), ('/dangling', '', False), ('/file-parent/child', '', False),
            ('relative', '', False), ('/../outside', '', False),
            ('/model-eacces', 'eacces', False), ('/model-eperm', 'eperm', False),
            ('/model-eio', 'eio', False), ('/model-enotdir', 'enotdir', False),
            ('/appeared', 'appeared', False),
        )
        for group, fault, expected in cases:
            tested = code.replace('/usr/bin/python3', shlex.quote(str(wrapper))) if fault else code
            result = subprocess.run([shell, '-c', tested + '\n' + name + ' "$1"\n', '-', group],
                                    env=dict(os.environ, CGROUP_FAULT=fault), capture_output=True, text=True, timeout=10)
            assert (result.returncode == 0) == expected, (path, name, group, fault, result.returncode, result.stderr)
            assert 'Traceback' not in result.stderr, (path, result.stderr)
            count += 1
        # Missing trusted Python or a timed-out lookup cannot prove absence.
        for tested in (code.replace('/usr/bin/python3', str(stage / 'missing-python')),
                       'timeout() { return 124; }\n' + code):
            result = subprocess.run([shell, '-c', tested + '\n' + name + ' /timeout-missing\n'], capture_output=True, text=True, timeout=10)
            assert result.returncode != 0, (path, result.stderr)
            count += 1
        denied.chmod(0)
        try:
            if os.geteuid() != 0:
                try:
                    (denied / 'worker.scope').lstat()
                    raise AssertionError('private ancestor did not deny inspection')
                except PermissionError as error:
                    assert error.errno == errno.EACCES
            result = subprocess.run([shell, '-c', code + '\n' + name + ' /denied/worker.scope\n'], capture_output=True, text=True, timeout=10)
            assert result.returncode != 0, (path, 'inaccessible or populated cgroup accepted', result.stderr)
            count += 1
            if index == 0 and os.geteuid() != 0:
                old = gzip.decompress((ROOT / f'tests/unit/platform/linux/fixtures/package-2.0.2-fuse/{BRAND}.install.gz').read_bytes()).decode()
                old_helper = re.search(r'^' + BRAND + r'_cgroup_is_quiescent\(\) \{\n.*?^\}', old, re.M | re.S).group()
                old_helper = old_helper.replace('/sys/fs/cgroup', str(base))
                result = subprocess.run(['bash', '-c', old_helper + '\n' + BRAND + '_cgroup_is_quiescent /denied/worker.scope\n'], capture_output=True, text=True, timeout=10)
                assert result.returncode == 0, ('immutable old hook did not reproduce false absence', result.stderr)
        finally:
            denied.chmod(0o700)
        assert sentinel.read_bytes() == b'preserve administrator masks and configuration\n'
print(f'{BRAND}: {count} errno-aware cgroup cases across {len(helpers)} controller/host/package helpers passed; true absence and empty groups preserved, denied lookup blocks')
