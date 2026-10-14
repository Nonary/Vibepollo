#!/usr/bin/env python3
"""Stage and execute the complete incoming Arch guard on a private first-install model.

All runtime/helper paths and proc enumeration are redirected into the fixture.
Only root UID/GID metadata expectations are mapped to the fixture owner. No
production function is replaced; command discovery selects the no-systemd path.
"""
import os
import pathlib
import shlex
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[4]
brand = sys.argv[2] if len(sys.argv) > 2 else 'vibepollo'
source = (root / f'packaging/linux/Arch/{brand}.install').read_text()
with tempfile.TemporaryDirectory(prefix=f'{brand}-real-incoming-guard-') as temporary:
    fixture = pathlib.Path(temporary)
    archive = fixture / 'archive'
    archive.mkdir()
    proc = fixture / 'private-proc'
    proc.mkdir()
    guard = source.replace('0:0:', f'{os.getuid()}:{os.getgid()}:')
    guard = guard.replace(f'/run/{brand}', str(fixture / 'absent-runtime'))
    guard = guard.replace('/run/systemd/system', str(fixture / 'absent-systemd'))
    guard = guard.replace(f'/usr/libexec/vibeshine/{brand}', str(fixture / 'absent-helper'))
    guard = guard.replace(f'/usr/bin/{brand}', str(fixture / 'absent-public-host'))
    guard = guard.replace('pathlib.Path("/proc")', f'pathlib.Path({str(proc)!r})')
    guard = guard.replace('/proc/[0-9]*/exe', str(proc) + '/[0-9]*/exe')
    guard += '\ncommand() { if [[ "$1" = -v && "$2" = systemctl ]]; then return 1; else builtin command "$@"; fi; }\n'
    (archive / '.INSTALL').write_text(guard)
    migration = archive / f'usr/share/{brand}/installer-migrations/upgrade-2.0-lifecycle.sh'
    migration.parent.mkdir(parents=True)
    migration.write_text((root / 'scripts/linux/migrations/upgrade-2.0-lifecycle.sh').read_text())
    candidate = fixture / 'incoming.pkg.tar'
    subprocess.run(['bsdtar', '-cf', str(candidate), '-C', str(archive), '.INSTALL', 'usr'], check=True)
    outgoing = fixture / 'historical-hooks'
    outgoing.write_text((root / 'tests/unit/platform/linux/fixtures/package-2.0.0/arch-selector.sh').read_text().replace('vibepollo', brand))
    record = fixture / 'absent-runtime-package-transaction'
    body = f'''
source {shlex.quote(str(root / 'scripts/linux_install.sh'))}
workdir=$(mktemp -d {shlex.quote(str(fixture))}/installer.XXXXXX)
pacman() {{ printf '%s\\n' '{brand} 1:2.0.0-1'; }}
stage_upgrade_guard {shlex.quote(str(candidate))} {shlex.quote(str(outgoing))}
[[ -f "$workdir/hooks/00-{brand}-quiesce.hook" ]]
export {brand.upper()}_PACKAGE_RUN_ID=11111111111111111111111111111111
"$workdir/preflight"
[[ $(cat {shlex.quote(str(record / 'phase'))}) == preflight ]]
[[ $(cat {shlex.quote(str(record / 'run-id'))}) == 11111111111111111111111111111111 ]]
# The complete incoming guard is idempotent, including the modern installed
# hook's second invocation. An unknown old version retains its distinct hook.
"$workdir/preflight"
pacman() {{ printf '%s\\n' '{brand} 2.1.0-1'; }}
stage_upgrade_guard {shlex.quote(str(candidate))} {shlex.quote(str(outgoing))}
[[ ! -e "$workdir/hooks/00-{brand}-quiesce.hook" && -f "$workdir/hooks/00-{brand}-incoming-quiesce.hook" ]]
"$workdir/preflight"
'''
    completed = subprocess.run(['bash', '-c', body], capture_output=True, text=True)
    assert completed.returncode == 0, completed.stdout + completed.stderr
print(f'{brand}: actual complete staged guard passes private no-service preflight, idempotent repeat, epoch migration, and conditional override retirement')
