#!/usr/bin/env python3
"""Run the DualSense USB/IP feedback flush against the shared client mailbox under saturation.

The production `publish_latest` and `flush_output` members of the USB/IP slot are compiled with a
fake slot layout and the production queue and admission policy (common.h, thread_safe.h). The
consumer never drains, as when the control connection stalls. No device or driver is involved.
--baseline compiles HEAD; AUDIT_JSON_INCLUDE locates nlohmann/json.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def read(name):
    if '--baseline' in sys.argv:
        return subprocess.check_output(['git', 'show', 'HEAD:' + name], cwd=ROOT, text=True)
    return (ROOT / name).read_text()


def block(text, start):
    pos = text.index('{', start)
    depth = 1
    end = pos + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


common = read('src/platform/common.h')
types = common[common.index('  enum class gamepad_feedback_e'):common.index('  using feedback_queue_t')]
helpers = '\n'.join(block(common, common.index(n)) for n in (
    'inline unsigned steam_haptic_stop_key', '  inline unsigned gamepad_feedback_state_key', '  inline bool raise_gamepad_feedback('))
usbip = read('src/platform/windows/dualsense_usbip_gamepad.cpp')
publish = block(usbip, usbip.index('      bool publish_latest(gamepad_feedback_msg_t message) {'))
flush = block(usbip, usbip.index('      void flush_output() {'))
policy = read('src/platform/windows/vhf_gamepad_policy.h')
outcome = block(policy, policy.index('  enum class rumble_poll_outcome_e')) + ';'
observation = block(policy, policy.index('  struct rumble_poll_observation_t')) + ';'
dispatch = block(policy, policy.index('  template<class Queue, class MakeRumble'))
legacy_dispatch = 'Admit' not in dispatch

code = r'''
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>
#include "thread_safe_test.h"
#include "controller_speaker_pcm.h"
using namespace std::literals;
constexpr int LI_MOTION_TYPE_ACCEL = 1, LI_MOTION_TYPE_GYRO = 2;
@TYPES@
@HELPERS@
@OUTCOME@
@OBSERVATION@
@DISPATCH@
using queue_t = safe::queue_t<gamepad_feedback_msg_t>;
using feedback_queue_t = std::shared_ptr<queue_t>;
namespace vhf_gamepad {
  struct trigger_effect_t { std::uint8_t mode {}; std::array<std::uint8_t, 10> parameters {}; };
  struct rumble_rgb_t {
    std::uint16_t low_frequency {}, high_frequency {};
    std::uint8_t red {}, green {}, blue {};
    trigger_effect_t left_effect {}, right_effect {};
  };
}
// The USB/IP slot as the flush sees it.
struct usb_slot {
    bool active = true;
    std::uint16_t client_index = 0;
    feedback_queue_t feedback_queue;
    vhf_gamepad::rumble_rgb_t last_output {};
    bool pending_rumble = false, pending_rgb = false;
    std::uint8_t pending_triggers = 0;
    bool pending_accel_request = false, pending_gyro_request = false;
@PUBLISH@
@FLUSH@
};
int checks = 0, failures = 0;
void check(bool ok, const char* message) { ++checks; failures += !ok; std::cout << (ok ? "PASS " : "FAIL ") << message << '\n'; }
int count_triggers(queue_t& q, int id, int flags) {
    const auto& v = q.unsafe();
    return (int)std::count_if(v.begin(), v.end(), [&](const gamepad_feedback_msg_t& m) {
        return m.type == gamepad_feedback_e::set_adaptive_triggers && m.id == id && (flags < 0 || m.data.adaptive_triggers.event_flags == flags); });
}
int main() {
    std::cout.setf(std::ios::unitbuf);
    auto queue = std::make_shared<queue_t>(256, false);   // the stream mailbox; nobody drains it
    usb_slot pad; pad.client_index = 3; pad.feedback_queue = queue;

    // Independent enables: a left-only program, then a right-only one; both must stay queued.
    pad.last_output.left_effect.mode = 0x21; pad.last_output.left_effect.parameters[0] = 0xAA;
    pad.pending_triggers = 0x04; pad.flush_output();
    pad.last_output.right_effect.mode = 0x26; pad.last_output.right_effect.parameters[0] = 0xBB;
    pad.pending_triggers = 0x08; pad.flush_output();
    check(count_triggers(*queue, 3, 0x04) == 1 && count_triggers(*queue, 3, 0x08) == 1 && pad.pending_triggers == 0,
          "a right-only program does not replace the unsent left-only program");
    // A newer right-only program replaces the pending right-only one, not the left-only one.
    pad.last_output.right_effect.parameters[0] = 0xCC;
    pad.pending_triggers = 0x08; pad.flush_output();
    const auto& v = queue->unsafe();
    auto right = std::find_if(v.begin(), v.end(), [](const gamepad_feedback_msg_t& m) {
        return m.type == gamepad_feedback_e::set_adaptive_triggers && m.data.adaptive_triggers.event_flags == 0x08; });
    check(count_triggers(*queue, 3, -1) == 2 && right != v.end() && right->data.adaptive_triggers.right[0] == 0xCC,
          "a newer program for the same side replaces the pending one and leaves the other side's program");

    // The consumer is stalled: hundreds of changed programs for each side and for both sides.
    for (int i = 0; i < 300; ++i) {
        pad.last_output.left_effect.parameters[1] = (std::uint8_t)i;
        pad.last_output.right_effect.parameters[1] = (std::uint8_t)(i * 3);
        pad.pending_triggers = (std::uint8_t)(i % 3 == 0 ? 0x04 : i % 3 == 1 ? 0x08 : 0x0c);
        pad.flush_output();
    }
    check(count_triggers(*queue, 3, -1) == 3 && queue->unsafe().size() <= 256 && pad.pending_triggers == 0,
          "saturating trigger programs coalesce to one pending program per enable set (left, right, both)");

    // A Steam slot (index 15, no raw haptic capability) had a synthesized rumble start consumed
    // earlier; its expiry stop must still be admitted now.
    const auto make_rumble = [](std::uint16_t id, std::uint16_t low, std::uint16_t high) { return gamepad_feedback_msg_t::make_rumble(id, low, high); };
    const auto admit = [](auto& q, gamepad_feedback_msg_t msg) { return raise_gamepad_feedback(q, std::move(msg)); };
    rumble_poll_observation_t stop {};
    stop.driver_slot = 15; stop.client_relative_index = 15; stop.outcome = rumble_poll_outcome_e::raise_attempted;
#if LEGACY_DISPATCH
    const bool admitted = dispatch_rumble_poll(stop, queue, make_rumble);
#else
    const bool admitted = dispatch_rumble_poll(stop, queue, make_rumble, admit);
#endif
    const auto& w = queue->unsafe();
    const bool present = std::any_of(w.begin(), w.end(), [](const gamepad_feedback_msg_t& m) {
        return m.type == gamepad_feedback_e::rumble && m.id == 15 && m.data.rumble.lowfreq == 0 && m.data.rumble.highfreq == 0; });
    check(admitted && present, "a consumed Steam start's expiry stop is admitted after USB/IP trigger saturation");

    // Rumble and LED from this producer keep their latest-wins behaviour.
    pad.last_output.low_frequency = 10; pad.pending_rumble = true; pad.flush_output();
    pad.last_output.low_frequency = 20; pad.pending_rumble = true; pad.flush_output();
    const auto& x = queue->unsafe();
    const int rumbles = (int)std::count_if(x.begin(), x.end(), [](const gamepad_feedback_msg_t& m) { return m.type == gamepad_feedback_e::rumble && m.id == 3; });
    check(rumbles == 1 && !pad.pending_rumble, "the USB/IP rumble keeps one pending entry with the latest value");

    // A stopped mailbox refuses: the enables stay pending for the next flush.
    queue->stop();
    pad.pending_triggers = 0x04; pad.flush_output();
    check(pad.pending_triggers == 0x04, "a refused trigger program keeps its enable pending");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures != 0;
}
'''.replace('@TYPES@', types).replace('@HELPERS@', helpers).replace('@OUTCOME@', outcome).replace('@OBSERVATION@', observation).replace(
    '@DISPATCH@', dispatch).replace('@PUBLISH@', publish).replace('@FLUSH@', flush)

with tempfile.TemporaryDirectory(prefix='usbip-triggers-') as directory:
    work = Path(directory)
    (work / 'thread_safe_test.h').write_text(read('src/thread_safe.h'))
    (work / 'test.cpp').write_text(code)
    args = ['g++', '-std=c++20', '-fsanitize=address,undefined', '-g', '-I' + str(ROOT / 'src'),
            '-DLEGACY_DISPATCH=' + ('1' if legacy_dispatch else '0')]
    if os.getenv('AUDIT_JSON_INCLUDE'):
        args += ['-I' + os.environ['AUDIT_JSON_INCLUDE']]
    subprocess.run(args + [str(work / 'test.cpp'), '-pthread', '-o', str(work / 'test')], check=True)
    raise SystemExit(subprocess.run([str(work / 'test')]).returncode)
