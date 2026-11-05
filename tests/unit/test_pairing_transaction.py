#!/usr/bin/env python3
"""Compile the production pairing transaction with deterministic I/O adapters."""
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
source = (root / 'src/nvhttp.cpp').read_text()
start = source.index('    bool add_authorized_client(')
end = source.index('    struct resolved_client_identity_t', start)
body = source[start:end]
template = Path(__file__).with_suffix('.cpp.in').read_text()
if args.generate_only:
    if not args.output:
        parser.error('--generate-only requires --output')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(template.replace('@FUNCTION@', body))
    raise SystemExit(0)
if not args.compiler:
    parser.error('compiler is required unless --generate-only is used')
with tempfile.TemporaryDirectory(prefix='pairing-transaction-') as directory:
    path = Path(directory)
    fixture = path / 'test.cpp'
    fixture.write_text(template.replace('@FUNCTION@', body))
    executable = path / 'test'
    subprocess.run([args.compiler, '-std=c++23', '-pthread', '-I', str(root), str(fixture),
                    str(root / 'src/http_pairing_policy.cpp'), '-lgtest_main', '-lgtest',
                    '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
