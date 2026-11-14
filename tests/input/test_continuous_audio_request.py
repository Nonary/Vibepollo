#!/usr/bin/env python3
"""Exercise the production query parser and RTSP audio flag with client inputs."""
import argparse
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--compiler', default='c++')
args = parser.parse_args()
nvhttp = (root / 'src/nvhttp.cpp').read_text()
rtsp = (root / 'src/rtsp.cpp').read_text()
utility = (root / 'src/utility.h').read_text()
get_arg = nvhttp[nvhttp.index('    std::string get_arg('):nvhttp.index('    // Helper function to extract command entries', nvhttp.index('    std::string get_arg('))]
conversion = utility[utility.index('  inline std::int64_t from_chars('):utility.index('  template<class X, class Y>', utility.index('  inline std::int64_t from_chars('))]
launch_begin = nvhttp.index('make_launch_session_from_snapshot(')
launch_end = nvhttp.index('launch_session->gcpersist =', launch_begin)
launch = nvhttp[launch_begin:launch_end]
line = next(line for line in launch.splitlines() if 'launch_session->continuous_audio =' in line)
start = rtsp.index('    if (session->continuous_audio) {')
end = rtsp.index('\n    }', start) + len('\n    }')
rtsp_gate = rtsp[start:end]
code = r'''
#include <cassert>
#include <cstdint>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
using namespace std::literals;
using args_t = std::map<std::string, std::string>;
namespace util { CONVERSION }
GET_ARG
struct Session { bool continuous_audio = false; };
namespace audio { struct config_t { enum { CONTINUOUS_AUDIO }; }; }
struct Config { struct Audio { bool flags[1] {}; } audio; };
std::ostringstream log_sink;
#define BOOST_LOG(level) log_sink
bool launch_and_negotiate(const args_t &args) {
  Session value; auto *launch_session = &value; auto *session = &value; Config config;
  LAUNCH
  RTSP
  return config.audio.flags[audio::config_t::CONTINUOUS_AUDIO];
}
int main() {
  assert(!launch_and_negotiate({}));
  assert(!launch_and_negotiate({{"continuousAudio", "0"}}));
  assert(launch_and_negotiate({{"continuousAudio", "1"}}));
}
'''.replace('CONVERSION', conversion).replace('GET_ARG', get_arg).replace('LAUNCH', line).replace('RTSP', rtsp_gate)
with tempfile.TemporaryDirectory(prefix='continuous-audio-') as temp:
    source, binary = Path(temp) / 'test.cpp', Path(temp) / 'test'
    source.write_text(code)
    subprocess.run([args.compiler, '-std=c++20', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('continuous-audio query and RTSP flag: absent, 0 and 1 passed')
