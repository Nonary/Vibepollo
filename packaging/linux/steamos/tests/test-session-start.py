#!/usr/bin/env python3
"""Exercise mode startup with isolated units in an available user manager.

No real desktop, compositor, or Vibepollo service is started or stopped.
"""
import os
import pathlib
import subprocess
import tempfile
import uuid


def systemctl(*args, check=True):
    return subprocess.run(
        ['systemctl', '--user', *args], check=check,
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )


def exercise(source, direct_start):
    prefix = 'vibepollo-session-test-' + uuid.uuid4().hex
    names = {name: prefix + '-' + name for name in (
        'graphical-session.target', 'gamescope-session.target',
        'gamescope-session.service', 'vibepollo-steamos.service',
        'desktop.target',
    )}
    host = names['vibepollo-steamos.service']
    with tempfile.TemporaryDirectory(prefix=prefix) as directory:
        root = pathlib.Path(directory)
        ready = root / 'compositor-ready'
        unit = source.replace('Wants=pipewire.service wireplumber.service', '')
        unit = unit.replace(' pipewire.service wireplumber.service', '')
        unit = unit.replace(
            'ExecStart=%h/.local/bin/vibepollo-steamos-session',
            'ExecStart=/usr/bin/sleep infinity\nExecStop=/usr/bin/sleep 1',
        )
        if not direct_start:
            unit = unit.replace(
                'WantedBy=graphical-session.target gamescope-session.target',
                'WantedBy=graphical-session.target',
            )
        for original, isolated in names.items():
            unit = unit.replace(original, isolated)
        (root / host).write_text(unit)
        units = {
            'graphical-session.target': '[Unit]\nDescription=Test shared graphical target\n',
            'desktop.target': '[Unit]\nRequires=graphical-session.target\n',
            'gamescope-session.target': (
                '[Unit]\nRequires=graphical-session.target gamescope-session.service\n'
                'After=graphical-session.target\n'
            ),
            'gamescope-session.service': (
                '[Unit]\nBefore=graphical-session.target\n'
                '[Service]\nType=oneshot\nRemainAfterExit=yes\n'
                f'ExecStart=/usr/bin/touch {ready}\n'
            ),
        }
        for original, contents in units.items():
            for name, isolated in names.items():
                contents = contents.replace(name, isolated)
            (root / names[original]).write_text(contents)
        try:
            systemctl('link', '--runtime', *map(str, root.iterdir()))
            systemctl('enable', '--runtime', host)
            systemctl('start', names['desktop.target'])
            systemctl('start', host)
            # The shared target survives while desktop shutdown stops its host.
            systemctl('stop', '--no-block', host)
            assert systemctl('is-active', names['graphical-session.target']).stdout.strip() == 'active'
            # Check that Gaming Mode waits for the compositor before launching.
            (root / host).write_text(unit.replace(
                '[Service]', f'[Service]\nExecStartPost=/usr/bin/test -f {ready}',
            ))
            systemctl('daemon-reload')
            assert str(ready) in systemctl('show', host, '-p', 'ExecStartPost').stdout
            systemctl('start', names['gamescope-session.target'])
            active = systemctl('is-active', host, check=False).stdout.strip()
            assert (active == 'active') == direct_start, active
        finally:
            systemctl('stop', *names.values(), check=False)
            systemctl('disable', '--runtime', *names.values(), check=False)
            systemctl('daemon-reload', check=False)
            systemctl('reset-failed', *names.values(), check=False)


if __name__ == '__main__':
    if os.environ.get('VIBEPOLLO_TEST_SYSTEMD_INTEGRATION') != '1':
        print('SKIP: set VIBEPOLLO_TEST_SYSTEMD_INTEGRATION=1 to use the live user manager.')
        raise SystemExit(0)
    source = (pathlib.Path(__file__).resolve().parent.parent /
              'vibepollo-steamos.service').read_text()
    if systemctl('show-environment', check=False).returncode:
        raise SystemExit('A reachable systemd user manager is required for this test.')
    exercise(source, direct_start=False)
    exercise(source, direct_start=True)
    print('Reproduced missing Gaming Mode startup; corrected service starts after compositor readiness.')
