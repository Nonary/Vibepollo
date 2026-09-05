"""Compile the production RTSP-to-display-policy adapter and refresh helper.

Usage: python3 tests/unit/test_display_policy_adapter.py [repo] [adapter-source]
The optional source override supports before/after verification. Network and
Windows display APIs are not needed; only the launch-session data is stubbed.
"""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2]
adapter_source = pathlib.Path(sys.argv[2]).read_text() if len(sys.argv) > 2 else (root / 'src/display_device.cpp').read_text()


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


adapter = function(adapter_source, 'policy::session_t to_policy_session(')
refresh = function((root / 'src/rtsp.h').read_text(), 'inline std::uint32_t effective_display_refresh_millihz(')
# Vibepollo's launch boundary stores millihertz, whereas Vibeshine stores Hz.
scaled = 'normalize_refresh_millihz(session.fps)' in refresh
program = r'''
#include <cassert>
#include <iostream>
#include "src/display_device_policy.h"
#include "src/framegen_policy.h"
namespace rtsp_stream {
 struct launch_session_t : display_device::policy::session_t {};
''' + refresh + r'''
}
namespace policy = display_device::policy;
''' + adapter + r'''
int main() {
 policy::video_config_t video;
 video.dd.configuration_option = policy::video_config_t::dd_t::config_option_e::ensure_active;
 video.dd.resolution_option = policy::video_config_t::dd_t::resolution_option_e::automatic;
 video.dd.refresh_rate_option = policy::video_config_t::dd_t::refresh_rate_option_e::automatic;
 int count = 0;
 auto check = [&](rtsp_stream::launch_session_t session, unsigned int millihz) {
   const auto result = policy::parse_configuration(video, to_policy_session(session));
   const auto *config = std::get_if<policy::configuration_t>(&result);
   assert(config && config->m_refresh_rate);
   assert((*config->m_refresh_rate == policy::rational_t{millihz, 1000}));
   ++count;
 };
 rtsp_stream::launch_session_t session;
 session.width = 2560;
 session.height = 1440;
 session.fps = SCALED ? 60000 : 60;
 check(session, 60000);
 session.fps = SCALED ? 59940 : 60;
 check(session, SCALED ? 59940 : 60000);
 session.client_display_refresh_millihz = 59940;
 check(session, 59940);
 session.framegen_refresh_rate = 240;
 check(session, 59940); // Explicit client rate keeps precedence over rounded FG.
 session.framegen_refresh_millihz = 239760;
 check(session, 239760);
 session.client_display_refresh_millihz = 0;
 session.framegen_refresh_millihz.reset();
 check(session, 240000);
 session.framegen_refresh_rate.reset();
 session.fps = SCALED ? 60000 : 60;
 video.dd.refresh_rate_option = policy::video_config_t::dd_t::refresh_rate_option_e::manual;
 video.dd.manual_refresh_rate = "50";
 check(session, 50000);
 session.client_display_mode_override = true;
 check(session, 60000); // Client override retains priority over manual rate.
 std::cout << count << " adapter refresh cases passed (scaled=" << SCALED << ")\n";
}
'''
with tempfile.TemporaryDirectory(prefix='display-policy-adapter-') as temporary:
    directory = pathlib.Path(temporary)
    source = directory / 'test.cpp'
    source.write_text(program)
    binary = directory / 'test'
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers', f'-DSCALED={int(scaled)}', '-I', str(root), str(source), str(root / 'src/display_device_policy.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
