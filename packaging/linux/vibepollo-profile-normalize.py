#!/usr/bin/env python3
"""Adapt a private, confined legacy copy; never read a live source profile.

The machine-host caller validates and seals staging before invoking this file.
External credential/state paths fail closed instead of sharing the old host's
mutable files or having root read paths from user configuration.
"""
import json
import importlib.util
import os
import pathlib
import re
import sys


def normalize(staging, desktop_home, selection, destination='/var/lib/vibepollo'):
    staging = pathlib.Path(staging)
    candidates = [host for host in ('vibepollo', 'vibeshine', 'sunshine')
                  if (staging / (host + '.conf')).is_file()]
    if not candidates:
        if any(staging.iterdir()):
            raise ValueError('legacy profile has data but no recognized host configuration')
        return
    expected = 'vibeshine' if selection == 'machine-vibeshine' else selection
    if expected == 'auto':
        if len(candidates) != 1:
            raise ValueError('multiple host configurations; choose a migration source')
        expected = candidates[0]
    if expected not in candidates:
        raise ValueError('selected legacy host configuration is missing')
    old = '/var/lib/vibeshine' if selection == 'machine-vibeshine' else f'{desktop_home}/.config/{expected}'
    old = pathlib.PurePosixPath(old)
    destination = pathlib.PurePosixPath(destination)
    path_keys = {'file_state', 'vibeshine_file_state', 'credentials_file', 'file_apps', 'pkey', 'cert', 'log_path'}
    seen = set()
    pairing_paths = {staging / 'sunshine_state.json', staging / 'vibeshine_state.json'}
    lines = []
    for line in (staging / (expected + '.conf')).read_text().splitlines(keepends=True):
        match = re.fullmatch(r'\s*([A-Za-z0-9_]+)\s*=\s*([^#]*?)(?:\s*#.*)?\s*', line)
        if match and match[1] in path_keys and match[2].strip():
            key, value = match[1], match[2].strip()
            if key in seen:
                raise ValueError(f'duplicate migration path: {key}')
            seen.add(key)
            path = pathlib.PurePosixPath(os.path.normpath(value if value.startswith('/') else str(old / value)))
            if path.is_relative_to(old):
                relative = path.relative_to(old)
                if key != 'log_path' and not (staging / relative).is_file():
                    raise ValueError(f'configured {key} is missing from the confined copy')
                line = f'{key} = {destination / relative}\n'
                if key in {'file_state', 'vibeshine_file_state'}:
                    pairing_paths.add(staging / relative)
            elif key == 'log_path':
                line = f'log_path = {destination / "vibepollo.log"}\n'
            else:
                raise ValueError(f'external {key} must be moved inside the legacy profile before migration')
        lines.append(line)
    # Original profile and its backup are untouched. Keep the original config
    # in the imported copy as well, for administrator inspection/rollback.
    config = staging / 'vibepollo.conf'
    if expected != 'vibepollo' and config.exists():
        raise ValueError('refusing to replace another Vibepollo configuration')
    config.write_text(''.join(lines))
    config.chmod(0o600)
    # Use the same certificate-identity policy as the reversible user cutover.
    # Resolve code only beside this installed helper (or its repository source),
    # never from the imported profile or an environment-controlled search path.
    helper = pathlib.Path(__file__).resolve().with_name('pairing_migration.py')
    if not helper.is_file():
        helper = pathlib.Path(__file__).resolve().parent / 'steamos/local/pairing_migration.py'
    spec = importlib.util.spec_from_file_location('pairing_migration', helper)
    pairing = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pairing)
    for path in pairing_paths:
        if path.is_file():
            pairing.normalize_pairing_state(path)
    for path in staging.rglob('*.json'):
        if path.stat().st_size > 64 * 1024 * 1024:
            raise ValueError('profile JSON exceeds migration limit')
        def remap(value):
            if isinstance(value, str):
                return value.replace(str(old) + '/', str(destination) + '/')
            if isinstance(value, list):
                return [remap(item) for item in value]
            if isinstance(value, dict):
                return {key: remap(item) for key, item in value.items()}
            return value
        original = json.loads(path.read_text())
        updated = remap(original)
        if updated != original:
            path.write_text(json.dumps(updated, indent=2) + '\n')


if __name__ == '__main__':
    normalize(*sys.argv[1:])
