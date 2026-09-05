#!/usr/bin/env python3
"""Build a native integration probe from a completed Ninja Vibepollo build.
Usage: build-hdr-probe.py BUILD_DIRECTORY OUTPUT_DIRECTORY
"""
import json
import pathlib
import shlex
import subprocess
import sys

build = pathlib.Path(sys.argv[1]).resolve()
out = pathlib.Path(sys.argv[2]).resolve()
out.mkdir(parents=True, exist_ok=True)
source = pathlib.Path(__file__).with_name('hdr-capture-probe.cpp').resolve()
commands = json.loads((build / 'compile_commands.json').read_text())
command = next(c for c in commands if c['file'].endswith('/gamescopegrab.cpp'))
args = shlex.split(command['command'])
args[args.index('-o') + 1] = str(out / 'probe.o')
args[args.index('-c') + 1] = str(source)
args = ['-O0' if x == '-O3' else x for x in args]
subprocess.run(args, cwd=build, check=True)
main_object = out / 'main-for-probe.o'
subprocess.run(['objcopy', '--redefine-sym', 'main=vibepollo_application_main',
                str(build / 'CMakeFiles/sunshine.dir/src/main.cpp.o'), str(main_object)], check=True)
lines = subprocess.check_output(['ninja', '-t', 'commands', 'sunshine'], cwd=build, text=True).splitlines()
args = shlex.split(next(line for line in reversed(lines) if ' -o vibepollo ' in line))
start = next(i for i, x in enumerate(args) if x.endswith('/c++'))
args = args[start:]
if '&&' in args:
    args = args[:args.index('&&')]
args = [str(main_object) if x == 'CMakeFiles/sunshine.dir/src/main.cpp.o' else x for x in args]
args[args.index('-o') + 1] = str(out / 'hdr-capture-probe')
args.insert(1, str(out / 'probe.o'))
subprocess.run(args, cwd=build, check=True)
print(out / 'hdr-capture-probe')
