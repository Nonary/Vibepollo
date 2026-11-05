#!/usr/bin/env python3
"""Generate a focused fixture using the production launch-resolution block."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('root', type=Path)
parser.add_argument('compiler', nargs='?')
parser.add_argument('--generate-only', action='store_true')
parser.add_argument('--output', type=Path)
args = parser.parse_args()
root = args.root.resolve()
source = (root / 'src/process.cpp').read_text()
start = source.index('    const bool has_resolution_override =')
end = source.index('    this->initial_display =', start)
block = source[start:end]
keys = ('SUNSHINE_CLIENT_WIDTH', 'SUNSHINE_CLIENT_HEIGHT', 'APOLLO_CLIENT_RENDER_WIDTH', 'APOLLO_CLIENT_RENDER_HEIGHT')
lines = source.splitlines()
env = '\n'.join(next(line for line in lines if f'_env["{key}"] =' in line) for key in keys)
template = Path(__file__).with_suffix('.cpp.in').read_text().replace('@BLOCK@', block).replace('@ENV@', env)
if args.generate_only:
    if not args.output:
        parser.error('--generate-only requires --output')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(template)
    raise SystemExit(0)
if not args.compiler:
    parser.error('compiler is required unless --generate-only is used')
with tempfile.TemporaryDirectory(prefix='launch-resolution-') as directory:
    path = Path(directory)
    fixture, executable = path / 'test.cpp', path / 'test'
    fixture.write_text(template)
    subprocess.run([args.compiler, '-std=c++23', str(fixture), '-lgtest_main', '-lgtest', '-pthread', '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
