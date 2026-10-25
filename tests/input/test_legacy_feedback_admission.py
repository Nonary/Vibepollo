#!/usr/bin/env python3
"""Run the legacy (ViGEm, inputtino) feedback producers against the shared client mailbox.

The production ViGEm `rumble`/`set_rgb_led` members and the inputtino rumble/LED lambdas are
compiled with fake driver objects and the production queue and admission policy (common.h,
thread_safe.h). No driver is loaded. --baseline compiles HEAD; AUDIT_JSON_INCLUDE locates
nlohmann/json.
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
helper_names = ['inline unsigned steam_haptic_stop_key', '  inline unsigned gamepad_feedback_state_key',
                '  inline bool raise_gamepad_feedback(']
if 'raise_gamepad_feedback_deduped' in common:
    helper_names += ['  inline bool gamepad_feedback_same_value', '  inline bool raise_gamepad_feedback_deduped']
helpers = '\n'.join(block(common, common.index(n)) for n in helper_names)

vigem = read('src/platform/windows/input.cpp')
vigem_rumble = block(vigem, vigem.index('    void rumble(target_t::pointer target, std::uint8_t largeMotor, std::uint8_t smallMotor) {'))
vigem_led = block(vigem, vigem.index('    void set_rgb_led(target_t::pointer target, std::uint8_t r, std::uint8_t g, std::uint8_t b) {'))

inputtino = read('src/platform/linux/input/inputtino_gamepad.cpp')
rumble_start = inputtino.index('    auto on_rumble_fn = [')
inputtino_rumble = inputtino[rumble_start:inputtino.index('};', rumble_start) + 2]
led_start = inputtino.index('(*ds5).set_on_led(') + len('(*ds5).set_on_led(')
inputtino_led = 'auto on_led_fn = ' + inputtino[led_start:inputtino.index('});', led_start) + 1] + ';'

policy = read('src/platform/windows/vhf_gamepad_policy.h')
outcome = block(policy, policy.index('  enum class rumble_poll_outcome_e')) + ';'
observation = block(policy, policy.index('  struct rumble_poll_observation_t')) + ';'
dispatch = block(policy, policy.index('  template<class Queue, class MakeRumble'))
legacy_dispatch = 'Admit' not in dispatch

code = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "thread_safe_test.h"
#include "controller_speaker_pcm.h"
using namespace std::literals;
#define BOOST_LOG(level) std::clog
constexpr int MAX_GAMEPADS = 16;
constexpr int LI_MOTION_TYPE_ACCEL = 1, LI_MOTION_TYPE_GYRO = 2;
namespace config { struct { bool forward_rumble = true; } input; }
@TYPES@
@HELPERS@
namespace platf {
  using ::gamepad_feedback_e;
  using ::gamepad_feedback_msg_t;
  using ::raise_gamepad_feedback;
#if HAVE_DEDUPED
  using ::raise_gamepad_feedback_deduped;
#endif
}
@OUTCOME@
@OBSERVATION@
@DISPATCH@
using queue_t = safe::queue_t<gamepad_feedback_msg_t>;

// ----- ViGEm fakes: the target handle is an int, the per-gamepad context what the callbacks touch -----
struct target_t { using pointer = int*; };
struct gamepad_context_t {
    struct { int* p = nullptr; int* get() const { return p; } } gp;
    uint8_t client_relative_index = 0;
    std::shared_ptr<queue_t> feedback_queue;
    gamepad_feedback_msg_t last_rumble {};
    gamepad_feedback_msg_t last_rgb_led {};
};
struct vigem_t {
    std::vector<gamepad_context_t> gamepads;
    std::array<std::atomic_bool, MAX_GAMEPADS> first_rumble_callback_logged {};
@VIGEM_RUMBLE@
@VIGEM_LED@
};

// ----- inputtino fakes: the lambdas capture the mailbox, the client index and the joypad state -----
struct joypad_state { gamepad_feedback_msg_t last_rumble {}; gamepad_feedback_msg_t last_rgb_led {}; };
struct inputtino_pad {
    std::function<void(int, int)> on_rumble;
    std::function<void(int, int, int)> on_led;
    std::shared_ptr<joypad_state> state;
};
inputtino_pad make_inputtino(std::shared_ptr<queue_t> feedback_queue, uint16_t clientRelativeIndex) {
    struct { uint16_t clientRelativeIndex; } id {clientRelativeIndex};
    auto gamepad = std::make_shared<joypad_state>();
@INPUTTINO_RUMBLE@
@INPUTTINO_LED@
    return {on_rumble_fn, on_led_fn, gamepad};
}

int checks = 0, failures = 0;
void check(bool ok, const char* message) {
    ++checks; failures += !ok;
    std::cout << (ok ? "PASS " : "FAIL ") << message << '\n';
}
std::vector<gamepad_feedback_msg_t> pending(queue_t& q) { return q.unsafe(); }
int count_rumble(const std::vector<gamepad_feedback_msg_t>& v, int id) {
    return (int)std::count_if(v.begin(), v.end(), [&](const gamepad_feedback_msg_t& m) { return m.type == gamepad_feedback_e::rumble && m.id == id; });
}
bool has_rumble(const std::vector<gamepad_feedback_msg_t>& v, int id, int low, int high) {
    return std::any_of(v.begin(), v.end(), [&](const gamepad_feedback_msg_t& m) {
        return m.type == gamepad_feedback_e::rumble && m.id == id && m.data.rumble.lowfreq == low && m.data.rumble.highfreq == high; });
}

int main() {
    std::cout.setf(std::ios::unitbuf);
    auto queue = std::make_shared<queue_t>(256, false);   // the stream mailbox
    // Fifteen ViGEm pads (client indices 0..14) and one inputtino pad (15) share it. A Steam slot
    // without the raw haptic capability lives on index 15's slot as far as the mailbox is concerned.
    int handles[MAX_GAMEPADS];
    vigem_t vigem;
    for (int i = 0; i < 15; ++i) {
        gamepad_context_t ctx; ctx.gp.p = &handles[i]; ctx.client_relative_index = (uint8_t)i; ctx.feedback_queue = queue;
        vigem.gamepads.push_back(ctx);
    }
    inputtino_pad tino = make_inputtino(queue, 15);

    // Mixed-controller pressure: every legacy pad changes rumble and LED hundreds of times while
    // settings chunks (reserved) and a haptic pulse burst (ordinary events) arrive for the Steam slot.
    for (int round = 0; round < 300; ++round) {
        for (int i = 0; i < 15; ++i) {
            vigem.rumble(&handles[i], (uint8_t)(round % 2 ? 0x40 : 0x80), (uint8_t)(round % 3));
            vigem.set_rgb_led(&handles[i], (uint8_t)round, 0, (uint8_t)i);
        }
        tino.on_rumble(round % 2 ? 100 : 200, round);
        tino.on_led(round & 0xff, 1, 2);
    }
    for (uint8_t chunk = 0; chunk < 16; ++chunk)
        raise_gamepad_feedback(*queue, gamepad_feedback_msg_t::make_steam_settings(15, 0x87, 1, {chunk, 1, 0}));
    for (int i = 0; i < 40; ++i)
        raise_gamepad_feedback(*queue, gamepad_feedback_msg_t::make_steam_haptic(15, 8, {0x81, 1, 0x90, 0x01, 0, 0, 0x01, 0}));
    auto v = pending(*queue);
    bool coalesced = v.size() <= 256;
    for (int i = 0; i < 16 && coalesced; ++i) coalesced = count_rumble(v, i) == 1;
    check(coalesced, "legacy rumble/LED churn from sixteen controllers coalesces to one pending state each under settings and haptic pressure");
    check(has_rumble(v, 0, 0x4000, 0x0200) && has_rumble(v, 15, 100, 299),
          "the newest legacy rumble of each producer is the pending one");

    // The legacy stops: both must land at capacity, and the caches must say so only then.
    vigem.rumble(&handles[0], 0, 0);
    tino.on_rumble(0, 0);
    v = pending(*queue);
    check(has_rumble(v, 0, 0, 0) && count_rumble(v, 0) == 1 && vigem.gamepads[0].last_rumble.data.rumble.lowfreq == 0 && vigem.gamepads[0].last_rumble.data.rumble.highfreq == 0,
          "a ViGEm zero-motor stop replaces the pending rumble at capacity and is recorded as delivered");
    check(has_rumble(v, 15, 0, 0) && count_rumble(v, 15) == 1 && tino.state->last_rumble.data.rumble.lowfreq == 0 && tino.state->last_rumble.data.rumble.highfreq == 0,
          "an inputtino zero-motor stop replaces the pending rumble at capacity and is recorded as delivered");

    // A consumed synthesized start expiring on the Steam slot: its zero goes through the VHF
    // dispatcher under the same pressure and must be admitted.
    const auto make_rumble = [](uint16_t id, uint16_t low, uint16_t high) { return gamepad_feedback_msg_t::make_rumble(id, low, high); };
    const auto admit = [](auto& q, gamepad_feedback_msg_t msg) { return raise_gamepad_feedback(q, std::move(msg)); };
    rumble_poll_observation_t expiry {};
    expiry.driver_slot = 15; expiry.client_relative_index = 15; expiry.outcome = rumble_poll_outcome_e::raise_attempted;
#if LEGACY_DISPATCH
    bool admitted = dispatch_rumble_poll(expiry, queue, make_rumble);
#else
    bool admitted = dispatch_rumble_poll(expiry, queue, make_rumble, admit);
#endif
    v = pending(*queue);
    check(admitted && has_rumble(v, 15, 0, 0) && count_rumble(v, 15) == 1 && v.size() <= 256,
          "a synthesized rumble expiry stop for the Steam slot is admitted under mixed-controller pressure");

    // Duplicates are still suppressed.
    queue->reset();
    vigem.rumble(&handles[1], 7, 7); vigem.rumble(&handles[1], 7, 7);
    vigem.set_rgb_led(&handles[1], 1, 2, 3); vigem.set_rgb_led(&handles[1], 1, 2, 3);
    tino.on_rumble(9, 9); tino.on_rumble(9, 9);
    tino.on_led(4, 5, 6); tino.on_led(4, 5, 6);
    check(pending(*queue).size() == 4, "repeated identical rumble and LED values are sent once per producer");

    // Refusal (a stopped mailbox) leaves the duplicate cache stale so the next callback retries.
    queue->stop();
    const auto before_vigem = vigem.gamepads[1].last_rumble, before_tino = tino.state->last_rumble;
    vigem.rumble(&handles[1], 0, 0);
    tino.on_rumble(0, 0);
    check(gamepad_feedback_same_value_or_equal(vigem.gamepads[1].last_rumble, before_vigem) && gamepad_feedback_same_value_or_equal(tino.state->last_rumble, before_tino),
          "a refused stop is not recorded as delivered, so the next callback with the same value is retried");

    config::input.forward_rumble = false;
    auto quiet = std::make_shared<queue_t>(256, false);
    vigem.gamepads[2].feedback_queue = quiet;
    vigem.rumble(&handles[2], 50, 50);
    check(quiet->unsafe().empty(), "forward_rumble off still withholds ViGEm rumble");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures != 0;
}
'''
same_value_shim = r'''
inline bool gamepad_feedback_same_value_or_equal(const gamepad_feedback_msg_t& a, const gamepad_feedback_msg_t& b) {
    return a.type == b.type && a.id == b.id && a.data.rumble.lowfreq == b.data.rumble.lowfreq && a.data.rumble.highfreq == b.data.rumble.highfreq;
}
'''
code = (code.replace('@TYPES@', types).replace('@HELPERS@', helpers + same_value_shim)
        .replace('@OUTCOME@', outcome).replace('@OBSERVATION@', observation).replace('@DISPATCH@', dispatch)
        .replace('@VIGEM_RUMBLE@', vigem_rumble).replace('@VIGEM_LED@', vigem_led)
        .replace('@INPUTTINO_RUMBLE@', inputtino_rumble).replace('@INPUTTINO_LED@', inputtino_led))

with tempfile.TemporaryDirectory(prefix='legacy-feedback-') as directory:
    work = Path(directory)
    (work / 'thread_safe_test.h').write_text(read('src/thread_safe.h'))
    (work / 'test.cpp').write_text(code)
    args = ['g++', '-std=c++20', '-fsanitize=address,undefined', '-g', '-I' + str(ROOT / 'src'),
            '-DLEGACY_DISPATCH=' + ('1' if legacy_dispatch else '0'),
            '-DHAVE_DEDUPED=' + ('1' if 'raise_gamepad_feedback_deduped' in common else '0')]
    if os.getenv('AUDIT_JSON_INCLUDE'):
        args += ['-I' + os.environ['AUDIT_JSON_INCLUDE']]
    subprocess.run(args + [str(work / 'test.cpp'), '-pthread', '-o', str(work / 'test')], check=True)
    raise SystemExit(subprocess.run([str(work / 'test')]).returncode)
