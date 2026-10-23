"""Compare CUDA table layouts using the actual production compiler commands.

FFmpeg can bundle an older ffnvcodec header. Passing CudaFunctions between
objects built with different revisions silently calls the wrong CUDA entry.
No driver, CUDA context, or GPU is required for this ABI regression check.
"""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import tempfile

PROBE = r'''
#include <cstddef>
#include <cstdio>
#include <ffnvcodec/dynlink_loader.h>
int main() {
  std::printf("%zu %zu %zu %zu %zu %zu %zu\n", sizeof(CudaFunctions),
    offsetof(CudaFunctions, cuCtxPushCurrent),
    offsetof(CudaFunctions, cuStreamSynchronize),
    offsetof(CudaFunctions, cuMemcpy2DAsync),
    offsetof(CudaFunctions, cuGraphicsGLRegisterImage),
    offsetof(CudaFunctions, cuGraphicsSubResourceGetMappedArray),
    offsetof(CudaFunctions, cuGraphicsUnregisterResource));
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('build', type=Path)
    args = parser.parse_args()
    commands = json.loads((args.build / 'compile_commands.json').read_text())
    suffixes = ('/src/platform/linux/cuda.cpp',
                '/src/platform/linux/truehdr_gl.cpp',
                '/tests/test_linux_truehdr_gpu.cpp',
                '/src/platform/linux/pyrowave_core.cpp')
    layouts = {}
    with tempfile.TemporaryDirectory(prefix='truehdr-cuda-abi-') as temporary:
        root = Path(temporary)
        probe = root / 'probe.cpp'
        probe.write_text(PROBE)
        for suffix in suffixes:
            entries = [entry for entry in commands if entry['file'].endswith(suffix)]
            if not entries:
                if suffix.endswith('pyrowave_core.cpp'):
                    continue
                raise RuntimeError(f'Missing actual compilation command for {suffix}')
            for number, entry in enumerate(entries):
                original = entry.get('arguments') or shlex.split(entry['command'])
                command = []
                skip = False
                for argument in original:
                    if skip:
                        skip = False
                    elif argument in ('-o', '-MF', '-MT', '-MQ'):
                        skip = True
                    elif argument not in ('-c', '-MD', '-MMD', entry['file']):
                        command.append(argument)
                output = root / f'probe-{len(layouts)}'
                subprocess.run(command + [str(probe), '-o', str(output), '-ldl'],
                               cwd=entry['directory'], check=True, timeout=40,
                               capture_output=True, text=True)
                result = subprocess.check_output([str(output)], text=True).strip()
                key = f'{suffix}:{number}'
                layouts[key] = result
                print(f'{key}: {result}', flush=True)
    reference = next(iter(layouts.values()))
    if any(layout != reference for layout in layouts.values()):
        raise RuntimeError('CUDA dispatch ABI differs across production/TrueHDR/test objects')


if __name__ == '__main__':
    main()
