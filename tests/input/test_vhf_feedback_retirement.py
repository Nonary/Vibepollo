#!/usr/bin/env python3
"""Run production VHF slot retirement against a real feedback queue and driver fakes.

No Windows driver is loaded. This checks queued feedback ownership, not packets
already removed by the transport. AUDIT_JSON_INCLUDE locates nlohmann/json.
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


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start)
    depth = 1
    while depth:
        end += 1
        depth += (source[end] == '{') - (source[end] == '}')
    return source[start:end + 1]


source = read('src/platform/windows/vhf_gamepad.cpp')
retire = function(source, '  void vhf_gamepad_t::free(')
common = read('src/platform/common.h')
types = common[common.index('  enum class gamepad_feedback_e'):common.index('  using feedback_queue_t')]
code = r'''
#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <shared_mutex>
#include <vector>
#include "thread_safe_test.h"
#include "controller_speaker_pcm.h"
using namespace std::literals;
#define BOOST_LOG(level) std::clog
using DWORD = unsigned;
constexpr DWORD ERROR_SUCCESS = 0;
constexpr int MAX_GAMEPADS = 16;
namespace util { struct hex { DWORD n; std::string to_string_view() const { return std::to_string(n); } }; }
namespace dualsense_audio { void set_slot(int, bool) {} }
@TYPES@
using queue_t = safe::queue_t<gamepad_feedback_msg_t>;
struct slot_t {
    bool active = false;
    int profile = 0;
    std::uint16_t client_relative_index = 0;
    std::shared_ptr<queue_t> feedback_queue;
    void reset() { active = false; client_relative_index = 0; feedback_queue.reset(); }
};
struct client_t {
    DWORD status = ERROR_SUCCESS;
    int destroyed = 0, closes = 0;
    DWORD destroy_controller(std::uint32_t) { ++destroyed; return status; }
    void close() { ++closes; }
};
void log_feedback_counters(client_t&, int, int) {}
// Device-removal recovery is never pending here: the driver is present, so
// free() destroys the controller and reads its feedback counters.
struct recovery_t {
    bool pending() const { return false; }
    void reset() {}
};
struct impl_t {
    std::shared_mutex lifetime;
    std::array<slot_t, MAX_GAMEPADS> slots;
    std::atomic<unsigned> active_count {0};
    client_t client;
    recovery_t recovery;
    bool recovery_failure_logged = false;
    void observe_status(DWORD) {}
};
struct vhf_gamepad_t { impl_t* impl; void free(int); };
@RETIRE@
int main() {
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* message) {
        ++checks; failures += !ok;
        std::cout << (ok ? "PASS " : "FAIL ") << message << '\n';
    };
    for (DWORD destroy_status : {0u, 1167u}) {
        impl_t state;
        vhf_gamepad_t host {&state};
        auto queue = std::make_shared<queue_t>(256);
        auto attach = [&](int global, int relative) {
            state.slots[global].active = true;
            state.slots[global].client_relative_index = relative;
            state.slots[global].feedback_queue = queue;
            ++state.active_count;
        };
        attach(5, 1); attach(1, 7);  // Global and stream-relative indices differ.
        state.client.status = destroy_status;
        queue->try_raise(gamepad_feedback_msg_t::make_steam_settings(1, 0x87, 1, {7, 1, 0}));
        queue->try_raise(gamepad_feedback_msg_t::make_rumble(7, 40, 50));
        queue->try_raise(gamepad_feedback_msg_t::make_steam_haptic(1, 8, {0x81, 0, 1, 0, 1, 0, 1, 0}));
        queue->try_raise(gamepad_feedback_msg_t::make_steam_settings(7, 0x87, 1, {8, 2, 0}));
        host.free(5);
        check(!state.slots[5].active && state.active_count == 1 && state.client.destroyed == 1,
              "slot retires even if the device has already disappeared");
        attach(5, 1);
        queue->try_raise(gamepad_feedback_msg_t::make_steam_settings(1, 0x87, 1, {24, 3, 0}));
        std::vector<gamepad_feedback_msg_t> delivered;
        while (auto msg = queue->pop(0ms)) delivered.push_back(*msg);
        check(delivered.size() == 3 && delivered[0].id == 7 && delivered[1].id == 7 &&
                  delivered[0].type == gamepad_feedback_e::rumble &&
                  delivered[1].data.steam_settings.triples[0] == 8,
              "retirement preserves the other controller's feedback and FIFO order");
        check(delivered.size() == 3 && delivered[2].id == 1 &&
                  delivered[2].type == gamepad_feedback_e::steam_settings &&
                  delivered[2].data.steam_settings.triples[0] == 24,
              "a reused stream-relative slot receives only its new settings, no old pulse or settings");
        queue->try_raise(gamepad_feedback_msg_t::make_steam_settings(1, 0x8E, 0, {}));
        host.free(5);
        check(!queue->peek() && !queue->pop(0ms), "retiring the sole queued producer clears queue readiness");
        host.free(5); host.free(-1); host.free(MAX_GAMEPADS);
        check(state.active_count == 1 && state.client.destroyed == 2,
              "repeated retirement and invalid global slots leave the active controller alone");
        host.free(1);
        check(state.active_count == 0 && state.client.closes == 1,
              "the last controller still closes the driver client exactly once");
    }
    impl_t empty;
    vhf_gamepad_t host {&empty};
    empty.slots[2].active = true; empty.active_count = 1;
    host.free(2);
    check(empty.active_count == 0, "a controller with no feedback queue can retire");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures != 0;
}
'''.replace('@TYPES@', types).replace('@RETIRE@', retire)

with tempfile.TemporaryDirectory(prefix='vhf-retirement-') as directory:
    work = Path(directory)
    (work / 'thread_safe_test.h').write_text(read('src/thread_safe.h'))
    (work / 'test.cpp').write_text(code)
    args = ['g++', '-std=c++20', '-fsanitize=address,undefined', '-g', '-I' + str(ROOT / 'src')]
    if os.getenv('AUDIT_JSON_INCLUDE'):
        args += ['-I' + os.environ['AUDIT_JSON_INCLUDE']]
    subprocess.run(args + [str(work / 'test.cpp'), '-pthread', '-o', str(work / 'test')], check=True)
    raise SystemExit(subprocess.run([str(work / 'test')]).returncode)
