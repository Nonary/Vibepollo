#!/usr/bin/env python3
"""Exercise production Guide config resolution, allocation and held input together.

The backend/mail/task boundaries are deterministic fakes. Config numeric parsing,
base snapshot, client layer resolution, input allocation, and packet handlers are
compiled from production sources. --input-source accepts the v1 candidate as a
negative control for its process-global allocation.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

import test_disconnect_release as packet_harness
from test_delayed_mouse_release import ROOT, function, production_prelude


def config_code(source):
    signatures = (
        'void int_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, int &input)',
        'void int_between_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, int &input, const std::pair<int, int> &range)',
        'void update_base_session_input_config_snapshot(const std::unordered_map<std::string, std::string> &vars) {',
        '::input::guide_button_config_t guide_button_config_for_session(',
    )
    return ('using namespace std::literals;\nnamespace config {\nstd::mutex g_session_input_config_mutex;\n'
            'std::unordered_map<std::string, std::string> g_base_guide_button_vars;\n' +
            '\n'.join(function(source, signature) for signature in signatures) + '\n}\n')


MAIL = r'''
using input::guide_button_config_t;
namespace input { struct touch_port_t {}; }
namespace platf { using gamepad_feedback_msg_t = int; }
namespace mail { constexpr int touch_port = 0, gamepad_feedback = 1; }
namespace safe {
  struct mail_raw_t {
    template<class T> int event(int) { return 0; }
    template<class T> int queue(int) { return 0; }
  };
  using mail_t = std::shared_ptr<mail_raw_t>;
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
bool held(std::shared_ptr<input_t> &input, int slot = 0) {
  const auto id = input->gamepads[slot].id;
  for (auto it = platf::gamepad_events.rbegin(); it != platf::gamepad_events.rend(); ++it)
    if (it->first == id) return bool(it->second.buttonFlags & platf::HOME);
  std::abort();
}
using vars_t = std::unordered_map<std::string, std::string>;
int main(int argc, char **argv) {
  assert(argc == 2);
  std::string test = argv[1];
  // File + CLI snapshot is independent of A's process-wide runtime settings.
  config::update_base_session_input_config_snapshot({{"guide_button_key", "123"}, {"guide_button_controller", "2"}});
  config::input.guide_button_key = 124; config::input.guide_button_controller = 0;
  auto mail = std::make_shared<safe::mail_raw_t>();
  if (test == "distinct_concurrent_clients") {
    vars_t app {{"guide_button_key", "122"}, {"guide_button_controller", "3"}};
    auto a = session_alloc(mail, config::guide_button_config_for_session(app, {{"guide_button_key", "124"}, {"guide_button_controller", "0"}}));
    auto b = session_alloc(mail, config::guide_button_config_for_session(app, {{"guide_button_key", "125"}, {"guide_button_controller", "1"}}));
    assert(a->guide_button_key == 124 && a->guide_button_controller == 0);
    assert(b->guide_button_key == 125 && b->guide_button_controller == 1);
    connect(a, 0, 3); connect(a, 1, 3); connect(b, 0, 3); connect(b, 1, 3);
    key(a, 124); key(b, 125); assert(held(a) && !held(a, 1) && !held(b) && held(b, 1));
    key(a, 124, true); assert(!held(a) && held(b, 1));
    key(b, 125, true); assert(!held(b, 1) && platf::keyboard_events.empty());
  } else if (test == "disabled_client_does_not_inherit_owner") {
    auto b = session_alloc(mail, config::guide_button_config_for_session({}, {{"guide_button_key", "0"}}));
    connect(b, 0, 1); key(b, 124); key(b, 124, true);
    assert(b->guide_button_key == 0 && !held(b));
    assert(platf::keyboard_events.size() == 2);
  } else if (test == "unconfigured_client_inherits_base") {
    auto b = session_alloc(mail, config::guide_button_config_for_session({}, {}));
    assert(b->guide_button_key == 123 && b->guide_button_controller == 2);
    connect(b, 2, 4); key(b, 123); assert(held(b, 2)); key(b, 123, true); assert(!held(b, 2));
  } else if (test == "app_layer_and_partial_client") {
    auto b = session_alloc(mail, config::guide_button_config_for_session({{"guide_button_key", "125"}, {"guide_button_controller", "1"}}, {{"guide_button_controller", "0"}}));
    assert(b->guide_button_key == 125 && b->guide_button_controller == 0);
  } else if (test == "numeric_parser_bounds_and_encoding") {
    auto v = config::guide_button_config_for_session({{"guide_button_key", "\"0x7d\""}}, {{"guide_button_controller", "15"}});
    assert(v.key == 125 && v.controller == 15);
    v = config::guide_button_config_for_session({{"guide_button_key", "125"}}, {{"guide_button_key", "256"}, {"guide_button_controller", "16"}});
    assert(v.key == 0 && v.controller == 0);
    v = config::guide_button_config_for_session({}, {{"guide_button_key", "-1"}, {"guide_button_controller", "-1"}});
    assert(v.key == 0 && v.controller == 0);
  } else if (test == "held_settings_survive_base_reload") {
    auto a = session_alloc(mail, config::guide_button_config_for_session({}, {{"guide_button_key", "124"}, {"guide_button_controller", "0"}}));
    connect(a, 0, 1); key(a, 124); assert(held(a));
    config::update_base_session_input_config_snapshot({{"guide_button_key", "125"}, {"guide_button_controller", "1"}});
    auto b = session_alloc(mail, config::guide_button_config_for_session({}, {}));
    connect(b, 1, 2); key(b, 125); assert(held(a) && held(b, 1));
    key(a, 124, true); assert(!held(a) && held(b, 1)); key(b, 125, true); assert(!held(b, 1));
  } else if (test == "persist_flag_and_default_disabled") {
    config::update_base_session_input_config_snapshot({});
    auto b = session_alloc(mail, config::guide_button_config_for_session({}, {}), true);
    assert(b->persist_gamepads && b->guide_button_key == 0 && b->guide_button_controller == 0);
  } else return 2;
}
'''


def input_harness(source):
    """Compile actual allocation/handlers with fake mail, worker and OS output.

    Geometry refresh is outside this harness's Guide/session-config contract,
    so the extracted snippets use inert lifecycle hooks while retaining the
    production atomic worker-wake coalescing policy.
    """
    prelude = production_prelude(source)
    # Use the real utility numeric helpers rather than duplicating parsing.
    prelude = '#include "src/utility.h"\n' + prelude
    prelude = re.sub(r'namespace util::endian \{.*?\n\}', '', prelude, flags=re.S)
    prelude = prelude.replace('std::list<int> input_queue;', 'std::list<std::vector<uint8_t>> input_queue;\n  std::atomic_bool input_queue_task_scheduled {false};\n  int feedback_queue = 0;\n  bool persist_gamepads = false;')
    prelude = prelude.replace('  const int guide_button_key;', '''  input_t(int, int, input::guide_button_config_t guide_button): input_t(guide_button) {}
  input_t(int, int): input_t() {}
  const int guide_button_key;''')
    prelude = prelude.replace('void push(std::function<void()> f) { tasks.push_back(std::move(f)); }', 'template<class F, class... A> void push(F f, A... a) { tasks.push_back([=] { f(a...); }); }')
    extra = re.sub(r'namespace util \{.*?\n\}', '', packet_harness.EXTRA, flags=re.S)
    signatures = (
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
        'std::shared_ptr<input_t> alloc(safe::mail_t mail,',
        'void update_guide_button_permissions(const std::shared_ptr<input_t> &input,',
    )
    crypto_source = (ROOT / 'src/crypto.h').read_text()
    permissions = crypto_source[crypto_source.index('  enum class PERM :'):crypto_source.index('  struct command_entry_t')]
    prelude += '\nnamespace crypto {\n' + permissions + '\n}\n'
    geometry_support = r'''
namespace geometry {
  inline bool request_serialized_worker_wake(std::atomic_bool &scheduled) noexcept {
    return !scheduled.exchange(true, std::memory_order_acq_rel);
  }
}
void refresh_input_geometry(const std::shared_ptr<input_t> &) {}
void start_input_geometry_refresh(const std::shared_ptr<input_t> &) {}
'''
    code = '\n'.join(function(source, signature) for signature in signatures if signature in source)
    return prelude + MAIL + config_code((ROOT / 'src/config.cpp').read_text()) + extra + geometry_support + code


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input-source', type=Path, default=ROOT / 'src/input.cpp')
    args = parser.parse_args()
    code = input_harness(args.input_source.read_text())
    modern = 'guide_button_config_t guide_button, bool persist_gamepads' in code
    adapter = '''std::shared_ptr<input_t> session_alloc(safe::mail_t mail, guide_button_config_t guide_button, bool persist = false) {
      return alloc(mail, ''' + ('guide_button, persist' if modern else 'persist') + ''');
    }\n'''
    cases = re.findall(r'test == "([^"]+)"', TESTS)
    failed = []
    with tempfile.TemporaryDirectory(prefix='guide-session-') as temp:
        cpp, binary = Path(temp) / 'test.cpp', Path(temp) / 'test'
        cpp.write_text(code + adapter + TESTS)
        subprocess.run(['c++', '-I', str(ROOT), '-std=c++20', '-Wall', '-Wextra', '-Wno-sign-compare', '-Wno-missing-field-initializers', str(cpp), '-o', str(binary)], check=True)
        for case in cases:
            result = subprocess.run([str(binary), case], capture_output=True, text=True)
            print(f'{"PASS" if result.returncode == 0 else "FAIL"}: {case}')
            if result.returncode:
                failed.append(case)
                print(result.stderr)
    print(f'{len(cases) - len(failed)}/{len(cases)} passed')
    return bool(failed)


if __name__ == '__main__':
    raise SystemExit(main())
