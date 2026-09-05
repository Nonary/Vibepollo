#!/usr/bin/env python3
"""Fetch the locked Gamescope revision, apply the HDR patch, build and stage it.

Run in a SteamOS-compatible SDK. Does not install packages or change a session.
"""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('work', type=pathlib.Path, help='New, empty build workspace')
parser.add_argument('stage', type=pathlib.Path, help='New staging root (DESTDIR)')
parser.add_argument('--jobs', type=int, default=4)
parser.add_argument('--meson-option', action='append', default=[])
args = parser.parse_args()
here = pathlib.Path(__file__).resolve().parent
lock = json.loads((here / 'source-lock.json').read_text())
patch = here / lock['patch']
if hashlib.sha256(patch.read_bytes()).hexdigest() != lock['patch_sha256']:
    raise SystemExit('Patch checksum does not match source-lock.json')
work, stage = args.work.resolve(), args.stage.resolve()
for directory in [work, stage]:
    if directory.exists():
        raise SystemExit(f'Refusing existing directory: {directory}')
    directory.mkdir(parents=True)
source, build = work / 'source', work / 'build'

def run(command, **kwargs):
    subprocess.run(command, check=True, **kwargs)

run(['git', 'clone', '--branch', lock['tag'], '--depth', '1', lock['upstream'], str(source)])
actual = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=source, text=True).strip()
if actual != lock['commit']:
    raise SystemExit(f'Upstream tag moved: {actual}')
run(['git', 'submodule', 'update', '--init', '--recursive', '--depth', '1', '--',
     'subprojects/wlroots', 'subprojects/libliftoff', 'subprojects/vkroots', 'src/reshade', 'thirdparty/SPIRV-Headers'], cwd=source)
run(['git', 'apply', '--check', str(patch)], cwd=source)
run(['git', 'apply', str(patch)], cwd=source)
run(['meson', 'setup', str(build), str(source), '--buildtype=release', '--prefix=/opt/vibepollo-gamescope',
     '-Denable_openvr_support=false', '-Dpipewire=enabled', '-Drt_cap=enabled', '-Ddrm_backend=enabled',
     '-Dsdl2_backend=enabled', '-Dinput_emulation=enabled', '-Davif_screenshots=enabled',
     '-Dwlroots:werror=false'] + args.meson_option)
run(['meson', 'compile', '-C', str(build), '-j', str(args.jobs)])
run(['meson', 'install', '-C', str(build), '--no-rebuild'], env={**os.environ, 'DESTDIR': str(stage)})
shutil.copytree(source / 'scripts', stage / 'opt/vibepollo-gamescope/share/gamescope/scripts', dirs_exist_ok=True)
(stage / 'source-lock.json').write_text(json.dumps(lock, indent=2) + '\n')
print(f'Built and staged {lock["tag"]} + HDR capture profile 1 at {stage}')
