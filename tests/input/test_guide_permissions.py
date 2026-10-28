#!/usr/bin/env python3
"""Exercise queued Guide permission changes through production session/input code.

Production update_device_info, permission admission, queue dispatch and Guide /
keyboard / controller handlers execute. The wire validator, logging, unexercised
packet classes, OS backend, mail and worker scheduler are fakes. Keyboard packets
use the harness's deterministic struct layout. Keyboard packets do not batch;
the batch boundary returns the production terminate result. No live I/O runs.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

from test_delayed_mouse_release import ROOT, function
from test_guide_session_config import input_harness

DISPATCH = r'''
struct input_header { int magic; };
using PNV_INPUT_HEADER = input_header *;
template<int N> struct unused_packet {};
using PNV_ABS_MOUSE_MOVE_PACKET = unused_packet<0> *;
using PNV_SCROLL_PACKET = unused_packet<1> *;
using PSS_HSCROLL_PACKET = unused_packet<2> *;
using PNV_UNICODE_PACKET = unused_packet<3> *;
using PSS_TOUCH_PACKET = unused_packet<4> *;
using PSS_PEN_PACKET = unused_packet<5> *;
using PSS_CONTROLLER_TOUCH_PACKET = unused_packet<6> *;
using PSS_CONTROLLER_MOTION_PACKET = unused_packet<7> *;
using PSS_CONTROLLER_BATTERY_PACKET = unused_packet<8> *;
void passthrough(std::shared_ptr<input_t> &, PNV_REL_MOUSE_MOVE_PACKET) {}
void passthrough(std::shared_ptr<input_t> &, PNV_MOUSE_BUTTON_PACKET) {}
void passthrough(std::shared_ptr<input_t> &, PSS_CONTROLLER_ARRIVAL_PACKET) {}
template<int N> void passthrough(std::shared_ptr<input_t> &, unused_packet<N> *) {}
void passthrough(PNV_UNICODE_PACKET) {}
namespace input {
  using input_t = ::input_t;
  using ::update_guide_button_permissions;
  void print(void *) {}
}
'''
SESSION = r'''
namespace stream {
  struct session_t {
    std::shared_ptr<input::input_t> input;
    crypto::PERM permission = crypto::PERM::_all;
    std::mutex metadata_mutex;
    std::string device_name = "client";
    bool stopped = false;
  };
  namespace session { void graceful_stop(session_t &session) { session.stopped = true; } }
}
'''
TESTS = r'''
crypto::PERM rights(crypto::PERM input) {
  return static_cast<crypto::PERM>(static_cast<uint32_t>(input) | static_cast<uint32_t>(crypto::PERM::view));
}
void change(stream::session_t &session, crypto::PERM input) {
  auto permission = rights(input);
  assert(!stream::session::update_device_info(session, "renamed", permission));
  assert(session.permission == permission && session.device_name == "renamed" && !session.stopped);
}
void packet(stream::session_t &session, int code = 124, bool release = false) {
  keyboard_packet key {{release ? KEY_UP_EVENT_MAGIC : KEY_DOWN_EVENT_MAGIC}, 0, static_cast<short>(code), 0};
  std::vector<uint8_t> data(sizeof(key)); std::memcpy(data.data(), &key, sizeof(key));
  passthrough(session.input, std::move(data), session.permission);
}
bool guide(const std::shared_ptr<input_t> &input) {
  const auto id = input->gamepads[0].id;
  for (auto it = platf::gamepad_events.rbegin(); it != platf::gamepad_events.rend(); ++it)
    if (it->first == id) return bool(it->second.buttonFlags & platf::HOME);
  std::abort();
}
void allowed_press(stream::session_t &session) {
  packet(session); task_pool.drain(); assert(guide(session.input));
}
int main(int argc, char **argv) {
  assert(argc == 2); const std::string test = argv[1];
  stream::session_t session;
  session.input = alloc(std::make_shared<safe::mail_raw_t>(), {124, 0}, false);
  auto input = session.input;
  controller_packet arrival {}; arrival.controllerNumber = 0; arrival.activeGamepadMask = 1;
  passthrough(input, &arrival); assert(input->gamepads[0].id >= 0);
  if (test == "allowed_down_up") {
    allowed_press(session); packet(session, 124, true); task_pool.drain();
    assert(!guide(input) && !input->guide_key_pressed && platf::keyboard_events.empty());
  } else if (test == "keyboard_only_denied_retained_controller") {
    change(session, crypto::PERM::input_kbd); task_pool.drain();
    packet(session); task_pool.drain(); assert(!guide(input) && input->guide_key_pressed);
    packet(session, 124, true); task_pool.drain(); assert(!input->guide_key_pressed);
    assert(platf::keyboard_events.empty());
  } else if (test == "denied_admission_stays_denied_after_grant") {
    change(session, crypto::PERM::input_kbd); packet(session);
    change(session, crypto::PERM::_all_inputs); task_pool.drain();
    assert(!guide(input) && input->guide_key_pressed && platf::keyboard_events.empty());
  } else if (test == "queued_allowed_press_invalidated_by_revoke_regrant") {
    packet(session); change(session, crypto::PERM::input_kbd);
    change(session, crypto::PERM::_all_inputs); task_pool.drain();
    assert(!guide(input) && input->guide_key_pressed && platf::keyboard_events.empty());
    packet(session); task_pool.drain(); assert(!guide(input));
    packet(session, 124, true); task_pool.drain(); allowed_press(session);
  } else if (test == "revoke_neutralizes_without_keyboard_packet") {
    allowed_press(session); change(session, crypto::PERM::input_kbd); task_pool.drain();
    assert(!guide(input) && !input->gamepads[0].guide_key_down && input->guide_key_pressed);
    change(session, crypto::PERM::_all_inputs); packet(session); task_pool.drain();
    assert(!guide(input)); packet(session, 124, true); task_pool.drain(); allowed_press(session);
  } else if (test == "held_keyup_after_controller_revocation") {
    allowed_press(session); change(session, crypto::PERM::input_kbd);
    packet(session, 124, true); task_pool.drain();
    assert(!guide(input) && !input->guide_key_pressed && platf::keyboard_events.empty());
  } else if (test == "keyboard_permission_loss_also_neutralizes") {
    allowed_press(session); change(session, crypto::PERM::input_controller);
    packet(session, 124, true); task_pool.drain();
    assert(!guide(input) && input->guide_key_pressed);
    change(session, crypto::PERM::_all_inputs); packet(session); task_pool.drain(); assert(!guide(input));
    packet(session, 124, true); task_pool.drain(); allowed_press(session);
  } else if (test == "neutralization_preserves_native_home_back_timer") {
    allowed_press(session);
    arrival.buttonFlags = platf::HOME | platf::BACK; passthrough(input, &arrival);
    assert(!task_pool.timers.empty()); auto timer_count = task_pool.timers.size();
    change(session, crypto::PERM::input_kbd); task_pool.drain();
    assert(!input->gamepads[0].guide_key_down && guide(input));
    assert(task_pool.timers.size() == timer_count);
    assert(input->gamepads[0].gamepad_state.buttonFlags & platf::BACK);
  } else if (test == "ordinary_keyboard_still_allowed") {
    change(session, crypto::PERM::input_kbd); task_pool.drain();
    packet(session, 65); packet(session, 65, true); task_pool.drain();
    assert(!guide(input) && platf::keyboard_events.size() == 2 && !input->guide_key_pressed);
  } else return 2;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input-source', type=Path, default=ROOT / 'src/input.cpp')
    parser.add_argument('--stream-source', type=Path, default=ROOT / 'src/stream.cpp')
    parser.add_argument('--save-cpp', type=Path)
    args = parser.parse_args()
    source = args.input_source.read_text()
    code = input_harness(source).replace('void passthrough_next_message(std::shared_ptr<input_t>) {}',
                                         'void passthrough_next_message(std::shared_ptr<input_t>);')
    code = code.replace('if (data.size() != sizeof(uint32_t)) return std::nullopt;',
                        'if (data.size() < sizeof(uint32_t)) return std::nullopt;')
    if 'void update_guide_button_permissions(' not in source:
        code += '\nvoid update_guide_button_permissions(const std::shared_ptr<input_t> &, const crypto::PERM &) {}\n'
    batch_enum = source[source.index('  enum class batch_result_e {'):source.index('  };', source.index('  enum class batch_result_e {')) + len('  };')]
    dispatch = function(source, 'void passthrough_next_message(std::shared_ptr<input_t> input)')
    enqueue = function(source, 'void passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data, const crypto::PERM &permission)')
    update = function(args.stream_source.read_text(), 'bool update_device_info(session_t &session, const std::string &name, const crypto::PERM &newPerm)')
    code += DISPATCH + batch_enum + '\nbatch_result_e batch(PNV_INPUT_HEADER, PNV_INPUT_HEADER) { return batch_result_e::terminate_batch; }\n'
    code += dispatch + enqueue + SESSION + '\nnamespace stream::session {\n' + update + '\n}\n' + TESTS
    if args.save_cpp:
        args.save_cpp.write_text(code)
    cases = re.findall(r'test == "([^"]+)"', TESTS)
    failed = []
    with tempfile.TemporaryDirectory(prefix='guide-permissions-') as temp:
        cpp, binary = Path(temp) / 'test.cpp', Path(temp) / 'test'
        cpp.write_text(code)
        subprocess.run(['c++', '-I', str(ROOT), '-std=c++20', '-Wall', '-Wextra', '-Wno-sign-compare', '-Wno-missing-field-initializers', str(cpp), '-o', str(binary)], check=True)
        for case in cases:
            result = subprocess.run([str(binary), case], capture_output=True, text=True)
            print(f'{"PASS" if result.returncode == 0 else "FAIL"}: {case}', flush=True)
            if result.returncode:
                failed.append(case)
                print(result.stderr)
    print(f'{len(cases) - len(failed)}/{len(cases)} passed')
    return bool(failed)


if __name__ == '__main__':
    raise SystemExit(main())
