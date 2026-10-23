#!/usr/bin/env python3
"""Portable exact-target registry/real coordinator and monitor-entrypoint proof.

No display, broker, service, or device APIs are invoked. Serialize this runner
with any other local C++ validation using the established compiler lock.
"""
import argparse
import pathlib
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--json-include', type=pathlib.Path)
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='exact-target-leases-') as temporary:
    for enabled in (False, True):
        executable = pathlib.Path(temporary) / ('enabled' if enabled else 'default')
        command = ['g++', '-std=c++23', '-O0', '-g0', '-pthread', '-I' + str(root), '-I' + str(root / 'src')]
        if args.json_include:
            command += ['-I' + str(args.json_include)]
        if enabled:
            command += ['-DSUNSHINE_EXPERIMENTAL_TARGET_LEASES']
        command += [str(root / 'src/remote_session.cpp'), str(root / 'src/remote_display_topology.cpp'),
                    str(root / 'tests/input/test_exact_target_leases.cpp'), '-lboost_process', '-o', str(executable)]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
