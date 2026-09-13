#!/usr/bin/env python3
"""Exercise production input handlers with recorded OS output and a deterministic worker.

No live keyboard, mouse, touch, or gamepad devices are used.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

from test_delayed_mouse_release import ROOT, production_prelude, function

EXTRA = r'''
using namespace std::literals;
struct controller_packet {
  short controllerNumber;
  short activeGamepadMask;
  uint16_t buttonFlags;
  uint16_t buttonFlags2;
  uint8_t leftTrigger, rightTrigger;
  int16_t leftStickX, leftStickY, rightStickX, rightStickY;
};
using PNV_MULTI_CONTROLLER_PACKET = controller_packet *;
namespace platf {
  struct gamepad_id_t { int globalIndex; uint8_t clientRelativeIndex; };
  struct gamepad_arrival_t {};
  int alloc_gamepad(int, gamepad_id_t, gamepad_arrival_t, int) { return 0; }
}
int alloc_id(std::bitset<16> &mask) {
  for (int i = 0; i < 16; ++i) if (!mask[i]) { mask[i] = true; return i; }
  return -1;
}
namespace util {
  struct hex_t { std::string to_string_view() { return "0"; } };
  template<class T> hex_t hex(T) { return {}; }
}
#include <optional>
#include <cstring>
constexpr int KEY_DOWN_EVENT_MAGIC = 0x03, UTF8_TEXT_EVENT_MAGIC = 0x17;
constexpr int MOUSE_MOVE_REL_MAGIC_GEN5 = 0x07, MOUSE_MOVE_ABS_MAGIC = 0x05;
constexpr int MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5 = 0x08, SCROLL_MAGIC_GEN5 = 0x0a;
constexpr int MULTI_CONTROLLER_MAGIC_GEN5 = 0x0c;
constexpr int SS_HSCROLL_MAGIC = 0x55000001, SS_TOUCH_MAGIC = 0x55000002;
constexpr int SS_PEN_MAGIC = 0x55000003, SS_CONTROLLER_ARRIVAL_MAGIC = 0x55000004;
constexpr int SS_CONTROLLER_TOUCH_MAGIC = 0x55000005, SS_CONTROLLER_MOTION_MAGIC = 0x55000006;
constexpr int SS_CONTROLLER_BATTERY_MAGIC = 0x55000007;
// Models validation success; this suite tests enqueue permissions/lifetime, not wire parsing.
struct validated_input_packet_t { uint32_t magic; };
std::optional<validated_input_packet_t> validate_packet(const std::vector<uint8_t> &data) {
  if (data.size() != sizeof(uint32_t)) return std::nullopt;
  uint32_t magic;
  std::memcpy(&magic, data.data(), sizeof(magic));
  return validated_input_packet_t{magic};
}
std::vector<uint8_t> packet_bytes(uint32_t magic) {
  std::vector<uint8_t> data(sizeof(magic));
  std::memcpy(data.data(), &magic, sizeof(magic));
  return data;
}
void passthrough_next_message(std::shared_ptr<input_t>) {}
'''

TESTS = r'''
void key(std::shared_ptr<input_t> &input, short code, bool release = false, int mods = 0, int flags = 0) {
  keyboard_packet p {{release ? KEY_UP_EVENT_MAGIC : 0}, static_cast<char>(flags), code, static_cast<char>(mods)};
  passthrough(input, &p);
}
void button(std::shared_ptr<input_t> &input, int code, bool release = false) {
  button_packet p {{release ? MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5 : 0}, code};
  passthrough(input, &p);
}
void expect(std::initializer_list<platf::keyboard_event_t> wanted) {
  if (platf::keyboard_events != std::vector<platf::keyboard_event_t>(wanted)) {
    for (const auto &e : platf::keyboard_events) std::cerr << e.key << (e.release ? " up " : " down ") << e.flags << '\n';
    std::abort();
  }
}
void disconnect(std::shared_ptr<input_t> &input) { reset(input); task_pool.drain(); }
int main(int argc, char **argv) {
  assert(argc == 2);
  auto a = std::make_shared<input_t>(), b = std::make_shared<input_t>();
  const std::string test = argv[1];
  if (test == "every_key_released") {
    // Ordinary keys, function/navigation/numpad keys and all modifier variants.
    for (int code = 1; code <= 255; ++code) {
      key(a, code, false, 0, 1);
    }
    disconnect(a);
    std::map<int, int> held;
    for (auto e : platf::keyboard_events) held[host_keycode(e.key)] += e.release ? -1 : 1;
    for (auto [key, count] : held) assert(count == 0);
    assert(host_keys.empty() && a->keys.empty());
  } else if (test == "remap_changes_while_held") {
    config::input.keybindings[0x41] = 0x42;
    key(a, 0x41, false, 0, 1);
    config::input.keybindings[0x41] = 0x43;
    disconnect(a);
    expect({{0x42, false, 1}, {0x42, true, 1}});
  } else if (test == "key_up_flags_change") {
    key(a, 0x41, false, 0, 1); key(a, 0x41, true, 0, 0);
    disconnect(a);
    expect({{0x41, false, 1}, {0x41, true, 1}});
  } else if (test == "two_clients_same_key") {
    key(a, 0x41); key(b, 0x41); disconnect(a);
    expect({{0x41, false, 0}});
    disconnect(b); expect({{0x41, false, 0}, {0x41, true, 0}});
  } else if (test == "foreign_key_up") {
    key(a, 0x41); key(b, 0x41, true); disconnect(b);
    expect({{0x41, false, 0}});
    disconnect(a); expect({{0x41, false, 0}, {0x41, true, 0}});
  } else if (test == "two_keys_same_host_key") {
    config::input.keybindings[0x42] = 0x41;
    key(a, 0x41); key(a, 0x42); key(a, 0x41, true);
    expect({{0x41, false, 0}});
    disconnect(a); expect({{0x41, false, 0}, {0x41, true, 0}});
  } else if (test == "remapped_alt_no_synthetic_alt") {
    config::input.keybindings[VKEY_LMENU] = 0x5B;
    key(a, VKEY_LMENU, false, MODIFIER_ALT);
    key(a, 0x20, false, MODIFIER_ALT); key(a, 0x20, true);
    key(a, VKEY_LMENU, true);
    expect({{0x5B, false, 0}, {0x20, false, 0}, {0x20, true, 0}, {0x5B, true, 0}});
  } else if (test == "modifier_sides") {
    for (auto mods : {std::array<int, 3>{VKEY_LSHIFT, VKEY_RSHIFT, MODIFIER_SHIFT},
                      std::array<int, 3>{VKEY_LCONTROL, VKEY_RCONTROL, MODIFIER_CTRL},
                      std::array<int, 3>{VKEY_LMENU, VKEY_RMENU, MODIFIER_ALT}}) {
      key(a, mods[0]); key(a, mods[1]); key(a, mods[1], true);
      auto count = platf::keyboard_events.size();
      key(a, 0x41, false, mods[2]); key(a, 0x41, true);
      assert(platf::keyboard_events.size() == count + 2);
      key(a, mods[0], true);
    }
  } else if (test == "synthetic_modifiers_balanced") {
    for (int mods = 1; mods < 8; ++mods) {
      key(a, 0x41, false, mods); key(a, 0x41, true);
    }
    std::map<int, int> held;
    for (auto e : platf::keyboard_events) held[e.key] += e.release ? -1 : 1;
    for (auto [key, count] : held) assert(count == 0);
  } else if (test == "synthetic_does_not_release_foreign_alt") {
    key(a, VKEY_LMENU); key(b, 0x41, false, MODIFIER_ALT); key(b, 0x41, true);
    disconnect(b);
    expect({{VKEY_LMENU, false, 0}, {0x41, false, 0}, {0x41, true, 0}});
    disconnect(a); assert(host_keys.empty());
  } else if (test == "remapped_ordinary_key_holds_alt") {
    config::input.keybindings[0x41] = VKEY_LMENU;
    key(a, 0x41); key(a, 0x42, false, MODIFIER_ALT); key(a, 0x42, true);
    expect({{VKEY_LMENU, false, 0}, {0x42, false, 0}, {0x42, true, 0}});
    disconnect(a); assert(host_keys.empty());
  } else if (test == "repeat_cancelled_on_disconnect") {
    config::input.key_repeat_delay = 10ms;
    key(a, 0x41); task_pool.fire(); disconnect(a);
    expect({{0x41, false, 0}, {0x41, false, 0}, {0x41, true, 0}});
    assert(task_pool.timers.empty());
  } else if (test == "other_client_repeat_survives") {
    config::input.key_repeat_delay = 10ms;
    key(a, 0x41); disconnect(b); task_pool.fire(); disconnect(a);
    expect({{0x41, false, 0}, {0x41, false, 0}, {0x41, true, 0}});
  } else if (test == "repeat_uses_original_mapping") {
    config::input.key_repeat_delay = 10ms;
    config::input.keybindings[0x41] = 0x42; key(a, 0x41);
    config::input.keybindings[0x41] = 0x43; task_pool.fire(); disconnect(a);
    expect({{0x42, false, 0}, {0x42, false, 0}, {0x42, true, 0}});
  } else if (test == "repeat_rechecks_modifiers") {
    config::input.key_repeat_delay = 10ms;
    key(a, 0x41, false, MODIFIER_ALT);
    key(b, VKEY_LMENU);
    // Two timers; fire only the repeating ordinary key from a.
    auto f = std::move(task_pool.timers.at(a->key_press_repeat_id));
    task_pool.timers.erase(a->key_press_repeat_id);
    platf::keyboard_events.clear(); f();
    expect({{0x41, false, 0}});
    disconnect(a); disconnect(b); assert(task_pool.timers.empty());
  } else if (test == "repeat_real_alt_released") {
    config::input.key_repeat_delay = 10ms;
    key(a, VKEY_LMENU); key(a, 0x41, false, MODIFIER_ALT);
    key(a, VKEY_LMENU, true); platf::keyboard_events.clear();
    task_pool.fire(); expect({{0x41, false, 0}});
    disconnect(a);
  } else if (test == "all_mouse_buttons") {
    for (int button_id = 1; button_id <= 5; ++button_id) button(a, button_id);
    disconnect(a);
    std::map<int, int> held;
    for (auto e : events) held[e.button] += e.release ? -1 : 1;
    assert(held.size() == 5);
    for (auto [key, count] : held) assert(count == 0);
  } else if (test == "invalid_mouse_buttons") {
    for (int button_id : {0, -1, 6, 100}) button(a, button_id);
    disconnect(a); assert(events.empty());
  } else if (test == "disabled_input_still_releases") {
    key(a, 0x41); button(a, 5);
    config::input.keyboard = false; config::input.mouse = false;
    key(a, 0x41, true); button(a, 5, true);
    expect({{0x41, false, 0}, {0x41, true, 0}});
    assert(events == (std::vector<event_t>{{5, false}, {5, true}}));
  } else if (test == "enqueue_permissions") {
    auto enqueue = [&](uint32_t magic, crypto::PERM permission) {
      passthrough(a, packet_bytes(magic), permission);
    };
    enqueue(KEY_DOWN_EVENT_MAGIC, crypto::PERM::_no);
    assert(a->input_queue.empty() && task_pool.tasks.empty());
    enqueue(KEY_DOWN_EVENT_MAGIC, crypto::PERM::input_mouse);
    assert(a->input_queue.empty() && task_pool.tasks.empty());
    enqueue(MOUSE_MOVE_REL_MAGIC_GEN5, crypto::PERM::input_mouse);
    assert(a->input_queue.size() == 1 && task_pool.tasks.size() == 1);
    enqueue(KEY_DOWN_EVENT_MAGIC, crypto::PERM::input_kbd);
    assert(a->input_queue.size() == 2 && task_pool.tasks.size() == 1);
    enqueue(SS_PEN_MAGIC, crypto::PERM::_all_inputs);
    assert(a->input_queue.size() == 3 && task_pool.tasks.size() == 1);
    disconnect(a);
    enqueue(MOUSE_MOVE_REL_MAGIC_GEN5, crypto::PERM::input_mouse);
    assert(a->input_queue.empty() && task_pool.tasks.empty());
  } else if (test == "late_packets_and_double_reset") {
    key(a, 0x41);
    a->input_queue.push_back({1}); reset(a);
    passthrough(a, packet_bytes(KEY_DOWN_EVENT_MAGIC), crypto::PERM::_all_inputs);
    assert(a->input_queue.empty());
    reset(a); task_pool.drain();
    passthrough(a, packet_bytes(KEY_DOWN_EVENT_MAGIC), crypto::PERM::_all_inputs);
    assert(a->input_queue.empty() && task_pool.tasks.empty());
    expect({{0x41, false, 0}, {0x41, true, 0}});
  } else if (test == "controller_hold_disconnect") {
    controller_packet p {0, 1, platf::BACK, 0, 255, 255, 32767, -32768, 123, -456};
    passthrough(a, &p);
    assert(!task_pool.timers.empty());
    auto retained = a; disconnect(a);
    assert(task_pool.timers.empty());
    assert(a->gamepads[0].id == -1 && platf::freed_gamepads.size() == 1);
    auto state = platf::gamepad_events.back().second;
    assert(state.buttonFlags == 0 && state.lt == 0 && state.rt == 0);
    assert(state.lsX == 0 && state.lsY == 0 && state.rsX == 0 && state.rsY == 0);
    disconnect(a); assert(platf::freed_gamepads.size() == 1);
  } else if (test == "controller_removed_with_back_timer") {
    controller_packet p {0, 1, platf::BACK}; passthrough(a, &p);
    p.activeGamepadMask = 0; passthrough(a, &p);
    assert(task_pool.timers.empty() && a->gamepads[0].id == -1);
    p.activeGamepadMask = 1; p.buttonFlags = 0; passthrough(a, &p);
    assert(a->gamepads[0].back_button_state == button_state_e::NONE);
    assert(platf::gamepad_events.back().second.buttonFlags == 0);
    disconnect(a);
  } else if (test == "controller_mask_removes_other_slot") {
    controller_packet p {0, 3, platf::BACK}; passthrough(a, &p);
    p.controllerNumber = 1; p.buttonFlags = 0; passthrough(a, &p);
    p.activeGamepadMask = 2; passthrough(a, &p);
    assert(a->gamepads[0].id == -1 && a->gamepads[1].id >= 0);
    assert(task_pool.timers.empty() && platf::freed_gamepads.size() == 1);
    disconnect(a);
  } else if (test == "touch_pen_destroyed_while_context_retained") {
    a->client_context = std::make_unique<platf::client_input_t>();
    auto retained = a; disconnect(a);
    assert(!retained->client_context);
    assert(platf::touch_cancels == 1 && platf::pen_cancels == 1 && platf::client_input_t::destroyed == 1);
    disconnect(a); assert(platf::client_input_t::destroyed == 1);
  } else if (test == "shortcut_swallowed") {
    key(a, VKEY_LSHIFT); key(a, VKEY_LCONTROL); key(a, VKEY_LMENU);
    auto count = platf::keyboard_events.size(); key(a, 0x70); key(a, 0x70, true);
    assert(platf::keyboard_events.size() == count); disconnect(a);
  } else { return 2; }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'src/input.cpp')
    args = parser.parse_args()
    source = args.source.read_text()
    prelude = production_prelude(source).replace('std::list<int> input_queue;', 'std::list<std::vector<uint8_t>> input_queue;\n  std::atomic_bool input_queue_task_scheduled {false};\n  int feedback_queue = 0;')
    prelude = prelude.replace('void push(std::function<void()> f) { tasks.push_back(std::move(f)); }',
                              'template<class F, class... A> void push(F f, A... a) { tasks.push_back([=] { f(a...); }); }')
    prelude = prelude.replace('constexpr int KEY_UP_EVENT_MAGIC = 2;', 'constexpr int KEY_UP_EVENT_MAGIC = 0x04;')
    prelude = prelude.replace('constexpr int MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5 = 1;', 'constexpr int MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5 = 0x09;')
    crypto_source = (ROOT / 'src/crypto.h').read_text()
    permissions = crypto_source[crypto_source.index('  enum class PERM :'):crypto_source.index('  struct command_entry_t')]
    prelude += '\nnamespace crypto {\n' + permissions + '\n}\n'
    signatures = (
        'short map_keycode(short keycode)',
        'inline void update_shortcutFlags(int *flags, short keyCode, bool release)',
        'bool is_modifier(uint16_t keyCode)',
        'uint16_t host_keycode(uint16_t key)',
        'void refresh_shortcut_flags(input_t &input)',
        'void release_key(const held_key_t &key)',
        'void send_key_and_modifiers(const held_key_t &key, int shortcut_flags)',
        'void repeat_key(std::shared_ptr<input_t> input, uint16_t key_code)',
        'void passthrough(std::shared_ptr<input_t> &input, PNV_KEYBOARD_PACKET packet)',
        'void passthrough(std::shared_ptr<input_t> &input, PNV_MOUSE_BUTTON_PACKET packet)',
        'void free_gamepad(platf::input_t &platf_input, int id)',
        'void reset_gamepad(gamepad_t &gamepad)',
        'void passthrough(std::shared_ptr<input_t> &input, PNV_MULTI_CONTROLLER_PACKET packet)',
        'void passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data, const crypto::PERM &permission)',
        'void reset(std::shared_ptr<input_t> &input)',
    )
    handlers = '\n'.join(function(source, signature) for signature in signatures)
    import re
    cases = re.findall(r'test == "([^"]+)"', TESTS)
    failed = []
    with tempfile.TemporaryDirectory(prefix='input-disconnect-') as temp:
        cpp, binary = Path(temp) / 'test.cpp', Path(temp) / 'test'
        cpp.write_text(prelude + EXTRA + handlers + TESTS)
        subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Wno-sign-compare',
                        '-Wno-unused-const-variable', '-Wno-missing-field-initializers',
                        str(cpp), '-o', str(binary)], check=True)
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
