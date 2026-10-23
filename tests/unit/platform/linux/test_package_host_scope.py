#!/usr/bin/env python3
"""Read-only cross-scope executable proofs with private real ELF processes."""
import json
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[4]
brand = sys.argv[2] if len(sys.argv) > 2 else 'vibepollo'
assert brand in ('vibepollo', 'vibeshine')
source = root / f'packaging/linux/Arch/{brand}.install'
text = source.read_text()
names = (f'{brand}_package_capture_host_executables', f'{brand}_package_unmanaged_host_absent')
functions = '\n'.join(re.search(r'^' + name + r'\(\) \{\n.*?^\}$', text, re.M | re.S).group() for name in names)
paths = (f'/usr/bin/{brand}', f'/usr/libexec/vibeshine/{brand}-host')

def run(definitions, command, directory, force='0'):
    # Redirect only the existing root-private record boundary into the fixture.
    mock = f'''\n{brand}_package_transaction={shlex.quote(str(directory))}
{brand}_package_record_is_safe() {{ [ -f "$1" ] && [ ! -L "$1" ]; }}
'''
    return subprocess.run(['bash', '-c', definitions + mock + command], env={'PATH': '/usr/bin:/bin', f'{brand.upper()}_PACKAGE_FORCE': force}, capture_output=True, text=True)

with tempfile.TemporaryDirectory(prefix=f'{brand}-host-scope-') as temporary:
    fixture = pathlib.Path(temporary)
    proc = fixture / 'proc'
    proc.mkdir()
    proof = functions.replace('pathlib.Path("/proc")', f'pathlib.Path({str(proc)!r})').replace('/proc/[0-9]*/exe', str(proc) + '/[0-9]*/exe')
    record = fixture / 'host-executables.before'
    record.write_text('[]')
    assert run(proof, f'{names[1]}', fixture).returncode == 0
    # Capture actual immutable installed root-owned binaries as the two fixed
    # paths. This validates the production metadata/ancestry code without edits.
    capture = functions.replace(paths[0], '/usr/bin/sleep').replace(paths[1], '/usr/bin/true')
    # The executor maps immutable host files to nobody; redirect only that
    # ownership boundary while retaining actual inode, mode and ancestry checks.
    mapped_root_uid = pathlib.Path('/usr/bin/sleep').stat().st_uid
    capture = capture.replace('st_uid != 0', f'st_uid != {mapped_root_uid}')
    result = run(capture, f'{names[0]}', fixture)
    assert result.returncode == 0, result.stderr
    assert len(json.loads(result.stdout)) == 2
    missing = capture.replace('/usr/bin/python3', '/nonexistent/trusted-python')
    assert run(missing, f'{names[0]}', fixture).returncode != 0
    absent = functions.replace(paths[0], str(fixture / 'absent-public')).replace(paths[1], str(fixture / 'absent-private')).replace('/usr/bin/python3', '/nonexistent/trusted-python')
    assert run(absent, f'{names[0]}', fixture).stdout.strip() == '[]'
    assert run(proof.replace('/usr/bin/python3', '/nonexistent/trusted-python'), f'{names[1]}', fixture).returncode == 0
    # Unsafe links and writable/foreign owned candidates fail source validation.
    unsafe = fixture / 'unsafe'
    unsafe.symlink_to('/usr/bin/sleep')
    assert run(functions.replace(paths[0], str(unsafe)), f'{names[0]}', fixture).returncode != 0
    unsafe.unlink()
    shutil.copyfile('/usr/bin/sleep', unsafe)
    unsafe.chmod(0o777)
    assert run(functions.replace(paths[0], str(unsafe)), f'{names[0]}', fixture).returncode != 0

    # The kernel process fixture uses a copied ELF inode. Its private metadata
    # record models the previously validated root snapshot; no process names or
    # argv are used by the actual proof. A distinct inode remains unrelated.
    host_binary = fixture / 'legacy-host'
    shutil.copyfile('/usr/bin/sleep', host_binary)
    host_binary.chmod(0o755)
    identity = host_binary.stat()
    snapshot = [{'path': paths[0], 'device': identity.st_dev, 'inode': identity.st_ino}]
    record.write_text(json.dumps(snapshot))
    assert run(proof.replace('/usr/bin/python3', '/nonexistent/trusted-python'), f'{names[1]}', fixture).returncode != 0
    host = subprocess.Popen([str(host_binary), '30'], start_new_session=True)
    sibling = subprocess.Popen(['/usr/bin/sleep', '30'], start_new_session=True)
    try:
        (proc / str(host.pid)).symlink_to(f'/proc/{host.pid}', target_is_directory=True)
        (proc / str(sibling.pid)).symlink_to(f'/proc/{sibling.pid}', target_is_directory=True)
        for force in ('0', '1'):
            result = run(proof, f'{names[1]}', fixture, force)
            assert result.returncode != 0 and f'PID {host.pid}' in result.stderr, result.stderr
            assert f'UID {os.getuid()}' in result.stderr and 'scope ' in result.stderr, result.stderr
            assert json.dumps(pathlib.Path(f'/proc/{host.pid}/cgroup').read_text().rstrip('\n')) in result.stderr, result.stderr
            assert 'activation authority and session cleanup ownership' in result.stderr, result.stderr
            assert host.poll() is None and sibling.poll() is None, 'proof signalled a process'
        # Missing diagnostic metadata cannot turn positive old-inode evidence
        # into absence, or invent a UID/scope for a recycled process.
        uncertain = proc / '999999998'
        uncertain.mkdir()
        (uncertain / 'exe').symlink_to(f'/proc/{host.pid}/exe')
        (proc / str(host.pid)).unlink()
        result = run(proof, f'{names[1]}', fixture, '1')
        assert result.returncode != 0 and 'PID 999999998, UID unknown' in result.stderr, result.stderr
        assert 'scope \"<unavailable>\"' in result.stderr, result.stderr
        (uncertain / 'exe').unlink()
        uncertain.rmdir()
        (proc / str(host.pid)).symlink_to(f'/proc/{host.pid}', target_is_directory=True)
        # Deleted ELF still matches retained inode and blocks across scopes.
        host_binary.unlink()
        result = run(proof, f'{names[1]}', fixture, '1')
        assert result.returncode != 0 and host.poll() is None, result.stderr
        # Positive fallback is exact deleted path with trusted owner only. The
        # fixture redirects that fixed path/UID boundary, preserving real stat.
        record.write_text('[]')
        deleted = proof.replace(paths[0], str(host_binary)).replace('identity.st_uid == 0', 'identity.st_uid == os.getuid()')
        assert run(deleted, f'{names[1]}', fixture).returncode != 0
        missing_python_deleted = deleted.replace('/usr/bin/python3', '/nonexistent/trusted-python').replace('0:regular file', f'{identity.st_uid}:regular file')
        assert run(missing_python_deleted, f'{names[1]}', fixture).returncode != 0
        record.write_text(json.dumps(snapshot))
        # A recycled/stale PID entry that points at another executable inode
        # contributes no false ownership and leaves the unrelated process alive.
        (proc / str(host.pid)).unlink()
        (proc / str(host.pid)).symlink_to(f'/proc/{sibling.pid}', target_is_directory=True)
        assert run(proof, f'{names[1]}', fixture).returncode == 0
        assert sibling.poll() is None
        (proc / '999999999').symlink_to('/proc/999999999', target_is_directory=True)
        assert run(proof, f'{names[1]}', fixture).returncode == 0
        for malformed in ('{}', '[{"path":"/other","device":1,"inode":1}]', '[{"path":' + json.dumps(paths[0]) + ',"device":true,"inode":1}]', json.dumps(snapshot * 2)):
            record.write_text(malformed)
            assert run(proof, f'{names[1]}', fixture).returncode != 0
    finally:
        for child in (host, sibling):
            if child.poll() is None:
                child.terminate()
            child.wait(timeout=3)
print(f'PASS: {brand} exact executable inode scope proof; legacy/deleted hosts block without signals, unrelated and recycled-PID inode survive')
