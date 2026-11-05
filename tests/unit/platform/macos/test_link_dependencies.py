"""Configure the actual dependency fragments without downloading or linking SDKs.

This checks target/library composition, not a native Apple linker invocation.
"""

import argparse
import hashlib
import io
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cmake', default='cmake')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[4]
    tests = (root / 'tests/CMakeLists.txt').read_text()
    # Evaluate the standalone component dependency declarations too.
    preamble = tests[tests.index('set(SUNSHINE_TEST_FFMPEG_PLATFORM_LIBRARIES'):tests.index('# Test categories')]
    common = r'''
cmake_minimum_required(VERSION 3.20)
project(LinkDependencies NONE)
set(WIN32 FALSE)
set(UNIX TRUE)
set(FREEBSD FALSE)
set(APPLE TRUE)
set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR arm64)
# The automatic path is given a populated archive cache. No network access.
function(execute_process)
    cmake_parse_arguments(ARG "" "OUTPUT_VARIABLE" "" ${ARGN})
    if(ARG_OUTPUT_VARIABLE)
        set(${ARG_OUTPUT_VARIABLE} fixture-tag PARENT_SCOPE)
    endif()
endfunction()
'''
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        # Exercise the real dependency module against a controlled release pin.
        # The archive is intentionally minimal: this test configures link lists,
        # but never links or executes a Darwin binary.
        archive = directory / 'Darwin-arm64-ffmpeg.tar.gz'
        with tarfile.open(archive, 'w:gz') as tar:
            content = b'FFmpeg link fixture\n'
            entry = tarfile.TarInfo('ffmpeg/lib/libavcodec.a')
            entry.size = len(content)
            tar.addfile(entry, io.BytesIO(content))
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()

        fragments = directory / 'cmake-dependencies'
        fragments.mkdir()
        shutil.copyfile(root / 'cmake/dependencies/ffmpeg.cmake', fragments / 'ffmpeg.cmake')
        release = (root / 'cmake/dependencies/ffmpeg-release.cmake').read_text()
        release, count = re.subn(
            r'(?m)^(set\(FFMPEG_SHA256_Darwin_arm64 ")[0-9a-f]{64}("\))$',
            lambda match: match.group(1) + digest + match.group(2), release,
        )
        if count != 1:
            raise AssertionError('Expected exactly one Darwin arm64 FFmpeg pin')
        (fragments / 'ffmpeg-release.cmake').write_text(release)

        for mode in ('automatic', 'supplied', 'override', 'linux', 'windows'):
            source = directory / mode
            source.mkdir()
            build = source / 'build'
            archives = build / '_deps/ffmpeg'
            (archives / 'lib').mkdir(parents=True)
            (archives / 'lib/libavcodec.a').touch()
            if mode == 'automatic':
                pinned = build / '_deps' / f'ffmpeg-{digest}'
                pinned.mkdir(parents=True)
                shutil.copyfile(archive, pinned / archive.name)
            code = common
            if mode != 'automatic':
                code += f'set(FFMPEG_PREPARED_BINARIES "{archives}")\n'
            if mode == 'override':
                code += 'set(FFMPEG_PLATFORM_LIBRARIES custom-platform-library)\n'
            elif mode == 'linux':
                code += 'set(APPLE FALSE)\n'
            elif mode == 'windows':
                code += 'set(APPLE FALSE)\nset(WIN32 TRUE)\n'
            code += f'include("{fragments}/ffmpeg.cmake")\n'
            code += preamble
            if mode in ('automatic', 'supplied'):
                code += r'''
foreach(framework CoreFoundation CoreMedia CoreVideo VideoToolbox AudioToolbox)
    foreach(variable FFMPEG_PLATFORM_LIBRARIES FFMPEG_LIBRARIES SUNSHINE_TEST_FFMPEG_PLATFORM_LIBRARIES)
        if(NOT "-framework ${framework}" IN_LIST ${variable})
            message(FATAL_ERROR "${variable} is missing ${framework}")
        endif()
    endforeach()
endforeach()
'''
            elif mode == 'override':
                code += r'''
if(NOT FFMPEG_PLATFORM_LIBRARIES STREQUAL "custom-platform-library")
    message(FATAL_ERROR "Caller platform override was replaced")
endif()
'''
            else:
                code += r'''
if(FFMPEG_LIBRARIES MATCHES "framework")
    message(FATAL_ERROR "Apple frameworks leaked onto another platform")
endif()
'''
            (source / 'CMakeLists.txt').write_text(code)
            subprocess.run([args.cmake, '-S', str(source), '-B', str(build)], check=True)
            if mode == 'automatic':
                # Cached archives must still be checked; a stale or tampered
                # cache may not satisfy the production release pin.
                (pinned / archive.name).write_bytes(b'tampered archive')
                result = subprocess.run(
                    [args.cmake, '-S', str(source), '-B', str(build)],
                    capture_output=True, text=True,
                )
                if result.returncode == 0 or 'FFmpeg archive checksum mismatch' not in result.stderr:
                    raise AssertionError('Tampered FFmpeg archive was not rejected by its checksum')

        source = directory / 'zlib'
        source.mkdir()
        # Stand in only for dependency discovery, not for the production lists.
        (source / 'FindZLIB.cmake').write_text('''
if(NOT ZLIB_FIND_REQUIRED)
    message(FATAL_ERROR "ZLIB must be required")
endif()
add_library(ZLIB::ZLIB INTERFACE IMPORTED)
''')
        code = common + f'''
list(PREPEND CMAKE_MODULE_PATH "{source}")
function(FIND_LIBRARY name)
    set(${{name}} "fixture-library" PARENT_SCOPE)
endfunction()
set(SUNSHINE_ENABLE_TRAY FALSE)
set(SUNSHINE_SOURCE_ASSETS_DIR "{root}/src_assets")
include("{root}/cmake/dependencies/macos.cmake")
include("{root}/cmake/compile_definitions/macos.cmake")
if(NOT TARGET ZLIB::ZLIB OR NOT "ZLIB::ZLIB" IN_LIST SUNSHINE_EXTERNAL_LIBRARIES)
    message(FATAL_ERROR "macOS application does not discover and link ZLIB")
endif()
'''
        (source / 'CMakeLists.txt').write_text(code)
        subprocess.run([args.cmake, '-S', str(source), '-B', str(source / 'build')], check=True)
    print('Apple archive/component frameworks and application ZLIB dependency checks passed')


if __name__ == '__main__':
    main()
