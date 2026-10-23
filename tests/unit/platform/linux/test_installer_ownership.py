#!/usr/bin/env python3
"""Installer run ownership and failure classification with real hook validators.

No services/packages run. The callback is a private-file marker; full resource
quiescence/recovery is tested by test_package_transaction_recovery.py.
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
source = (root / f'packaging/linux/Arch/{brand}.install').read_text()
definitions = []
for suffix in ('package_record_is_safe', 'package_run_id_is_safe', 'package_run_is_owned', 'package_force_is_applicable'):
    opener, closer = ('(', ')') if suffix == 'package_force_is_applicable' else ('{', '}')
    match = re.search(r'^' + brand + '_' + suffix + r'\(\) ' + re.escape(opener) + r'\n.*?^' + re.escape(closer) + r'$', source, re.M | re.S)
    assert match, suffix
    definitions.append(match.group())
validators = '\n'.join(definitions).replace('0:0:', f'{os.getuid()}:{os.getgid()}:')

for scenario in ('database-locked', 'foreign-run', 'own-generic-error', 'own-host-decline', 'own-host-retry'):
    with tempfile.TemporaryDirectory(prefix=f'{brand}-run-owner-') as temporary:
        fixture = pathlib.Path(temporary)
        stage = fixture / 'stage'
        stage.mkdir()
        record = stage / 'transaction'
        record.mkdir(mode=0o700)
        for name, value in {'run-id': 'f' * 32, 'phase': 'late', 'failure-class': 'host-stop-deadline'}.items():
            path = record / name
            path.write_text(value + '\n')
            path.chmod(0o600)
        barrier = stage / 'barrier'
        barrier.write_text('owned by existing operation\n')
        original_inode = barrier.stat().st_ino
        library = stage / 'arch-package-hooks'
        library.write_text(validators + f'''
{brand}_package_transaction="$workdir/transaction"
{brand}_package_lock() {{ return 0; }}
{brand}_package_recover_failed_transaction() {{
  {brand}_package_run_is_owned || return 1
  printf 'recover\\n' >> "$workdir/recovery"
  rm -- "$workdir/barrier"
}}
''')
        db_lock = fixture / 'db.lck'
        if scenario == 'database-locked':
            db_lock.touch()
        body = f'''
source {shlex.quote(str(root / 'scripts/linux_install.sh'))}
workdir={shlex.quote(str(stage))}
package_database_lock={shlex.quote(str(db_lock))}
scenario={shlex.quote(scenario)}
can_offer_package_force() {{ return 0; }}
read() {{ case "$scenario" in own-host-retry) answer=y ;; own-host-decline) answer=n ;; *) die 'generic failures must not offer force' ;; esac; }}
pacman() {{
  printf '%s:%s\\n' "${{{brand.upper()}_PACKAGE_FORCE:-0}}" "${{{brand.upper()}_PACKAGE_RUN_ID:-}}" >> "$workdir/calls"
  case "$scenario" in
    database-locked|foreign-run) return 1 ;;
  esac
  printf '%s\\n' "${{{brand.upper()}_PACKAGE_RUN_ID}}" > "$workdir/transaction/run-id"
  case "$scenario" in own-generic-error) echo conflict > "$workdir/transaction/failure-class" ;; esac
  if [ "${{{brand.upper()}_PACKAGE_FORCE:-0}}" = 1 ]; then rm "$workdir/barrier"; return 0; fi
  return 1
}}
if run_package_transaction pacman -U fixture; then result=0; else result=$?; fi
cp "$workdir/calls" {shlex.quote(str(fixture / 'calls'))}
if [ -e "$workdir/barrier" ]; then
  stat -c '%i' "$workdir/barrier" > {shlex.quote(str(fixture / 'barrier-inode'))}
fi
if [ -e "$workdir/recovery" ]; then cp "$workdir/recovery" {shlex.quote(str(fixture / 'recovery'))}; fi
cp "$workdir/transaction/run-id" {shlex.quote(str(fixture / 'retained-run-id'))}
case "$scenario" in own-host-retry) [ "$result" = 0 ] ;; *) [ "$result" = 1 ] ;; esac
'''
        completed = subprocess.run(['bash', '-c', body], capture_output=True, text=True)
        assert completed.returncode == 0, (scenario, completed.stdout, completed.stderr)
        calls = (fixture / 'calls').read_text().splitlines()
        run_ids = [line.split(':', 1)[1] for line in calls]
        assert all(re.fullmatch('[0-9a-f]{32}', value) for value in run_ids)
        assert len(set(run_ids)) == 1, 'force retry must retain its originating run ID'
        assert len(calls) == (2 if scenario == 'own-host-retry' else 1)
        if scenario in ('database-locked', 'foreign-run'):
            assert (fixture / 'barrier-inode').read_text().strip() == str(original_inode)
            assert (fixture / 'retained-run-id').read_text().strip() == 'f' * 32
            assert not (fixture / 'recovery').exists()
        elif scenario == 'own-host-retry':
            assert not (fixture / 'recovery').exists(), 'accepted retry must not recover between attempts'
        else:
            assert (fixture / 'recovery').read_text() == 'recover\n'
print(f'{brand}: actual ownership/class validators preserve locked and foreign runs; only own host deadline offers one force retry')
