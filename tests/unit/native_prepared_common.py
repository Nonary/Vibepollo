"""Helpers for portable replay of immutable production snippets."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def arguments(description):
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--revision', help='Read immutable committed objects instead of working files')
    parser.add_argument('--compiler', default='clang++')
    return parser.parse_args()


def read(args, name):
    if args.revision:
        return subprocess.check_output(['git', '-C', str(args.repo), 'show', f'{args.revision}:{name}'], text=True)
    return (args.repo / name).read_text()


def block(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 0
    for end in range(opening, len(source)):
        if source[end] == '{':
            depth += 1
        elif source[end] == '}':
            depth -= 1
            if depth == 0:
                return source[start:end + 1]
    raise ValueError('Unclosed production block: ' + signature)


def compile_run(args, program, name, flags=()):
    with tempfile.TemporaryDirectory(prefix='native-prepared-' + name + '-') as directory:
        cpp = Path(directory) / (name + '.cpp')
        binary = Path(directory) / name
        cpp.write_text(program)
        subprocess.run([args.compiler, '-std=c++23', '-Wall', '-Wextra', '-Werror',
                        *flags, str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
