#!/usr/bin/env python3
"""Exercise production RTSP snapshot -> stream allocation -> Guide input behavior.

The entire production launch-session declaration, startup snapshot and stream
allocator are compiled. stream::session::start executes through input allocation,
then stops at its broadcast/network boundary. Input allocation, numeric resolver
and packet handlers are production code; external mail/OS/crypto/display and
capture types are deterministic fakes. --rtsp-source selects a missing-copy red
control. This is a function-boundary regression, not a linked RTSP server test.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

from test_delayed_mouse_release import ROOT, function
from test_guide_session_config import input_harness


# These types let the complete stream allocator execute without a real capture,
# display owner, history database, crypto operation, mail bus or device backend.
STUBS = r'''
#include "src/app_audio_termination.h"
#include <cmath>
#include <list>
#include <map>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <functional>
namespace display_target { struct descriptor_t {}; }
namespace input { using input_t = ::input_t; using ::alloc; using ::update_guide_button_permissions; }
namespace crypto {
  using aes_t = std::vector<unsigned char>;
  namespace cipher {
    struct gcm_t { gcm_t() = default; gcm_t(const aes_t &, bool) {} };
    struct cbc_t { cbc_t() = default; cbc_t(const aes_t &, bool) {} };
    constexpr int round_to_pkcs7_padded(int n) { return n; }
  }
}
namespace config {
  struct video_t {
    enum class virtual_display_mode_e {};
    enum class virtual_display_layout_e {};
    struct dd_t { enum class config_option_e {}; };
  };
  struct { bool remote_monitor_mute_audio = false; } video;
}
namespace remote_session {
  enum class role_e { game, input, monitor };
  enum class capture_source_e { synthetic_black, exact_output, invalid, active_output };
  struct capture_plan_t { capture_source_e source; std::optional<std::string> output; };
  bool exact_target_still_valid(const std::shared_ptr<void> &) { return true; }
  bool uses_audio(role_e, bool) { return false; }
  capture_plan_t capture_plan(role_e, const std::optional<std::string> &) {
    return {capture_source_e::active_output, {}};
  }
}
namespace video {
  enum class capture_source_e { synthetic_black, exact_output, active_output };
  struct config_t {
    int framerate = 60, encodingFramerate = 60000;
    bool input_only = false;
    capture_source_e capture_source {};
    std::optional<std::string> capture_output;
    std::function<bool()> capture_target_valid;
  };
  struct hdr_info_t {};
}
namespace audio { struct config_t {}; }
namespace uuid_util {
  struct uuid_t {
    static uuid_t generate() { return {}; }
    std::string string() { return "history-id"; }
  };
}
namespace proc {
  struct app_t { uint64_t normal_vdd_identity_token = 0; std::string client_uuid; };
  struct { app_t active_session_guard() { return {}; } } proc;
}
namespace remote_display_topology {
  struct fake_t {
    std::shared_ptr<void> retain_normal_game_capture(const std::string &, uint64_t) { return {}; }
    bool normal_game_release_pending() { return false; }
  };
  fake_t &instance() { static fake_t value; return value; }
}
namespace mail {
  constexpr int shutdown = 2, hdr = 3, idr = 4, invalidate_ref_frames = 5, dynamic_bitrate = 6;
}
constexpr int SS_ENC_VIDEO = 1, RTPA_TOTAL_SHARDS = 4;
namespace stream {
  enum class state_e { STOPPED };
}
'''

STREAM_TYPES = r'''
namespace stream {
  struct session_t {
    int shutdown_event = 0;
    uint32_t launch_session_id = 0;
    std::shared_ptr<void> display_power_guard, audio_context_guard, normal_display_capture;
    std::optional<display_target::descriptor_t> exact_monitor_target;
    std::shared_ptr<void> exact_target_hold;
    std::string device_name, device_uuid, history_uuid;
    config_t config {};
    int stream_fps = 0, stream_fps_scaled = 0;
    crypto::PERM permission {};
    std::list<crypto::command_entry_t> do_cmds, undo_cmds;
    uint32_t client_display_refresh_millihz = 0;
    bool secondary_game_client = false, input_only = false, audio_disabled = false;
    remote_session::role_e remote_role {};
    uint64_t remote_role_generation = 0;
    std::string monitor_profile_uuid;
    struct { bool active = false; } virtual_display;
    struct {
      uint32_t connect_data = 0;
      int feedback_queue = 0, hdr_queue = 0;
      crypto::aes_t legacy_input_enc_iv;
      crypto::cipher::gcm_t cipher;
      void *peer = nullptr;
    } control;
    struct {
      int idr_events = 0, invalidate_ref_frames_events = 0, bitrate_events = 0, lowseq = 0;
      std::string ping_payload;
      crypto::cipher::gcm_t cipher;
      uint64_t gcm_iv_counter = 0;
    } video;
    struct {
      util::buffer_t<char> shards;
      util::buffer_t<uint8_t *> shards_p;
      struct {
        struct { int header, packetType, timestamp, ssrc; } rtp;
        struct { int payloadType, ssrc; } fecHeader;
      } fec_packet {};
      crypto::cipher::cbc_t cipher;
      std::string ping_payload;
      uint32_t avRiKeyId = 0, sequenceNumber = 0, timestamp = 0;
    } audio;
    std::atomic<state_e> state {};
    safe::mail_t mail;
    std::shared_ptr<input::input_t> input;
  };
}
'''

TESTS = r'''
void key(std::shared_ptr<input_t> &input, short code, bool release = false) {
  keyboard_packet p {{release ? KEY_UP_EVENT_MAGIC : 0}, 0, code, 0}; passthrough(input, &p);
}
void connect(std::shared_ptr<input_t> &input, int slot, int mask, int flags = 0) {
  controller_packet p {}; p.controllerNumber = slot; p.activeGamepadMask = mask; p.buttonFlags = flags;
  passthrough(input, &p);
}
bool held(const std::shared_ptr<input_t> &input, int slot) {
  const auto id = input->gamepads[slot].id;
  for (auto it = platf::gamepad_events.rbegin(); it != platf::gamepad_events.rend(); ++it)
    if (it->first == id) return bool(it->second.buttonFlags & platf::HOME);
  std::abort();
}
std::shared_ptr<stream::session_t> start(input::guide_button_config_t binding) {
  rtsp_stream::launch_session_t pending {};
  pending.perm = crypto::PERM::_all_inputs; pending.id = 42; pending.client_uuid = "joining-client"; pending.guide_button = binding;
  auto startup = pending.clone_for_startup();
  // ANNOUNCE captures config by value and passes its startup snapshot to alloc.
  // Poison this config so a missing launch->stream adapter cannot pass by default.
  stream::config_t negotiated {}; negotiated.guide_button = {121, 7};
  auto stream = stream::session::alloc(negotiated, *startup);
  assert(stream::session::start(*stream, "127.0.0.1") == -1);
  std::cout << "pending=" << pending.guide_button.key << '/' << pending.guide_button.controller
            << " startup=" << startup->guide_button.key << '/' << startup->guide_button.controller
            << " stream=" << stream->config.guide_button.key << '/' << stream->config.guide_button.controller
            << " input=" << stream->input->guide_button_key << '/' << stream->input->guide_button_controller << std::endl;
  return stream;
}
int main(int argc, char **argv) {
  assert(argc == 2);
  const std::string test = argv[1];
  config::input.guide_button_key = 120; config::input.guide_button_controller = 6;
  if (test == "nondefault_target_and_no_keyboard_leak") {
    auto stream = start({124, 1}); auto a = stream->input;
    connect(a, 0, 3); connect(a, 1, 3);
    key(a, 124); assert(held(a, 1) && !held(a, 0));
    key(a, 124, true); assert(!held(a, 1) && platf::keyboard_events.empty());
    assert(a->guide_button_key == 124 && a->guide_button_controller == 1);
  } else if (test == "distinct_concurrent_launches") {
    auto first = start({124, 0}), second = start({125, 1});
    auto a = first->input, b = second->input;
    connect(a, 0, 3); connect(a, 1, 3); connect(b, 0, 3); connect(b, 1, 3);
    key(a, 124); key(b, 125);
    assert(held(a, 0) && !held(a, 1) && !held(b, 0) && held(b, 1));
    key(a, 124, true); assert(!held(a, 0) && held(b, 1));
    key(b, 125, true); assert(!held(b, 1) && platf::keyboard_events.empty());
  } else if (test == "explicit_disabled_overrides_runtime_and_negotiation") {
    auto stream = start({0, 1}); auto a = stream->input;
    connect(a, 1, 2); key(a, 120); key(a, 120, true);
    assert(a->guide_button_key == 0 && a->guide_button_controller == 1 && !held(a, 1));
    assert(platf::keyboard_events.size() == 2);
  } else if (test == "resolved_app_client_layers_reach_startup") {
    config::update_base_session_input_config_snapshot({{"guide_button_key", "123"}, {"guide_button_controller", "2"}});
    auto binding = config::guide_button_config_for_session(
      {{"guide_button_key", "125"}, {"guide_button_controller", "3"}},
      {{"guide_button_key", "124"}, {"guide_button_controller", "1"}});
    auto stream = start(binding); auto a = stream->input;
    connect(a, 1, 2); key(a, 124); assert(held(a, 1));
    config::update_base_session_input_config_snapshot({{"guide_button_key", "126"}});
    key(a, 124, true); assert(!held(a, 1) && platf::keyboard_events.empty());
  } else if (test == "snapshot_value_survives_pending_mutation") {
    rtsp_stream::launch_session_t pending {}; pending.perm = crypto::PERM::_all_inputs; pending.guide_button = {124, 1};
    auto startup = pending.clone_for_startup();
    pending.guide_button = {125, 2};
    stream::config_t negotiated {}; auto stream = stream::session::alloc(negotiated, *startup);
    assert(stream::session::start(*stream, "127.0.0.1") == -1);
    auto a = stream->input; connect(a, 1, 2); key(a, 124); assert(held(a, 1));
    key(a, 124, true); assert(!held(a, 1));
  } else if (test == "target_snapshot_preserves_input_permissions_commands_audio") {
    rtsp_stream::launch_session_t pending {};
    pending.guide_button = {124, 1}; pending.input_only = true; pending.gcpersist = true;
    pending.perm = crypto::PERM::_all_inputs; pending.fps = 144000;
    pending.client_do_cmds.push_back({"client-start", false});
    pending.client_undo_cmds.push_back({"client-stop", true});
    pending.id = 43; pending.unique_id = "live-pending-owner";
    pending.audio_context_guard = std::make_shared<int>(9);
    pending.display_power_guard = std::make_shared<int>(10);
    auto audio_owner = pending.audio_context_guard, display_owner = pending.display_power_guard;
    auto startup = pending.clone_for_startup();
    if (fixture_admitted_runtime_holds) {
      // Current startup snapshots are metadata-only until an admitted worker
      // revalidates the exact live pending identity under the real registry body.
      assert(!startup->audio_context_guard && !startup->display_power_guard);
      auto pending_alias = std::shared_ptr<rtsp_stream::launch_session_t>(&pending, [](auto *) {});
      auto stale = pending.clone_for_startup(); stale->unique_id = "stale-pending-owner";
      assert(!fixture_retain_pending_holds(stale, pending_alias));
      assert(!stale->audio_context_guard && !stale->display_power_guard);
      assert(!fixture_retain_pending_holds(startup, pending_alias, true));
      assert(!startup->audio_context_guard && !startup->display_power_guard);
      assert(fixture_retain_pending_holds(startup, pending_alias));
    }
    assert(startup->audio_context_guard == audio_owner && startup->display_power_guard == display_owner);
    assert(startup->input_only && startup->gcpersist && startup->perm == pending.perm);
    assert(pending.client_do_cmds.size() == 1 && pending.client_undo_cmds.size() == 1);
    stream::config_t negotiated {}; auto stream = stream::session::alloc(negotiated, *startup);
    assert(stream->input_only && stream->config.monitor.input_only && stream->audio_disabled);
    assert(stream->permission == pending.perm && stream->stream_fps == 144 && stream->stream_fps_scaled == 144000);
    assert(stream->do_cmds.front().cmd == "client-start" && stream->undo_cmds.front().elevated);
    assert(startup->client_do_cmds.empty() && startup->client_undo_cmds.empty());
    assert(pending.client_do_cmds.size() == 1 && pending.client_undo_cmds.size() == 1);
    assert(stream->audio_context_guard == audio_owner && !startup->audio_context_guard);
    assert(pending.audio_context_guard == audio_owner && stream->display_power_guard == display_owner);
    assert(stream::session::start(*stream, "127.0.0.1") == -1);
    auto a = stream->input; assert(a->persist_gamepads);
    connect(a, 1, 2); key(a, 124); assert(held(a, 1)); key(a, 124, true);
    assert(!held(a, 1) && platf::keyboard_events.empty());
  } else if (test == "default_disabled_keyboard_passthrough") {
    auto stream = start({}); auto a = stream->input;
    connect(a, 0, 1); key(a, 124); key(a, 124, true);
    assert(a->guide_button_key == 0 && !held(a, 0));
    assert(platf::keyboard_events.size() == 2);
  } else return 2;
}
'''


def declaration(source, start, end):
    return source[source.index(start):source.index(end, source.index(start))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rtsp-source', type=Path, default=ROOT / 'src/rtsp.cpp')
    parser.add_argument('--stream-source', type=Path, default=ROOT / 'src/stream.cpp')
    parser.add_argument('--save-cpp', type=Path)
    args = parser.parse_args()
    launch = declaration((ROOT / 'src/rtsp.h').read_text(), '  struct launch_session_t {', '\n  struct client_disconnect_result_t')
    snapshot = function(args.rtsp_source.read_text(), 'std::shared_ptr<launch_session_t> launch_session_t::clone_for_startup() const')
    runtime_holds = r"""
constexpr bool fixture_admitted_runtime_holds = false;
bool fixture_retain_pending_holds(const std::shared_ptr<rtsp_stream::launch_session_t>&,
 const std::shared_ptr<rtsp_stream::launch_session_t>&, bool = false) { return false; }
"""
    rtsp_source = args.rtsp_source.read_text()
    if 'bool retain_pending_runtime_holds(' in rtsp_source:
        retain = function(rtsp_source, 'bool retain_pending_runtime_holds(')
        runtime_holds = r"""
constexpr bool fixture_admitted_runtime_holds = true;
namespace rtsp_stream {
struct fixture_pending_registry_t {
  std::mutex _launch_sessions_mutex;
  struct entry_t { std::shared_ptr<launch_session_t> session; std::chrono::steady_clock::time_point expires_at; };
  std::vector<entry_t> _launch_sessions;
""" + retain + r"""
};
}
bool fixture_retain_pending_holds(const std::shared_ptr<rtsp_stream::launch_session_t>& snapshot,
 const std::shared_ptr<rtsp_stream::launch_session_t>& pending, bool expired = false) {
 rtsp_stream::fixture_pending_registry_t registry;
 registry._launch_sessions.push_back({pending, std::chrono::steady_clock::now() + (expired ? -std::chrono::hours(1) : std::chrono::hours(1))});
 return registry.retain_pending_runtime_holds(snapshot);
}
"""
    stream_config = declaration((ROOT / 'src/stream.h').read_text(), '  struct config_t {', '\n  namespace session {')
    stream_source = args.stream_source.read_text()
    allocate = function(stream_source, 'std::shared_ptr<session_t> alloc(config_t &config,')
    # Execute the unchanged production start prefix, ending just before acquiring
    # the broadcast reference. No Guide transfer is recreated in this adapter.
    start = function(stream_source, 'int start(session_t &session, const std::string &addr_string)')
    start = start[:start.index('      session.broadcast_ref = broadcast.ref();')] + '      (void) addr_string; return -1;\n    }\n'
    crypto_source = (ROOT / 'src/crypto.h').read_text()
    crypto_types = declaration(crypto_source, '  struct command_entry_t', '\n  struct named_cert_t')
    code = (input_harness((ROOT / 'src/input.cpp').read_text()) + '\nnamespace crypto {\n' + crypto_types + '\n}\n' + STUBS +
            'namespace rtsp_stream {\n' + launch + snapshot + '\n}\n' +
            runtime_holds + 'namespace stream {\n' + stream_config + '\n}\n' + STREAM_TYPES +
            'namespace stream::session {\n' + allocate + start + '\n}\n' + TESTS)
    if args.save_cpp:
        args.save_cpp.write_text(code)
    cases = re.findall(r'test == "([^"]+)"', TESTS)
    failed = []
    with tempfile.TemporaryDirectory(prefix='guide-rtsp-handoff-') as temp:
        cpp, binary = Path(temp) / 'test.cpp', Path(temp) / 'test'
        cpp.write_text(code)
        subprocess.run(['c++', '-I', str(ROOT), '-std=c++20', '-Wall', '-Wextra', '-Wno-sign-compare', '-Wno-missing-field-initializers', str(cpp), '-o', str(binary)], check=True)
        for case in cases:
            result = subprocess.run([str(binary), case], capture_output=True, text=True)
            print(f'{"PASS" if result.returncode == 0 else "FAIL"}: {case}', flush=True)
            print(result.stdout, end='')
            if result.returncode:
                failed.append(case)
                print(result.stderr)
    print(f'{len(cases) - len(failed)}/{len(cases)} passed')
    return bool(failed)


if __name__ == '__main__':
    raise SystemExit(main())
