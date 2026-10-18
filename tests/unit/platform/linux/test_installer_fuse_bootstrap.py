#!/usr/bin/env python3
"""Exact installed-daily migration and incoming AbortOnFail guard, privately modeled.

No installed old hook is sourced, no native package transaction runs, and all
service/proc/runtime paths are private. Old bytes are immutable gzip fixtures.
"""
import gzip
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

COMMANDS = None
ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[4]
BRAND = sys.argv[2] if len(sys.argv) > 2 else 'vibepollo'
MIGRATION = 'upgrade-2.0.2-fuse-scan.sh'
installed = BRAND + ' 2.0.2daily.d20261017.2.416e6dec2b51.r416e6dec2b515b2e0f38f6329397a4d393dcd94f-1'
source = (ROOT / f'packaging/linux/Arch/{BRAND}.install').read_text()
old = gzip.decompress((ROOT / f'tests/unit/platform/linux/fixtures/package-2.0.2-fuse/{BRAND}.install.gz').read_bytes())
migration = (ROOT / 'scripts/linux/migrations' / MIGRATION).read_text()
assert hashlib.sha256(old).hexdigest() in migration
assert f'"${{CMAKE_SOURCE_DIR}}/scripts/linux/migrations/{MIGRATION}"' in (ROOT / 'cmake/packaging/linux.cmake').read_text()
assert 'installer_fuse_bootstrap' in (ROOT / 'tests/CMakeLists.txt').read_text()
spec = importlib.util.spec_from_file_location('fuse_fault', ROOT / 'tests/unit/platform/linux/fixtures/package_fuse_fault.py')
fault = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fault)


def run(body, **environment):
    code = f'source {shlex.quote(str(ROOT / "scripts/linux_install.sh"))}\n' + body
    environment = dict(os.environ, **environment)
    if COMMANDS is not None: environment['PATH'] = str(COMMANDS) + ':' + environment['PATH']
    return subprocess.run(['bash', '-c', code], env=environment, capture_output=True, text=True, timeout=20)


count = 0
with tempfile.TemporaryDirectory(prefix=BRAND + '-fuse-bootstrap-') as tmp:
    stage = Path(tmp)
    COMMANDS = stage / 'commands'
    COMMANDS.mkdir()
    pacman = COMMANDS / 'pacman'
    pacman.write_text('#!/bin/sh\n[ "$*" = "-Q BRAND" ] || exit 99\nprintf "%s\\n" "$identity"\n'.replace('BRAND', BRAND))
    pacman.chmod(0o755)
    archive = stage / 'archive'
    directory = archive / f'usr/share/{BRAND}/installer-migrations'
    directory.mkdir(parents=True)
    (directory / MIGRATION).write_text(migration)
    # An unknown archive script must never execute during migration discovery.
    (directory / 'unrecognized.sh').write_text('touch ' + shlex.quote(str(stage / 'unexpected-execution')) + '\n')
    (archive / '.INSTALL').write_text(source)
    candidate = stage / 'candidate.pkg.tar'
    subprocess.run(['bsdtar', '-cf', str(candidate), '-C', str(archive), '.INSTALL', 'usr'], check=True)
    outgoing = stage / 'outgoing.install'
    outgoing.write_bytes(old)
    baseline = hashlib.sha256(outgoing.read_bytes()).hexdigest()
    for identity, changed, expected in (
        (installed, False, True),
        (BRAND + ' 1:' + installed.split(' ', 1)[1], False, True),
        (BRAND + ' 2.0.2daily.d20261017.2-1', False, False),
        (installed.replace('416e6dec2b51', 'deadbeef0000', 1), False, False),
        (installed.replace('.r416e6dec2b515b2e0f38f6329397a4d393dcd94f', ''), False, True),
        (installed.replace('d20261017.2', 'd20261016.2'), False, False),
        (installed[:-1] + '2', False, False),
        (BRAND + ' 2.0.2-1', False, False),
        (BRAND + ' 2.0.3daily.d20261017.2-1', False, False),
        (installed + '\n' + installed, False, False),
        (installed, True, False),
    ):
        outgoing.write_bytes(old + (b'\n# administrator edit\n' if changed else b''))
        before = outgoing.read_bytes()
        work = stage / ('scope-' + str(count))
        body = f'''
workdir={shlex.quote(str(work))}
mkdir -p "$workdir"
pacman() {{ printf '%s\\n' "$identity"; }}
stage_upgrade_guard {shlex.quote(str(candidate))} {shlex.quote(str(outgoing))}
[[ -f "$workdir/hooks/00-{BRAND}-{'quiesce' if expected else 'incoming-quiesce'}.hook" ]]
grep -Fxq AbortOnFail "$workdir/hooks/00-{BRAND}-{'quiesce' if expected else 'incoming-quiesce'}.hook"
'''
        result = run(body, identity=identity)
        assert result.returncode == 0, (identity, changed, result.stderr)
        assert outgoing.read_bytes() == before
        assert not (stage / 'unexpected-execution').exists()
        count += 1
    outgoing.write_bytes(old)
    assert hashlib.sha256(outgoing.read_bytes()).hexdigest() == baseline
    # A recognized installed hook cannot be overridden with an unfixed candidate.
    (archive / '.INSTALL').write_bytes(old)
    unfixed = stage / 'unfixed.pkg.tar'
    subprocess.run(['bsdtar', '-cf', str(unfixed), '-C', str(archive), '.INSTALL', 'usr'], check=True)
    result = run(f'workdir={shlex.quote(str(stage / "unfixed"))}; mkdir -p "$workdir"\npacman() {{ printf \'%s\\n\' "$identity"; }}\nstage_upgrade_guard {shlex.quote(str(unfixed))} {shlex.quote(str(outgoing))}', identity=installed)
    assert result.returncode != 0 and 'FUSE-scan compatibility check failed' in result.stderr
    # Symlinked and already fixed installed libraries retain their own guard.
    target = stage / 'old-target.install'
    target.write_bytes(old)
    for variant in ('symlink', 'fixed'):
        outgoing.unlink()
        if variant == 'symlink': outgoing.symlink_to(target)
        else: outgoing.write_text(source)
        result = run(f'''
workdir={shlex.quote(str(stage / variant))}; mkdir -p "$workdir"
pacman() {{ printf '%s\\n' "$identity"; }}
stage_upgrade_guard {shlex.quote(str(candidate))} {shlex.quote(str(outgoing))}
[[ -f "$workdir/hooks/00-{BRAND}-incoming-quiesce.hook" ]]
''', identity=installed)
        assert result.returncode == 0, (variant, result.stderr)
    outgoing.write_bytes(old)
    duplicate = stage / 'duplicate.pkg.tar'
    duplicate.write_bytes(candidate.read_bytes())
    subprocess.run(['bsdtar', '-rf', str(duplicate), '-C', str(archive), f'usr/share/{BRAND}/installer-migrations/{MIGRATION}'], check=True)
    result = run(f'''
workdir={shlex.quote(str(stage / 'duplicate'))}; mkdir -p "$workdir"
pacman() {{ printf '%s\\n' "$identity"; }}
stage_upgrade_guard {shlex.quote(str(duplicate))} {shlex.quote(str(outgoing))}
''', identity=installed)
    assert result.returncode != 0 and 'duplicate compatibility scripts' in result.stderr

    # An administrator hook collision aborts rather than discarding that policy.
    admin = stage / 'administrator-hooks'
    admin.mkdir()
    collision = admin / f'00-{BRAND}-quiesce.hook'
    collision.symlink_to('/dev/null')
    inode = collision.lstat().st_ino
    result = run(f'''
workdir={shlex.quote(str(stage / 'collision'))}; mkdir -p "$workdir"
pacman() {{ printf '%s\\n' "$identity"; }}
pacman-conf() {{ printf '%s\\n' {shlex.quote(str(admin))}; }}
stage_upgrade_guard {shlex.quote(str(candidate))} {shlex.quote(str(outgoing))}
prepare_package_hook_dirs
''', identity=installed)
    assert result.returncode != 0 and 'administrator hook' in result.stderr
    assert collision.is_symlink() and collision.lstat().st_ino == inode
    # Re-staging must retire a previously selected override before scope changes.
    result = run(f'''
workdir={shlex.quote(str(stage / 'restage'))}; mkdir -p "$workdir"
pacman() {{ printf '%s\\n' "$identity"; }}
identity={shlex.quote(installed)}
stage_upgrade_guard {shlex.quote(str(candidate))} {shlex.quote(str(outgoing))}
[[ -f "$workdir/hooks/00-{BRAND}-quiesce.hook" ]]
identity='{BRAND} 9.0.0-1'
stage_upgrade_guard {shlex.quote(str(candidate))} {shlex.quote(str(outgoing))}
[[ ! -e "$workdir/hooks/00-{BRAND}-quiesce.hook" && -f "$workdir/hooks/00-{BRAND}-incoming-quiesce.hook" ]]
''')
    assert result.returncode == 0, result.stderr

    # The same fingerprint/version must still qualify when ALPM runs preflight.
    # A harmless entrypoint marker makes a missed recheck safe and observable.
    entered = stage / 'unexpected-shutdown'
    for change in ('edit', 'version', 'symlink', 'missing'):
        outgoing.write_bytes(old)
        if entered.exists(): entered.unlink()
        work = stage / ('recheck-' + change)
        mutation = {
            'edit': 'printf "\\n# changed after staging\\n" >> ' + shlex.quote(str(outgoing)),
            'version': 'identity=' + shlex.quote(BRAND + ' 9.0.0-1'),
            'symlink': 'mv ' + shlex.quote(str(outgoing)) + ' ' + shlex.quote(str(stage / 'saved-hook')) + '; ln -s ' + shlex.quote(str(stage / 'saved-hook')) + ' ' + shlex.quote(str(outgoing)),
            'missing': 'rm ' + shlex.quote(str(outgoing)),
        }[change]
        result = run(f'''
workdir={shlex.quote(str(work))}; mkdir -p "$workdir"
pacman() {{ printf '%s\\n' "$identity"; }}
stage_upgrade_guard {shlex.quote(str(candidate))} {shlex.quote(str(outgoing))}
printf '\\n{BRAND}_quiesce_or_abort() {{ touch %s; }}\\n' {shlex.quote(str(entered))} >> "$workdir/arch-package-hooks"
{mutation}
"$workdir/preflight"
''', identity=installed)
        assert result.returncode != 0 and 'changed after staging' in result.stderr, (change, result.stderr)
        assert not entered.exists(), 'changed installed hook reached new shutdown'
        if outgoing.is_symlink(): outgoing.unlink()
        outgoing.write_bytes(old)

    # Run the complete incoming package library staged by the actual installer.
    # Only metadata/syscall and manager boundaries are redirected to fixtures.
    for case in ('unrelated-fuse', 'same-device', 'controller-busy', 'failed-transaction'):
        base = stage / case
        base.mkdir()
        (base / 'proc').mkdir()
        (base / 'proc/100').symlink_to('/proc/' + str(os.getpid()))
        (base / 'systemd').mkdir()
        admin_mask = base / 'systemd/vibeshine-vkms-control.socket'
        admin_mask.symlink_to('/dev/null')
        original_mask = admin_mask.lstat().st_ino
        sentinel = base / 'configuration'
        sentinel.write_bytes(b'pairing and administrator policy\n')
        identity = Path('/proc/self/exe').stat()
        snapshot = json.dumps([dict(path='/usr/bin/' + BRAND, device=identity.st_dev, inode=identity.st_ino)])
        if case == 'unrelated-fuse':
            old_function = re.search(r'^' + BRAND + r'_package_unmanaged_host_absent\(\) \{\n.*?^\}', old.decode(), re.M | re.S).group()
            old_proof = old_function.split("<<'PY'\n", 1)[1].split('\nPY\n', 1)[0]
            old_proof = old_proof.replace('pathlib.Path("/proc")', f'pathlib.Path({str(base / "proc")!r})')
            baseline = subprocess.run([sys.executable, '-I', '-', snapshot, 'unrelated-fuse', str(base / 'proc'), str(identity.st_dev)], input=fault.prelude('/usr/bin/' + BRAND) + '\n' + old_proof, capture_output=True, text=True, timeout=5)
            assert baseline.returncode != 0 and 'fixture FUSE getattr denial' in baseline.stderr
        code = source.replace('0:0:', f'{os.getuid()}:{os.getgid()}:')
        code = code.replace(f'/run/{BRAND}', str(base / 'runtime')).replace('/run/vibeshine/', str(base / 'runtime') + '/').replace('/run/systemd/system', str(base / 'systemd'))
        code = code.replace(f'/usr/libexec/vibeshine/{BRAND}', str(base / 'absent-helper')).replace(f'/usr/bin/{BRAND}', str(base / 'absent-public'))
        # Keep the retained snapshot's canonical package path contract.
        code = code.replace(f'paths = ("{base}/absent-public", "{base}/absent-helper-host")', f'paths = ("/usr/bin/{BRAND}", "/usr/libexec/vibeshine/{BRAND}-host")')
        code = code.replace('pathlib.Path("/proc")', f'pathlib.Path({str(base / "proc")!r})')
        code = code.replace('"$' + BRAND + '_package_executable_records" <<\'PY\'', '"$' + BRAND + '_package_executable_records" "$identity_case" "$fixture/proc" "$old_device" <<\'PY\'')
        # Inject only into the proof heredoc, after capture code remains intact.
        proof_start = code.index('def inaccessible_fuse_is_unrelated')
        heredoc = code.rfind("<<'PY'\n", 0, proof_start) + len("<<'PY'\n")
        code = code[:heredoc] + fault.prelude('/usr/bin/' + BRAND) + '\n' + code[heredoc:]
        code += '\n' + BRAND + '_package_capture_host_executables() { printf \'%s\\n\' ' + shlex.quote(snapshot) + '; }\n'
        code += r'''
command() { if [[ "$1" = -v && "$2" = systemctl ]]; then return 1; else builtin command "$@"; fi; }
systemctl() {
  local unit=${2:-} property value
  case "$1" in
    show)
      shift 2
      for property do
        case "$property" in
          --property=Job) value= ;;
          --property=LoadState) value=loaded ;;
          --property=ActiveState) if [[ "$unit" = product-session-controller.service && "$manager_state" = busy ]]; then value=active; else value=inactive; fi ;;
          --property=SubState) if [[ "$unit" = product-session-controller.service && "$manager_state" = busy ]]; then value=running; else value=dead; fi ;;
          --property=MainPID) if [[ "$unit" = product-session-controller.service && "$manager_state" = busy ]]; then value=123; else value=0; fi ;;
          --property=ControlGroup|--property=FreezerState|--property=InvocationID) value= ;;
          *) return 99 ;;
        esac
        printf '%s=%s\n' "${property#--property=}" "$value"
      done ;;
    list-units|daemon-reload) : ;;
    *) return 99 ;;
  esac
}
# Route manager queries to the shell model; retain real Python/flock bounds.
timeout() {
  local -a original=("$@")
  while [[ "$1" == --* ]]; do shift; done
  shift
  if [[ "$1" == systemctl ]]; then "$@"; else command timeout "${original[@]}"; fi
}
product_control_instances_are_quiescent() { return 0; }
product_brokers_are_quiescent() { return 0; }
product_restore_instances_are_quiescent() { return 0; }
do_secure_executables() { echo secure >>"$fixture/events"; }
do_udev_reload() { :; }
do_restore_kwin_capability() { :; }
do_install_vibeshine_drm() { :; }
do_install_vibeshine_ds5() { :; }
do_install_prelogin() { echo configure >>"$fixture/events"; product_package_recover_failed_transaction; }
do_print_next_steps() { :; }
'''.replace('product', BRAND)
        (archive / '.INSTALL').write_text(code)
        modeled = base / 'candidate.pkg.tar'
        subprocess.run(['bsdtar', '-cf', str(modeled), '-C', str(archive), '.INSTALL', 'usr'], check=True)
        # The outgoing predicate is always checked against verbatim installed bytes.
        environment = dict(PATH=str(COMMANDS) + ':' + os.environ['PATH'], identity=installed, fixture=str(base), old_device=str(identity.st_dev), identity_case='same-device' if case == 'same-device' else 'unrelated-fuse', manager_state='idle', **{BRAND.upper() + '_PACKAGE_RUN_ID': '1' * 32})
        result = run(f'''
# Keep the staged library for the separately modeled manager/post callbacks.
trap - EXIT
workdir={shlex.quote(str(base / 'workdir'))}; mkdir -p "$workdir"
pacman() {{ printf '%s\\n' "$identity"; }}
stage_upgrade_guard {shlex.quote(str(modeled))} {shlex.quote(str(outgoing))}
[[ -f "$workdir/hooks/00-{BRAND}-quiesce.hook" ]]
"$workdir/preflight"
''', **environment)
        if case == 'same-device':
            assert result.returncode != 0, result.stdout + result.stderr
            assert not (base / 'events').exists()
            continue
        assert result.returncode == 0, (case, result.stdout, result.stderr)
        replacement = base / 'replacement'
        if case == 'controller-busy':
            environment['manager_state'] = 'busy'
            result = subprocess.run(['bash', '-c', 'source "$1"; ' + BRAND + '_package_recover_failed_transaction', 'fixture', str(base / 'workdir/arch-package-hooks')], env=dict(os.environ, **environment), capture_output=True, text=True)
            assert result.returncode != 0 and not replacement.exists()
            environment['manager_state'] = 'idle'
        if case == 'failed-transaction':
            # The manager child has returned without replacing files. Recovery
            # verifies the generation and leaves the administrator mask intact.
            operation = BRAND + '_package_recover_failed_transaction'
        else:
            replacement.write_bytes(b'new package payload')
            operation = 'post_upgrade'
        result = subprocess.run(['bash', '-c', 'source "$1"; ' + operation, 'fixture', str(base / 'workdir/arch-package-hooks')], env=dict(os.environ, **environment), capture_output=True, text=True, timeout=15)
        assert result.returncode == 0 and 'not found' not in result.stderr, (case, result.stdout, result.stderr)
        if case != 'failed-transaction':
            assert (base / 'events').read_text().splitlines() == ['secure', 'configure']
        else:
            assert not replacement.exists() and not (base / 'events').exists()
        assert sentinel.read_bytes() == b'pairing and administrator policy\n'
        assert admin_mask.is_symlink() and admin_mask.lstat().st_ino == original_mask
        assert not (base / 'runtime-package-transaction').exists(), (case, result.stderr)
        assert outgoing.read_bytes() == old
        if case == 'failed-transaction':
            environment[BRAND.upper() + '_PACKAGE_RUN_ID'] = '2' * 32
            result = subprocess.run([str(base / 'workdir/preflight')], env=dict(os.environ, **environment), capture_output=True, text=True, timeout=15)
            assert result.returncode == 0, result.stderr
            replacement.write_bytes(b'retry package payload')
            result = subprocess.run(['bash', '-c', 'source "$1"; post_upgrade', 'fixture', str(base / 'workdir/arch-package-hooks')], env=dict(os.environ, **environment), capture_output=True, text=True, timeout=15)
            assert result.returncode == 0, result.stderr
            assert (base / 'events').read_text().splitlines() == ['secure', 'configure']
            assert not (base / 'runtime-package-transaction').exists()
            assert sentinel.read_bytes() == b'pairing and administrator policy\n'
            assert admin_mask.lstat().st_ino == original_mask
print(f'{BRAND}: exact-daily bootstrap scope, refusal/idempotency/admin-policy cases and complete incoming proof/finalization/recovery passed; no native transaction')
