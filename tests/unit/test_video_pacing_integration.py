"""Compile production pacing/admission call sites with deterministic sessions.

No sockets, encoder, mail bus or platform backend is started. --source-ref
rebuilds the same behavioral tests from a pinned Git base as a negative control.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('root', type=Path)
parser.add_argument('compiler')
parser.add_argument('--source-ref')
parser.add_argument('--output-dir', type=Path)
parser.add_argument('--generate-only', action='store_true')
args = parser.parse_args()
root = args.root.resolve()


def read(name):
    if args.source_ref:
        return subprocess.check_output(['git', '-C', str(root), 'show', f'{args.source_ref}:{name}'], text=True)
    return (root / name).read_text()


source = read('src/stream.cpp')


def section(begin, end, offset=0):
    first = source.index(begin, offset)
    return source[first:source.index(end, first)]


updated = 'static bool pacing_admits_locked(' in source
fractional = 'video_pacing_delay_ns(' in read('src/stream_protocol.h')
helpers = section('  static video_pacing_reservation_t pacing_reservation_for_config(', '  int set_bitrate_for_sessions(') if updated else ''
abr = section('  int set_bitrate_for_sessions(', '\n  static const char *state_name')
start_at = source.index('    int start(session_t &session,')
publish = section('      // Insert this session into the session list', '\n      auto addr =', start_at)
if updated:
    assert 'constexpr int START_PACING_REJECTED = -2;' in read('src/stream.h')
    pacer = section('        const auto pacing_ceiling =', '        if (pyrowave_session) {')
    clamp = '' if fractional else section('          // An adaptive pace may be slower', '          const std::pair pacing_source')
else:
    pacer = section('        size_t pacing_bps;', '        if (pyrowave_session) {')
    clamp = ''
batches = section('        size_t max_batch_size_bytes =', '        // Don\'t ignore the last ratecontrol group')
frame_delay = '' if fractional else '''
    const auto frame_pacing_delay = [&](std::uint64_t packets_sent) {
      return std::chrono::nanoseconds(1'000'000 * packets_sent / ratecontrol_packets_in_1ms);
    };'''
if fractional:
    assert source.count('frame_pacing_delay(ratecontrol_frame_packets_sent)') == 3
    assert 'if (!wait_for_video_pacing(due,' in source
    assert 'throw video_send_failed_t {true, unsigned(blockIndex), 0};' in source
pacing_at = source.index('              // Do pacing within the frame.')
condition = section('              if (', ') {', pacing_at).strip()[len('if ('):]
fec_count = section('      auto payload_size = payload.size();', '\n      // If we need to store a zero-padded data shard', source.index('static fec_t encode('))
splitter = section('        // The max number of data shards per block', '\n      }\n\n      decltype(fec_blocks)::iterator')
detail_max = re.search(r'constexpr int MAX_DETAIL_FEC_PERCENTAGE = \d+;', read('src/pyrowave_policy.h')).group()
http = read('src/nvhttp.cpp')
http_at = http.index('    const int updated = stream::set_bitrate_for_sessions(')
http_handler = http[http.index('\n', http_at) + 1:http.index('    BOOST_LOG(info) << "Client ["sv', http_at)]
rtsp = read('src/rtsp.cpp')
rtsp_at = rtsp.index('          stream_session = stream::session::alloc(config, *launch_session);')
rtsp_mapping = rtsp[rtsp.index('\n', rtsp_at) + 1:rtsp.index('        } catch (const std::exception &e)', rtsp_at)]
if updated:
    assert 'startup_failed, startup_status, startup_error' in rtsp
    assert 'startup_status, startup_status == 400 ? "BAD REQUEST"' in rtsp
template = (Path(__file__).with_suffix('.cpp.in')).read_text()
program = template
for key, text in {'HELPERS': helpers, 'ABR': abr, 'PUBLISH': publish, 'PACER': pacer, 'CLAMP': clamp,
                  'BATCHES': batches, 'FRAME_DELAY': frame_delay, 'CONDITION': condition, 'FEC_COUNT': fec_count, 'SPLITTER': splitter,
                  'DETAIL_MAX': detail_max, 'UPDATED': '1' if updated else '0',
                  'RATE_HELPERS': ('using stream::video_pacing_batch_packets; using stream::video_pacing_delay_ns;' if fractional else 'using stream::video_pacing_packets_per_ms;'),
                  'HTTP_HANDLER': http_handler, 'RTSP_MAPPING': rtsp_mapping}.items():
    program = program.replace('@' + key + '@', text)

# Real layout contracts used by the sender; the fixture's plain byte structs
# provide only these sizes, while arithmetic/call sites come from production.
assert re.search(r'sizeof\(video_short_frame_header_t\)\s*==\s*8', source)
assert 'sizeof(video_packet_raw_t) - MAX_RTP_HEADER_SIZE == pyrowave::protocol::SHARD_OVERHEAD_BYTES' in source
assert 'std::uint8_t iv[12];' in source and 'std::uint8_t tag[16];' in source

with tempfile.TemporaryDirectory(prefix='video-pacing-') as temporary:
    output = args.output_dir or Path(temporary)
    output.mkdir(parents=True, exist_ok=True)
    cpp = output / 'video_pacing_integration.cpp'
    cpp.write_text(program)
    (output / 'stream_protocol.h').write_text(read('src/stream_protocol.h'))
    protocol = output / 'stream_protocol.cpp'
    protocol.write_text(read('src/stream_protocol.cpp'))
    if args.generate_only:
        if not args.output_dir:
            parser.error('--generate-only requires --output-dir')
        raise SystemExit(0)
    exe = output / 'video_pacing_integration'
    # CXXFLAGS allows a permitted cached compiler's include locations without
    # embedding host-specific toolchain paths in the test itself.
    import shlex
    common = shlex.split(os.environ.get('CXXFLAGS', ''))
    compile_command = [args.compiler, '-std=c++23', '-O0', '-g0', '-pthread'] + common + [
        '-I', str(output), str(cpp), str(protocol), '-lgtest_main', '-lgtest', '-o', str(exe)]
    # Preserve nice 15 when the test runner already has that priority.
    priority = max(0, 15 - os.getpriority(os.PRIO_PROCESS, 0)) if hasattr(os, 'getpriority') else 0
    prefix = ['nice', '-n', str(priority)] if os.name == 'posix' else []
    subprocess.run(prefix + compile_command, check=True)
    result = subprocess.run(prefix + [str(exe), '--gtest_output=json:' + str(output / 'results.json')])
    (output / 'recipe.json').write_text(json.dumps({'source_ref': args.source_ref, 'updated': updated,
        'compile_command': prefix + compile_command, 'run_command': prefix + [str(exe)],
        'runner_nice': os.getpriority(os.PRIO_PROCESS, 0) if hasattr(os, 'getpriority') else None,
        'exit_code': result.returncode}, indent=2) + '\n')
    raise SystemExit(result.returncode)
