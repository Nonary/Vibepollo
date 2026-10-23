#!/usr/bin/env python3
"""Pinned executable identity faults; no mounts, signals or live manager calls.

FUSE getattr denial and mount metadata are modeled. O_PATH pinning, fdinfo,
process executables and descriptor lifetime use the real kernel.
"""
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[4]
BRAND = sys.argv[2] if len(sys.argv) > 2 else 'vibepollo'
paths = (f'/usr/bin/{BRAND}', f'/usr/libexec/vibeshine/{BRAND}-host')
programs = []
for source in (ROOT / 'packaging/linux').rglob('*'):
    if source.suffix not in ('.install', '.in', '.spec') or not source.is_file():
        continue
    text = source.read_text().replace('%%', '%')
    for function in re.findall(r'^' + BRAND + r'_package_unmanaged_host_absent\(\) \{\n.*?^\}', text, re.M | re.S):
        programs.append((str(source.relative_to(ROOT)), function.split("<<'PY'\n", 1)[1].split('\nPY\n', 1)[0]))
assert len(programs) == 9, len(programs)
assert len({code for _, code in programs}) == 1, 'package proof copies drifted'

fault_spec = importlib.util.spec_from_file_location('fuse_fault', ROOT / 'tests/unit/platform/linux/fixtures/package_fuse_fault.py')
fuse_fault = importlib.util.module_from_spec(fault_spec)
fault_spec.loader.exec_module(fuse_fault)
PRELUDE = fuse_fault.prelude(paths[0])

count = 0
with tempfile.TemporaryDirectory(prefix=BRAND + '-fuse-identity-') as tmp:
    proc = Path(tmp) / 'proc'
    proc.mkdir()
    entry = proc / '100'
    entry.symlink_to('/proc/' + str(os.getpid()))
    identity = Path('/proc/self/exe').stat()
    snapshot = json.dumps([dict(path=paths[0], device=identity.st_dev, inode=identity.st_ino)])
    for label, program in programs:
        for case in ('normal', 'unrelated-fuse', 'same-device', 'non-fuse', 'open-denied',
                     'link-denied', 'io-error', 'missing-mount-id', 'duplicate-mount-id',
                     'invisible-mount', 'duplicate-mount', 'exact-path', 'deleted-path', 'pid-reuse'):
            if not entry.is_symlink():
                (entry / 'exe').unlink()
                entry.rmdir()
            else:
                entry.unlink()
            entry.symlink_to('/proc/' + str(os.getpid()))
            code = PRELUDE + '\n' + program.replace('pathlib.Path("/proc")', 'pathlib.Path(' + repr(str(proc)) + ')')
            result = subprocess.run([sys.executable, '-I', '-', snapshot, case, str(proc), str(identity.st_dev)], input=code, capture_output=True, text=True, timeout=5)
            assert (result.returncode == 0) == (case == 'unrelated-fuse'), (label, case, result.returncode, result.stderr)
            assert 'Traceback' not in result.stderr, (label, case, result.stderr)
            if case == 'pid-reuse':
                assert 'PINNED_BEFORE_PID_REUSE' in result.stderr, result.stderr
            count += 1
        if not entry.is_symlink():
            (entry / 'exe').unlink()
            entry.rmdir()
        else:
            entry.unlink()
        entry.symlink_to('/proc/' + str(os.getpid()))
        # Excluding one proven unrelated mount must not finish the whole scan.
        (proc / '200').symlink_to('/proc/' + str(os.getpid()))
        result = subprocess.run([sys.executable, '-I', '-', snapshot, 'unrelated-fuse', str(proc), str(identity.st_dev)], input=code, capture_output=True, text=True, timeout=5)
        assert result.returncode != 0 and 'PID 200' in result.stderr, (label, result.stderr)
        (proc / '200').unlink()
        count += 1
        # Empty capture still cannot bless an exact deleted package executable.
        for case, expected in (('unrelated-fuse', 0), ('deleted-path', 1)):
            result = subprocess.run([sys.executable, '-I', '-', '[]', case, str(proc), str(identity.st_dev)], input=code, capture_output=True, text=True, timeout=5)
            assert (result.returncode == 0) == (expected == 0), (label, case, result.stderr)
            count += 1
print(f'{BRAND}: {count} pinned FUSE identity cases across {len(programs)} package proofs passed; EACCES alone never proves absence')

# Exercise the real record/mask/recovery pipeline with the same syscall faults.
# The manager is a shell model and all owned paths are under a private root.
spec = importlib.util.spec_from_file_location('recovery', ROOT / 'tests/unit/platform/linux/test_package_transaction_recovery.py')
recovery = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recovery)
for hook in recovery.HOOKS:
    with tempfile.TemporaryDirectory(prefix=BRAND + '-fuse-retry-') as tmp:
        stage = Path(tmp)
        for name in ('systemd', 'runtime', 'proc'):
            (stage / name).mkdir()
        (stage / 'proc/100').symlink_to('/proc/' + str(os.getpid()))
        sentinel = stage / 'administrator-configuration'
        sentinel.write_bytes(b'pairing and administrator policy must survive\n')
        admin = stage / 'systemd/vibeshine-vkms-control.socket'
        admin.symlink_to('/dev/null')
        original_admin = admin.lstat().st_ino
        source = (ROOT / hook).read_text().replace('%%', '%')
        name = BRAND + '_package_unmanaged_host_absent'
        proof = re.search(r'^' + name + r'\(\) \{\n.*?^\}', source, re.M | re.S).group()
        proof = proof.replace('pathlib.Path("/proc")', 'pathlib.Path(' + repr(str(stage / 'proc')) + ')')
        proof = proof.replace('"$' + BRAND + '_package_executable_records" <<\'PY\'', '"$' + BRAND + '_package_executable_records" "$identity_case" "$stage/proc" "$old_device" <<\'PY\'')
        proof = proof.replace("<<'PY'\n", "<<'PY'\n" + PRELUDE + '\n')
        code = recovery.PackageRecovery().record_fixture_code(hook, stage) + '\n' + proof
        # Retain the actual captured inode/device; only immutable-root metadata
        # validation is redirected by the existing unprivileged fixture helper.
        code += '\n' + BRAND + '_package_capture_host_executables() { printf \'%s\\n\' ' + shlex.quote(snapshot) + '; }\n'
        shell = r'''
product_broker_socket="$stage/absent-broker"
product_control_socket="$stage/absent-control"
product_legacy_handoff_directory="$stage/absent-handoffs"
product_legacy_restore_directory="$stage/absent-restores"
product_legacy_transition_lock="$stage/absent-lock"
product_legacy_prelogin_marker="$stage/absent-marker"
product_control_instances_are_quiescent() { return 0; }
product_brokers_are_quiescent() { return 0; }
product_restore_instances_are_quiescent() { return 0; }
product_unit_is_quiescent() {
  [ "$1:$manager_state" != product-session-controller.service:busy ]
}
identity_case=same-device
manager_state=idle
product_package_begin || exit 81
printf '[Unit]\nRefuseManualStart=yes\n' >"$stage/fence"
chmod 0644 "$stage/fence"
product_package_record_owned fence "$stage/fence" || exit 82
product_package_mask product.service product-session-exec.socket CONTROL_SOCKET 'product-session-restore@.service' || exit 83
product_package_mark_late || exit 84
if product_package_recover_failed_transaction; then exit 85; fi
[ -f "$stage/fence" ] && [ -d "$stage/transaction" ] && [ -L "$stage/systemd/product.service" ] || exit 86
# A corrected executable proof alone cannot release a busy controller fence.
identity_case=unrelated-fuse
manager_state=busy
if product_package_recover_failed_transaction; then exit 87; fi
[ -f "$stage/fence" ] && [ -d "$stage/transaction" ] || exit 88
manager_state=idle
product_package_begin || exit 89
product_package_recover_failed_transaction || exit 90
[ ! -e "$stage/fence" ] && [ ! -e "$stage/transaction" ] || exit 91
[ ! -L "$stage/systemd/product.service" ] && [ ! -L "$stage/systemd/product-session-exec.socket" ] || exit 92
'''.replace('CONTROL_SOCKET', 'vibeshine-vkms-control.socket').replace('product', BRAND)
        result = subprocess.run(['bash', '-c', code + shell], env=dict(os.environ, stage=tmp, old_device=str(identity.st_dev)), capture_output=True, text=True, timeout=15)
        assert result.returncode == 0, (hook, result.returncode, result.stderr)
        assert sentinel.read_bytes() == b'pairing and administrator policy must survive\n'
        assert admin.is_symlink() and admin.lstat().st_ino == original_admin
print(f'{BRAND}: failed identity proof and busy controller retain fences; corrected package retry releases only owned fences across Arch/DEB/RPM')

# Actual Arch callbacks must never finalize capabilities or configure profiles
# after failed quiescence. Successful retries finalize before configuration.
arch = (ROOT / f'packaging/linux/Arch/{BRAND}.install').read_text()
callbacks = '\n'.join(re.search(r'^' + name + r'\(\) \{\n.*?^\}', arch, re.M | re.S).group() for name in ('post_install', 'post_upgrade'))
for name in ('post_install', 'post_upgrade'):
    with tempfile.TemporaryDirectory(prefix=BRAND + '-finalization-order-') as tmp:
        code = callbacks + '\n' + r'''
product_quiesce_or_abort() { echo quiesce >>"$events"; [ "$proof" = valid ]; }
do_secure_executables() { echo secure >>"$events"; }
do_udev_reload() { :; }
do_restore_kwin_capability() { :; }
do_install_vibeshine_drm() { :; }
do_install_vibeshine_ds5() { :; }
do_install_prelogin() { echo configure >>"$events"; }
do_print_next_steps() { :; }
proof=uncertain
if CALLBACK; then exit 93; fi
[ "$(cat "$events")" = quiesce ] || exit 94
proof=valid
CALLBACK || exit 95
'''.replace('product', BRAND).replace('CALLBACK', name)
        events = Path(tmp) / 'events'
        result = subprocess.run(['bash', '-c', code], env=dict(os.environ, events=str(events)), capture_output=True, text=True, timeout=5)
        assert result.returncode == 0, (name, result.returncode, result.stderr)
        assert 'not found' not in result.stderr, result.stderr
        assert events.read_text().splitlines() == ['quiesce', 'quiesce', 'secure', 'configure']
print(f'{BRAND}: post-install/upgrade finalize only after proof and before profile configuration')
