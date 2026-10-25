#!/usr/bin/env python3
"""Run the production ordinary-rumble dispatch against a feedback mailbox at capacity.

dispatch_rumble_poll (src/platform/windows/vhf_gamepad_policy.h) and the mailbox
admission policy (src/platform/common.h) are compiled with the production queue.
No driver is loaded. --baseline compiles HEAD (the pre-admission three-argument
dispatch is called with plain raise); AUDIT_JSON_INCLUDE locates nlohmann/json.
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
helpers = '\n'.join(block(common, common.index(signature)) for signature in (
    'inline unsigned steam_haptic_stop_key', '  inline unsigned gamepad_feedback_state_key',
    '  inline bool raise_gamepad_feedback'))
policy = read('src/platform/windows/vhf_gamepad_policy.h')
outcome = block(policy, policy.index('  enum class rumble_poll_outcome_e')) + ';'
observation = block(policy, policy.index('  struct rumble_poll_observation_t')) + ';'
dispatch = block(policy, policy.index('  template<class Queue, class MakeRumble'))
legacy = 'Admit' not in dispatch
code = r'''
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>
#include "thread_safe_test.h"
#include "controller_speaker_pcm.h"
using namespace std::literals;
@TYPES@
@HELPERS@
@OUTCOME@
@OBSERVATION@
@DISPATCH@
constexpr std::uint32_t CAPACITY = 256;
int main() {
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* message) {
        ++checks; failures += !ok;
        std::cout << (ok ? "PASS " : "FAIL ") << message << '\n';
    };
    const auto make_rumble = [](std::uint16_t id, std::uint16_t low, std::uint16_t high) {
        return gamepad_feedback_msg_t::make_rumble(id, low, high);
    };
    const auto admit = [](auto& queue, gamepad_feedback_msg_t msg) { return raise_gamepad_feedback(queue, std::move(msg)); };
    auto dispatch = [&](const rumble_poll_observation_t& observation, auto& queue) {
#if LEGACY_DISPATCH
        return dispatch_rumble_poll(observation, queue, make_rumble);
#else
        return dispatch_rumble_poll(observation, queue, make_rumble, admit);
#endif
    };
    // The stream mailbox: bounded, dropping the newest plain raise() when full.
    auto queue = std::make_shared<safe::queue_t<gamepad_feedback_msg_t>>(CAPACITY, false);
    // Every controller's protected state (15 keys each) and the reserved settings chunks.
    for (std::uint16_t id = 0; id < 16; ++id) {
        raise_gamepad_feedback(*queue, gamepad_feedback_msg_t::make_rumble(id, 0x100, 0x200));
        raise_gamepad_feedback(*queue, gamepad_feedback_msg_t::make_rumble_triggers(id, 1, 2));
        raise_gamepad_feedback(*queue, gamepad_feedback_msg_t::make_rgb_led(id, 1, 2, 3));
        for (std::uint8_t motion : {1, 2}) raise_gamepad_feedback(*queue, gamepad_feedback_msg_t::make_motion_event_state(id, motion, 100));
        for (std::uint8_t flags : {4, 8, 12}) raise_gamepad_feedback(*queue, gamepad_feedback_msg_t::make_adaptive_triggers(id, flags, 0, 0, {}, {}));
        raise_gamepad_feedback(*queue, gamepad_feedback_msg_t::make_steam_haptic(id, 12, {0x80}));
        for (std::uint8_t type : {0x81, 0x82})
            for (std::uint8_t side = 0; side < 3; ++side)
                raise_gamepad_feedback(*queue, gamepad_feedback_msg_t::make_steam_haptic(id, 12, {type, side}));
    }
    for (std::uint8_t chunk = 0; chunk < 16; ++chunk)
        raise_gamepad_feedback(*queue, gamepad_feedback_msg_t::make_steam_settings(chunk, 0x87, 1, {chunk, 1, 0}));
    check(queue->unsafe().size() == CAPACITY, "240 state keys and 16 settings chunks fill the mailbox");
    check(!queue->try_raise(gamepad_feedback_msg_t::make_rumble(1, 0, 0)), "a plain raise is refused at capacity");

    // Controller 1: active motors to a stop, observed as a motor transition.
    rumble_poll_observation_t stop {};
    stop.driver_slot = 3; stop.client_relative_index = 1; stop.low_frequency = 0; stop.high_frequency = 0;
    stop.outcome = rumble_poll_outcome_e::raise_attempted;
    check(dispatch(stop, queue), "the dispatcher reports the zero-motor stop as admitted");
    std::vector<gamepad_feedback_msg_t> delivered;
    while (auto msg = queue->pop(0ms)) delivered.push_back(*msg);
    const auto stops = std::count_if(delivered.begin(), delivered.end(), [](const gamepad_feedback_msg_t& msg) {
        return msg.type == gamepad_feedback_e::rumble && msg.id == 1 && msg.data.rumble.lowfreq == 0 && msg.data.rumble.highfreq == 0;
    });
    const auto stale = std::count_if(delivered.begin(), delivered.end(), [](const gamepad_feedback_msg_t& msg) {
        return msg.type == gamepad_feedback_e::rumble && msg.id == 1 && msg.data.rumble.lowfreq == 0x100;
    });
    check(delivered.size() == CAPACITY && stops == 1 && stale == 0,
          "the stop reaches the client in place of the stale motor state and nothing else is lost");

    // Dedup and absence never touch the queue; a stopped queue rejects.
    rumble_poll_observation_t same = stop; same.outcome = rumble_poll_outcome_e::deduplicated;
    check(!dispatch(same, queue) && !queue->peek(), "a deduplicated observation queues nothing");
    std::shared_ptr<safe::queue_t<gamepad_feedback_msg_t>> missing;
    check(!dispatch(stop, missing), "a missing queue prevents the call");
    queue->stop();
    check(!dispatch(stop, queue) && queue->unsafe().empty(), "a stopped mailbox reports the stop as rejected");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures != 0;
}
'''.replace('@TYPES@', types).replace('@HELPERS@', helpers).replace('@OUTCOME@', outcome).replace(
    '@OBSERVATION@', observation).replace('@DISPATCH@', dispatch)

with tempfile.TemporaryDirectory(prefix='vhf-admission-') as directory:
    work = Path(directory)
    (work / 'thread_safe_test.h').write_text(read('src/thread_safe.h'))
    (work / 'test.cpp').write_text(code)
    args = ['g++', '-std=c++20', '-fsanitize=address,undefined', '-g', '-I' + str(ROOT / 'src'),
            '-DLEGACY_DISPATCH=' + ('1' if legacy else '0')]
    if os.getenv('AUDIT_JSON_INCLUDE'):
        args += ['-I' + os.environ['AUDIT_JSON_INCLUDE']]
    subprocess.run(args + [str(work / 'test.cpp'), '-pthread', '-o', str(work / 'test')], check=True)
    raise SystemExit(subprocess.run([str(work / 'test')]).returncode)
