#!/usr/bin/env python3
"""Run native HDR capture/encode smoke tests without changing the live session."""
import argparse
import json
import os
import pathlib
import re
import signal
import struct
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--gamescope', type=pathlib.Path, required=True)
parser.add_argument('--pattern', type=pathlib.Path, required=True)
parser.add_argument('--probe', type=pathlib.Path, required=True)
parser.add_argument('--payload', type=pathlib.Path, required=True)
parser.add_argument('--output', type=pathlib.Path, required=True)
parser.add_argument('--scripts', type=pathlib.Path)
parser.add_argument('--mode', choices=['hdr', 'sdr-source', 'stock'], default='hdr')
parser.add_argument('--capture-only', action='store_true')
parser.add_argument('--encoder', choices=['vaapi', 'vulkan'], default='vaapi')
parser.add_argument('--full-range', action='store_true', help='Request full-range HEVC; default is limited range')
parser.add_argument('--probe-container', help='Run the probe in an existing container sharing the runtime sockets')
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
env = os.environ.copy()
env['LD_LIBRARY_PATH'] = str(args.payload.resolve() / 'lib')
env.pop('GAMESCOPE_WAYLAND_DISPLAY', None)
if args.scripts:
    env['GAMESCOPE_SCRIPT_PATH'] = str(args.scripts.resolve())
command = [str(args.gamescope.resolve()), '--backend', 'headless', '-W', '1280', '-H', '800',
           '-w', '1280', '-h', '800', '-r', '30', '--expose-wayland', '--', str(args.pattern.resolve())]
if args.mode == 'sdr-source':
    command.append('--sdr')
compositor_log = args.output / 'gamescope.log'
process = None
try:
    with compositor_log.open('wb') as log:
        process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        text = compositor_log.read_text(errors='replace')
        wayland = re.search("Running compositor on wayland display '([^']+)'", text)
        if wayland and 'PATTERN ' in text:
            env['GAMESCOPE_WAYLAND_DISPLAY'] = wayland[1]
            break
        if process.poll() is not None:
            raise RuntimeError(f'Test compositor exited ({process.returncode}); see {compositor_log}')
        time.sleep(.1)
    else:
        raise RuntimeError('Test source did not become ready within 15 seconds')
    bitstream = (args.output / 'pattern.hevc').resolve()
    env['VIBEPOLLO_HDR_BITSTREAM'] = str(bitstream)
    env['VIBEPOLLO_HDR_ENCODER'] = args.encoder
    env.pop('VIBEPOLLO_HDR_FULL_RANGE', None)
    if args.full_range:
        env['VIBEPOLLO_HDR_FULL_RANGE'] = '1'
    env.pop('VIBEPOLLO_HDR_CAPTURE_ONLY', None)
    if args.capture_only:
        env['VIBEPOLLO_HDR_CAPTURE_ONLY'] = '1'
    command = [str(args.probe.resolve())]
    if args.mode != 'hdr':
        command.append('--' + args.mode)
    if args.probe_container:
        prefix = ['podman', 'exec', '--user', str(os.getuid()), '--workdir', str(args.payload.resolve())]
        for key in ['XDG_RUNTIME_DIR', 'GAMESCOPE_WAYLAND_DISPLAY', 'VIBEPOLLO_HDR_BITSTREAM', 'VIBEPOLLO_HDR_ENCODER', 'VIBEPOLLO_HDR_CAPTURE_ONLY', 'VIBEPOLLO_HDR_FULL_RANGE']:
            if key in env:
                prefix += ['--env', key + '=' + env[key]]
        prefix += ['--env', 'LD_LIBRARY_PATH=/usr/lib:' + env['LD_LIBRARY_PATH']]
        command = prefix + [args.probe_container] + command
    with (args.output / 'capture.log').open('w') as log:
        subprocess.run(command, env=env, cwd=args.payload.resolve(), stdout=log, stderr=subprocess.STDOUT, timeout=55, check=True)
    if args.mode != 'stock' and not args.capture_only:
        result = subprocess.run(['ffprobe', '-v', 'error', '-show_streams', '-of', 'json', str(bitstream)],
                                capture_output=True, text=True, check=True, timeout=15)
        (args.output / 'ffprobe.json').write_text(result.stdout)
        (args.output / 'ffprobe.log').write_text(result.stderr)
        stream = json.loads(result.stdout)['streams'][0]
        decoded = subprocess.run(['ffmpeg', '-v', 'error', '-xerror', '-i', str(bitstream), '-f', 'framemd5', '-'],
                                 capture_output=True, text=True, timeout=20, check=True)
        (args.output / 'decode.log').write_text(decoded.stderr)
        (args.output / 'frames.md5').write_text(decoded.stdout)
        frames = [line for line in decoded.stdout.splitlines() if line and not line.startswith('#')]
        # FFmpeg can return success even when all frames failed to decode.
        if result.stderr.strip() or decoded.stderr.strip() or len(frames) < 12:
            raise RuntimeError(f'HEVC decoding failed ({len(frames)} frames); see ffprobe.log and decode.log')
        for key, expected in {'profile': 'Main 10', 'pix_fmt': 'yuv420p10le', 'color_primaries': 'bt2020',
                              'color_transfer': 'smpte2084', 'color_space': 'bt2020nc',
                              'color_range': 'pc' if args.full_range else 'tv'}.items():
            if stream.get(key) != expected:
                raise RuntimeError(f'Incorrect HEVC {key}: {stream.get(key)!r}; expected {expected!r}')
        pixels = subprocess.run(['ffmpeg', '-v', 'error', '-i', str(bitstream),
                                 '-vf', 'format=gbrp16le,crop=1280:1:0:400', '-f', 'rawvideo', '-'],
                                capture_output=True, check=True, timeout=20)
        row_bytes = 1280 * 3 * 2
        if pixels.stderr.strip() or len(pixels.stdout) != len(frames) * row_bytes:
            raise RuntimeError('Decoded pixel check did not produce one RGB row per frame')
        measurements = []
        for frame in range(len(frames)):
            bands = []
            for band in range(8):
                rgb = [round(struct.unpack_from('<H', pixels.stdout, frame * row_bytes +
                              2 * (plane * 1280 + band * 160 + 80))[0] * 1023 / 65535) for plane in [2, 0, 1]]
                bands.append(rgb)
                for channel, actual in enumerate(rgb):
                    expected = [0, 520, 594, 769, 923, 769, 769, 769][band]
                    if args.mode == 'sdr-source':
                        expected = 0 if band == 0 else 594
                    if band >= 5 and channel != band - 5:
                        expected = 0
                    if (args.mode != 'sdr-source' or band < 5) and abs(actual - expected) > 8:
                        raise RuntimeError(f'Decoded PQ mismatch: frame {frame}, band {band}, RGB {rgb}')
            measurements.append(bands)
        (args.output / 'decoded-pixels.json').write_text(json.dumps(measurements, indent=2) + '\n')
    checks = 'capture and metadata only' if args.capture_only else 'capture, signaling and codec checks'
    if args.mode == 'stock':
        checks = 'stock HDR rejection and SDR availability'
    print(f'{args.mode}: {checks} PASS')
finally:
    if process is not None:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=5)
        (args.output / 'cleanup.log').write_text(f'Stopped task compositor group {process.pid}.\n')
