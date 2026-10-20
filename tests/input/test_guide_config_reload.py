#!/usr/bin/env python3
"""Run the real two-PATCH handler and Guide removal interleaving without live I/O.

HTTP authentication/response, config-file I/O and the full-config apply boundary
are fakes. The real PATCH handler, hot-apply eligibility, config parser/bool
parser, and production input handlers execute. The apply adapter rereads the
whole stored file, matching apply_config_now's source-verified contract, and
installs only the controller boolean relevant to this test.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

import test_disconnect_release as packets
from test_delayed_mouse_release import ROOT, function, production_prelude

HTTP = r'''
namespace file_handler {
  std::string persisted;
  std::string read_file(const char *) { return persisted; }
  void write_file(const char *, const std::string &value) { persisted = value; }
}
'''
APPLY = r'''
namespace config {
  struct { std::string config_file = "in-memory"; } sunshine;
  int applies = 0;
  bool deferred = false;
  void mark_deferred_reload() { deferred = true; }
  void apply_config_now() {
    auto vars = parse_config(file_handler::read_file(sunshine.config_file.c_str()));
    bool_f(vars, "controller", input.controller);
    ++applies;
  }
}
struct response_t { nlohmann::json value; };
struct request_t { std::stringstream content; };
using resp_https_t = std::shared_ptr<response_t>;
using req_https_t = std::shared_ptr<request_t>;
bool validateContentType(resp_https_t, req_https_t, const char *) { return true; }
bool authenticate(resp_https_t, req_https_t) { return true; }
void print_req(req_https_t) {}
void bad_request(resp_https_t response, req_https_t, const char *message) { std::cerr << message; std::abort(); }
void send_response(resp_https_t response, nlohmann::json value) { response->value = std::move(value); }
bool has_active_stream_sessions() { return true; }
'''
TEST = r'''
void key(std::shared_ptr<input_t> &input, bool release = false) {
  keyboard_packet p {{release ? KEY_UP_EVENT_MAGIC : 0}, 0, 124, 0}; passthrough(input, &p);
}
void packet(std::shared_ptr<input_t> &input, int slot, int mask, int flags = 0) {
  controller_packet p {}; p.controllerNumber = slot; p.activeGamepadMask = mask; p.buttonFlags = flags;
  passthrough(input, &p);
}
auto patch(const char *json) {
  auto response = std::make_shared<response_t>(); auto request = std::make_shared<request_t>();
  request->content << json; patchConfig(response, request); return response->value;
}
int main() {
  config::input.guide_button_key = 124;
  auto a = std::make_shared<input_t>(), b = std::make_shared<input_t>();
  packet(a, 0, 3, platf::BACK); packet(a, 1, 3); packet(b, 0, 1, platf::HOME);
  key(a); assert(a->gamepads[0].guide_key_down && !task_pool.timers.empty());
  file_handler::persisted = "controller = true\n";
  auto first = patch(R"({"controller":false})");
  assert(first["deferred"] && !first["appliedNow"] && config::input.controller && config::applies == 0);
  assert(config::parse_config(file_handler::persisted).at("controller") == "false");
  auto second = patch(R"({"realtime_stats_enabled":true})");
  assert(second["appliedNow"] && !second["deferred"] && !config::input.controller && config::applies == 1);
  assert(config::parse_config(file_handler::persisted).at("controller") == "false");
  auto b_id = b->gamepads[0].id;
  packet(a, 1, 2, platf::HOME); // remove held target via surviving slot after reload
  assert(a->gamepads[0].id == -1 && !a->gamepads[0].guide_key_down && a->guide_key_pressed);
  assert(task_pool.timers.empty() && a->gamepads[1].id >= 0 && b->gamepads[0].id == b_id);
  assert(platf::freed_gamepads.size() == 1);
  auto count = platf::gamepad_events.size();
  packet(a, 0, 3, platf::HOME); // disabled forwarding cannot allocate or emit state
  assert(a->gamepads[0].id == -1 && count == platf::gamepad_events.size());
  config::input.controller = true;
  packet(a, 0, 3); key(a); assert(!a->gamepads[0].guide_key_down);
  key(a, true); key(a); assert(a->gamepads[0].guide_key_down);
  key(a, true); assert(!a->gamepads[0].guide_key_down && platf::keyboard_events.empty());
  std::cout << "PASS: two production PATCH requests, disabled removal, canceled Back timer, foreign-slot inventory, no disabled forwarding, independent client and held-key reconnect latch\n";
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'src/input.cpp')
    parser.add_argument('--include-dir', type=Path, action='append', default=[], help='Additional dependency includes, such as nlohmann/json.hpp')
    args = parser.parse_args()
    source = args.source.read_text()
    prelude = production_prelude(source)
    prelude = '#include "src/utility.h"\n#include "src/config_key.h"\n#include <nlohmann/json.hpp>\n#include <set>\n#include <sstream>\nusing namespace std::literals;\n' + prelude
    prelude = re.sub(r'namespace util::endian \{.*?\n\}', '', prelude, flags=re.S)
    prelude = prelude.replace('std::list<int> input_queue;', 'std::list<std::vector<uint8_t>> input_queue;\n  std::atomic_bool input_queue_task_scheduled {false};\n  int feedback_queue = 0;\n  bool persist_gamepads = false;')
    extra = re.sub(r'namespace util \{.*?\n\}', '', packets.EXTRA, flags=re.S)
    config = (ROOT / 'src/config.cpp').read_text()
    parsers = config[config.index('  bool endline(char ch)'):config.index('  void string_f(')]
    parsers += '\n'.join(function(config, signature) for signature in (
        'void string_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::string &input)',
        'bool to_bool(std::string &boolean)',
        'void bool_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, bool &input)',
    ))
    http = (ROOT / 'src/confighttp.cpp').read_text()
    endpoint = '\n'.join(function(http, signature) for signature in (
        'bool is_rtx_hdr_live_key(std::string_view key)',
        'void normalize_adapter_config_patch(nlohmann::json &patch_object)',
        'bool can_hot_apply_during_session(const std::set<std::string> &keys)',
        'void patchConfig(resp_https_t response, req_https_t request)',
    ))
    handlers = '\n'.join(function(source, signature) for signature in (
        'void send_gamepad_state(const gamepad_t &gamepad)',
        'short map_keycode(short keycode)',
        'bool passthrough_guide_key(input_t &input, uint16_t key_code, bool release',
        'inline void update_shortcutFlags(int *flags, short keyCode, bool release)',
        'bool is_modifier(uint16_t keyCode)',
        'uint16_t host_keycode(uint16_t key)',
        'void refresh_shortcut_flags(input_t &input)',
        'void release_key(const held_key_t &key)',
        'void send_key_and_modifiers(const held_key_t &key, int shortcut_flags)',
        'void repeat_key(std::shared_ptr<input_t> input, uint16_t key_code)',
        'void passthrough(std::shared_ptr<input_t> &input, PNV_KEYBOARD_PACKET packet',
        'void free_gamepad(platf::input_t &platf_input, int id)',
        'void reset_gamepad(gamepad_t &gamepad)',
        'void passthrough(std::shared_ptr<input_t> &input, PNV_MULTI_CONTROLLER_PACKET packet)',
    ))
    with tempfile.TemporaryDirectory(prefix='guide-reload-') as temp:
        cpp, binary = Path(temp) / 'test.cpp', Path(temp) / 'test'
        cpp.write_text(prelude + extra + HTTP + '\nnamespace config {\n' + parsers + '\n}\n' + APPLY + endpoint + handlers + TEST)
        includes = [item for path in [ROOT] + args.include_dir for item in ['-I', str(path)]]
        subprocess.run(['c++', *includes, '-std=c++20', '-Wall', '-Wextra', '-Wno-sign-compare', '-Wno-missing-field-initializers', str(cpp), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)]).returncode


if __name__ == '__main__':
    raise SystemExit(main())
