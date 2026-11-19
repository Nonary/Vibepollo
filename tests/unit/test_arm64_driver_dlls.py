"""Compile actual NVIDIA loader DLL selection for each Windows architecture."""
import re
from native_prepared_common import arguments, read, compile_run

args = arguments(__doc__)
paths = [('src/nvenc/nvenc_d3d11.cpp', 'nvEncodeAPIa64.dll', 'nvEncodeAPI64.dll', 'nvEncodeAPI.dll'),
         ('src/platform/windows/nvprefs/nvapi_opensource_wrapper.cpp', 'nvapia64.dll', 'nvapi64.dll', 'nvapi.dll')]
for filename, arm, win64, win32 in paths:
    source = read(args, filename)
    arm_pos = source.index('"' + arm + '"')
    start = source.rfind('#if', 0, arm_pos)
    end = source.index('#endif', arm_pos) + len('#endif')
    selection = source[start:end]
    assert 'LOAD_LIBRARY_SEARCH_SYSTEM32' in source
    variable = re.search(r'(?:auto|char\s*\*)\s+(\w+)\s*=', selection).group(1)
    for label, defines, expected in [
        ('x86', '', win32), ('x64', '#define _WIN64 1\n', win64),
        ('msvc-arm64', '#define _WIN64 1\n#define _M_ARM64 1\n', arm),
        ('gnu-arm64', '#define _WIN64 1\n#define __aarch64__ 1\n', arm)]:
        program = ('#include <cassert>\n#include <cstring>\n'
                   '#undef _WIN64\n#undef _M_ARM64\n#undef __aarch64__\n' + defines +
                   'int main() {\n' + selection +
                   f'\nassert(std::strcmp({variable},"{expected}")==0);\n}}\n')
        compile_run(args, program, 'driver-dll')
        print(f'PASS {filename}: {label} selects {expected}')
