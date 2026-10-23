#!/usr/bin/env python3
"""Compile production RTSP snapshot, capture plan and stream allocator.

Platform/capture/crypto/input adapters reuse the Guide handoff fixture. This
checks value ownership and exact-output admission without a live RTSP server.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

from test_delayed_mouse_release import ROOT, function
from test_guide_session_config import input_harness
from test_guide_rtsp_handoff import STUBS, STREAM_TYPES, declaration

TESTS = r'''
int main() {
  for (const auto &profile : {"profile-a", "profile-b"}) {
    rtsp_stream::launch_session_t pending {};
    pending.id = 41;
    pending.client_uuid = "paired-owner";
    pending.role = remote_session::role_e::monitor;
    pending.role_generation = 41;
    pending.monitor_profile_uuid = profile;
    pending.remote_capture_output = "exact-client-output";
    auto startup = pending.clone_for_startup();
    pending.monitor_profile_uuid = "mutated-pending-profile";
    pending.role_generation = 42;
    pending.remote_capture_output = "wrong-output";
    assert(startup->monitor_profile_uuid == profile);
    assert(startup->role_generation == 41);
    stream::config_t negotiated {};
    auto stream = stream::session::alloc(negotiated, *startup);
    assert(stream->monitor_profile_uuid == profile);
    assert(stream->remote_role == remote_session::role_e::monitor);
    assert(stream->remote_role_generation == 41);
    assert(stream->device_uuid == "paired-owner");
    assert(stream->config.monitor.capture_source == video::capture_source_e::exact_output);
    assert(stream->config.monitor.capture_output == "exact-client-output");
    startup->monitor_profile_uuid = "mutated-startup-profile";
    assert(stream->monitor_profile_uuid == profile);
  }
  rtsp_stream::launch_session_t missing {};
  missing.role = remote_session::role_e::monitor;
  missing.monitor_profile_uuid = "profile-without-output";
  stream::config_t negotiated {};
  bool rejected = false;
  try { (void) stream::session::alloc(negotiated, missing); }
  catch (const std::runtime_error &) { rejected = true; }
  assert(rejected);
  std::cout << "PASS: two profile identities survive pending/startup/stream copies\n"
            << "PASS: paired-client authorization identity stays separate\n"
            << "PASS: exact output survives pending mutation\n"
            << "PASS: missing exact output fails without fallback\n";
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rtsp-source', type=Path, default=ROOT / 'src/rtsp.cpp')
    parser.add_argument('--stream-source', type=Path, default=ROOT / 'src/stream.cpp')
    parser.add_argument('--save-cpp', type=Path)
    args = parser.parse_args()
    launch = declaration((ROOT / 'src/rtsp.h').read_text(), '  struct launch_session_t {', '\n  struct client_disconnect_result_t')
    snapshot = function(args.rtsp_source.read_text(), 'std::shared_ptr<launch_session_t> launch_session_t::clone_for_startup() const')
    stream_config = declaration((ROOT / 'src/stream.h').read_text(), '  struct config_t {', '\n  namespace session {')
    allocate = function(args.stream_source.read_text(), 'std::shared_ptr<session_t> alloc(config_t &config,')
    capture = function((ROOT / 'src/remote_session.cpp').read_text(), 'capture_plan_t capture_plan(')
    fake_capture = function(STUBS, 'capture_plan_t capture_plan(')
    stubs = STUBS.replace(fake_capture, capture)
    crypto_types = declaration((ROOT / 'src/crypto.h').read_text(), '  struct command_entry_t', '\n  struct named_cert_t')
    code = (input_harness((ROOT / 'src/input.cpp').read_text()) + '\nnamespace crypto {\n' + crypto_types + '\n}\n' + stubs +
            'namespace rtsp_stream {\n' + launch + snapshot + '\n}\n' +
            'namespace stream {\n' + stream_config + '\n}\n' + STREAM_TYPES +
            'namespace stream::session {\n' + allocate + '\n}\n' + TESTS)
    if args.save_cpp:
        args.save_cpp.write_text(code)
    with tempfile.TemporaryDirectory(prefix='monitor-profile-handoff-') as temp:
        cpp, binary = Path(temp) / 'test.cpp', Path(temp) / 'test'
        cpp.write_text(code)
        subprocess.run(['c++', '-I', str(ROOT), '-std=c++20', '-Wno-missing-field-initializers', str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
