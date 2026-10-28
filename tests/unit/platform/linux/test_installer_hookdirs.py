#!/usr/bin/env python3
"""Preserve administrator hooks through the actual installer -S/-U/-R paths.

The merge model follows official pacman v7.0.0 commit
138cbae58448b7fde90335526add3029875784ee: CLI hookdirs append first
(pacman.c:465), configured HookDir values append later (conf.c:653), the
/etc default appears only for an empty list (conf.c:1367), the system hookdir
is retained (conf.c:911), and ALPM reads directories in reverse order and
keeps the first occurrence of each hook name (libalpm/hook.c:540).

No package manager transaction or guard executes. Actual installer functions
stage a private archive; harmless pacman/pacman-conf fixtures supply queries,
and a transaction capture records the exact argv at their transaction edge.
When available, actual pacman-conf parses only private configuration files to
independently check effective default/Include/custom directory values.
"""
import os
import pathlib
import shlex
import shutil
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[4]
brand = sys.argv[2] if len(sys.argv) > 2 else 'vibepollo'
script = root / 'scripts/linux_install.sh'


def selected_hooks(directories, entries):
    selected = {}
    for directory in reversed(directories):
        for name, payload in entries.get(directory.rstrip('/'), {}).items():
            selected.setdefault(name, payload)
    return selected


def official_merge(cli, configured, default):
    user = [*cli, *configured]
    return ['/usr/share/libalpm/hooks', *(user or [default])]


count = 0
with tempfile.TemporaryDirectory(prefix=f'{brand}-hookdir-regression-') as temporary:
    fixture = pathlib.Path(temporary)
    archive = fixture / 'archive'
    archive.mkdir()
    # A real candidate archive is staged by the production staging function.
    # Reaching this guard would be a test error, including on query failures.
    (archive / '.INSTALL').write_text(f'{brand}_quiesce_or_abort() {{ return 99; }}\n')
    candidate = fixture / f'{brand}-3.0.0-1.pkg.tar.zst'
    subprocess.run(['bsdtar', '-cf', str(candidate), '-C', str(archive), '.INSTALL'], check=True)
    custom = [str(fixture / 'hooks-one'), str(fixture / 'hooks-$literal;two')]
    default = '/etc/pacman.d/hooks/'
    configurations = {'default': ([], [default]), 'custom': (custom, custom)}
    relative_value = str(fixture / 'relative-admin').lstrip('/')

    # pacman-conf does not call setup_libalpm (hence no database lock or
    # package writes), as confirmed in official src/pacman/pacman-conf.c.
    query = shutil.which('pacman-conf')
    if query:
        plain = fixture / 'default.conf'
        plain.write_text('[options]\n')
        actual = subprocess.run([query, '--config', str(plain), 'HookDir'], check=True, capture_output=True, text=True).stdout.splitlines()
        assert actual, 'native default query must not be empty'
        default = actual[0]
        configurations['default'] = ([], actual)
        include = fixture / 'included.conf'
        include.write_text(f'HookDir = {custom[1]}\n')
        configured = fixture / 'custom.conf'
        configured.write_text(f'[options]\nHookDir = {custom[0]}\nInclude = {include}\n')
        actual = subprocess.run([query, '--config', str(configured), 'HookDir'], check=True, capture_output=True, text=True).stdout.splitlines()
        assert actual == custom, actual
        print(f'{brand}: native pacman-conf private default/Include queries match source merge model')
        relative_config = fixture / 'relative.conf'
        relative_config.write_text(f'[options]\nHookDir = {relative_value}\n')
        actual = subprocess.run([query, '--config', str(relative_config), 'HookDir'], check=True, capture_output=True, text=True).stdout.splitlines()
        assert actual == [relative_value], actual
        print(f'{brand}: native pacman-conf preserves relative HookDir; installer must refuse it')

    for case, (configured, effective) in configurations.items():
        for operation in ('-S', '-U', '-R'):
            current = fixture / (case + operation)
            current.mkdir()
            argv_path = current / 'argv'
            private_path = current / 'private'
            preparation = current / 'driver-prep'
            query_file = current / 'query-values'
            query_file.write_text('\n'.join(effective) + '\n')
            body = f'''
source {shlex.quote(str(script))}
workdir={shlex.quote(str(current / 'workdir'))}
mkdir -p "$workdir"
local_package={shlex.quote(str(candidate))}
configure_pacman_repo() {{ :; }}
prepare_driver_replacement() {{ printf 'prepared\\n' > {shlex.quote(str(preparation))}; }}
pacman-conf() {{ [[ "$*" == HookDir ]] || return 89; cat {shlex.quote(str(query_file))}; }}
pacman() {{
  case "$1" in
    -Si) return 0 ;;
    -Sw) cp -- {shlex.quote(str(candidate))} "$workdir/cache/{candidate.name}" ;;
    -Qp) printf '%s\\n' '{brand} 3.0.0-1' ;;
    *) return 90 ;;
  esac
}}
run_package_transaction() {{
  printf '%s\\0' "$@" > {shlex.quote(str(argv_path))}
  [[ -f "$workdir/hooks/00-{brand}-incoming-quiesce.hook" ]] || return 91
  printf '%s\\n' "$workdir/hooks" > {shlex.quote(str(private_path))}
}}
{'install_from_repo' if operation == '-S' else 'install_from_package' if operation == '-U' else 'remove_' + brand}
'''
            completed = subprocess.run(['bash', '-c', body], capture_output=True, text=True)
            assert completed.returncode == 0, (case, operation, completed.stdout, completed.stderr)
            argv = argv_path.read_bytes().rstrip(b'\0').decode().split('\0')
            assert argv[:2] == ['pacman', operation], argv
            cli = [argv[index + 1] for index, value in enumerate(argv) if value == '--hookdir']
            private = private_path.read_text().strip()
            assert cli == [*effective, private], (case, operation, cli)
            merged = official_merge(cli, configured, default)
            incoming_name = f'00-{brand}-incoming-quiesce.hook'
            entries = {
                '/usr/share/libalpm/hooks': {'10-system.hook': 'system', '90-administrator.hook': 'system-shadow'},
                private: {incoming_name: 'incoming'},
            }
            for index, directory in enumerate(effective):
                entries[directory.rstrip('/')] = {'90-administrator.hook': f'admin-{index}', f'80-admin-{index}.hook': 'sentinel'}
            if configured:
                # Demonstrate native same-name collision precedence. Actual
                # collisions below must refuse before a transaction edge.
                entries[configured[-1]][incoming_name] = 'admin-override'
            selected = selected_hooks(merged, entries)
            assert selected['10-system.hook'] == 'system'
            assert selected['90-administrator.hook'] == f'admin-{len(effective) - 1}'
            assert all(selected[f'80-admin-{index}.hook'] == 'sentinel' for index in range(len(effective)))
            assert selected[incoming_name] == ('admin-override' if configured else 'incoming')
            # Reproduce the old sole-private-flag defect with the same model.
            if not configured:
                broken = selected_hooks(official_merge([private], [], default), entries)
                assert broken['90-administrator.hook'] == 'system-shadow'
                assert '80-admin-0.hook' not in broken
            assert preparation.exists() == (operation != '-R')
            count += 1

    # Query failures must never reach any transaction edge or driver-prep.
    for mode in ('missing', 'error', 'empty', 'cr', 'blank-line', 'over-limit', 'relative'):
        for operation in ('-S', '-U', '-R'):
            current = fixture / (mode + operation)
            current.mkdir()
            called = current / 'called'
            preparation = current / 'driver-prep'
            values = {'relative': relative_value + '\n', 'empty': '', 'cr': '/etc/pacman.d/hooks/\r\n', 'blank-line': '/etc/pacman.d/hooks/\n\n/tmp/hooks\n',
                      'over-limit': ''.join(f'/tmp/hooks-{index}\n' for index in range(257))}.get(mode, '')
            query_file = current / 'query-values'
            query_file.write_text(values)
            query_mock = 'return 1' if mode == 'error' else 'cat ' + shlex.quote(str(query_file))
            missing_mock = 'command() { if [[ "$1" == -v && "$2" == pacman-conf ]]; then return 1; fi; builtin command "$@"; }' if mode == 'missing' else ''
            body = f'''
source {shlex.quote(str(script))}
workdir={shlex.quote(str(current / 'workdir'))}
mkdir -p "$workdir"
local_package={shlex.quote(str(candidate))}
configure_pacman_repo() {{ :; }}
prepare_driver_replacement() {{ printf 'prepared\\n' > {shlex.quote(str(preparation))}; }}
pacman-conf() {{ {query_mock}; }}
{missing_mock}
pacman() {{ case "$1" in -Si) return 0 ;; -Sw) cp -- {shlex.quote(str(candidate))} "$workdir/cache/{candidate.name}" ;; -Qp) printf '%s\\n' '{brand} 3.0.0-1' ;; *) return 90 ;; esac; }}
run_package_transaction() {{ printf 'unexpected transaction\\n' > {shlex.quote(str(called))}; }}
{'install_from_repo' if operation == '-S' else 'install_from_package' if operation == '-U' else 'remove_' + brand}
'''
            completed = subprocess.run(['bash', '-c', body], capture_output=True, text=True)
            assert completed.returncode != 0, (mode, operation, completed.stdout, completed.stderr)
            assert not called.exists(), (mode, operation)
            assert not preparation.exists(), (mode, operation)
            count += 1

    # The helper keeps opaque path values in single argv entries, without
    # shell evaluation, whitespace splitting, or wildcard expansion.
    quoted = fixture / 'quoted-values'
    dangerous = [str(fixture / 'hooks with spaces'), str(fixture / 'hooks-$(touch UNEXPECTED)-*')]
    quoted.write_text('\n'.join(dangerous) + '\n')
    argv_path = fixture / 'quoted-argv'
    private_hooks = fixture / 'private hooks/hooks'
    private_hooks.mkdir(parents=True)
    (private_hooks / f'00-{brand}-incoming-quiesce.hook').write_text('fixture guard\n')
    body = f'''
source {shlex.quote(str(script))}
workdir={shlex.quote(str(fixture / 'private hooks'))}
pacman-conf() {{ cat {shlex.quote(str(quoted))}; }}
prepare_package_hook_dirs
printf '%s\\0' "${{package_hook_args[@]}}" > {shlex.quote(str(argv_path))}
'''
    completed = subprocess.run(['bash', '-c', body], cwd=fixture, capture_output=True, text=True)
    assert completed.returncode == 0, completed.stdout + completed.stderr
    argv = argv_path.read_bytes().rstrip(b'\0').decode().split('\0')
    assert argv == ['--hookdir', dangerous[0], '--hookdir', dangerous[1], '--hookdir', str(fixture / 'private hooks/hooks')], argv
    assert not (fixture / 'UNEXPECTED').exists()
    count += 1

    # Actual same-name administrator files and disabling/broken symlinks must
    # neither override the required guard nor be shadowed by the CLI guard.
    for policy in ('default', 'configured'):
        for qualified in (False, True):
            guard_name = f'00-{brand}-' + ('quiesce.hook' if qualified else 'incoming-quiesce.hook')
            for kind in ('regular', 'disabled-link', 'broken-link'):
                for operation in ('-S', '-U', '-R'):
                    current = fixture / f'collision-{policy}-{qualified}-{kind}-{operation}'
                    current.mkdir()
                    admin = current / 'administrator hooks'
                    admin.mkdir()
                    collision = admin / guard_name
                    if kind == 'regular':
                        collision.write_text('administrator policy\n')
                    else:
                        collision.symlink_to('/dev/null' if kind == 'disabled-link' else '/missing-hook-target')
                    before = collision.read_bytes() if kind == 'regular' else os.readlink(collision)
                    collision_query = [str(admin)] if policy == 'default' else [str(current / 'earlier nonconflicting hooks'), str(admin)]
                    called = current / 'called'
                    preparation = current / 'driver-prep'
                    body = f"""
source {shlex.quote(str(script))}
workdir={shlex.quote(str(current / 'workdir'))}
mkdir -p "$workdir"
local_package={shlex.quote(str(candidate))}
configure_pacman_repo() {{ :; }}
prepare_driver_replacement() {{ printf 'unexpected preparation\\n' > {shlex.quote(str(preparation))}; }}
pacman-conf() {{ printf '%s\\n' {' '.join(shlex.quote(value) for value in collision_query)}; }}
pacman() {{ case "$1" in -Si) return 0 ;; -Sw) cp -- {shlex.quote(str(candidate))} "$workdir/cache/{candidate.name}" ;; -Qp) printf '%s\\n' '{brand} 3.0.0-1' ;; *) return 90 ;; esac; }}
eval "$(declare -f stage_upgrade_guard | sed '1s/stage_upgrade_guard/stage_guard_fixture/')"
stage_upgrade_guard() {{
  stage_guard_fixture "$@" || return
  {'mv -- "$workdir/hooks/00-' + brand + '-incoming-quiesce.hook" "$workdir/hooks/' + guard_name + '"' if qualified else ':'}
}}
run_package_transaction() {{ printf 'unexpected transaction\\n' > {shlex.quote(str(called))}; }}
{'install_from_repo' if operation == '-S' else 'install_from_package' if operation == '-U' else 'remove_' + brand}
"""
                    completed = subprocess.run(['bash', '-c', body], capture_output=True, text=True)
                    assert completed.returncode != 0, (policy, qualified, kind, operation)
                    assert 'conflicts with the required shutdown guard' in completed.stderr, completed.stderr
                    assert not called.exists() and not preparation.exists(), (policy, kind, operation)
                    after = collision.read_bytes() if kind == 'regular' else os.readlink(collision)
                    assert after == before, (policy, kind, 'administrator policy changed')
                    count += 1

    for mode in ('missing', 'ambiguous', 'symlink'):
        current = fixture / ('private-guard-' + mode)
        guard_dir = current / 'hooks'
        guard_dir.mkdir(parents=True)
        if mode == 'ambiguous':
            (guard_dir / 'one.hook').write_text('one\n')
            (guard_dir / 'two.hook').write_text('two\n')
        elif mode == 'symlink':
            (guard_dir / 'one.hook').symlink_to('/dev/null')
        body = f"""
source {shlex.quote(str(script))}
workdir={shlex.quote(str(current))}
pacman-conf() {{ printf '%s\\n' {shlex.quote(str(fixture / 'admin-without-collision'))}; }}
prepare_package_hook_dirs
"""
        completed = subprocess.run(['bash', '-c', body], capture_output=True, text=True)
        assert completed.returncode != 0 and 'private package transaction guard' in completed.stderr, (mode, completed.stderr)
        count += 1
print(f'{brand}: {count} hook-directory cases PASS; actual installer entrypoints retain administrator/system hooks and config precedence; conflicting policy and unsafe query/guard results refuse before transaction/driver preparation')
