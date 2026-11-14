#!/usr/bin/env python3
"""Run the control thread's feedback drain against production slot retirement.

The drain loop is sliced out of src/stream.cpp and compiled with the production
queue (src/thread_safe.h) and vhf_gamepad_t::free(). A queue wrapper inserts a
barrier between the loop's readiness check and its dequeue so another thread's
retirement can land exactly there. No Windows driver is loaded. --baseline
compiles HEAD; AUDIT_JSON_INCLUDE locates nlohmann/json.
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


stream = read('src/stream.cpp')
drain_start = stream.index('            auto &feedback_queue = session->control.feedback_queue;')
drain = stream[drain_start:stream.index('            auto &hdr_queue', drain_start)].rstrip()
retire = function(read('src/platform/windows/vhf_gamepad.cpp'), '  void vhf_gamepad_t::free(')
common = read('src/platform/common.h')
types = common[common.index('  enum class gamepad_feedback_e'):common.index('  using feedback_queue_t')]
code = r'''
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <shared_mutex>
#include <thread>
#include <vector>
#include "thread_safe_test.h"
#include "controller_speaker_pcm.h"
#include "platform/windows/vhf_keyboard_mouse_policy.h"
namespace vhf_keyboard_mouse = platf::vhf_keyboard_mouse;
using namespace std::literals;
#define BOOST_LOG(level) std::clog
using DWORD = unsigned;
constexpr DWORD ERROR_SUCCESS = 0;
constexpr int MAX_GAMEPADS = 16;
namespace util { struct hex { DWORD n; std::string to_string_view() const { return std::to_string(n); } }; }
namespace dualsense_audio { void set_slot(int, bool) {} }
@TYPES@
// The production queue with a barrier between the consumer's readiness check
// and its dequeue. The hook runs at most once, on the consumer thread, while a
// value is queued: a consumer that checks readiness and then blocks for the
// value meets the removal in between; a consumer that dequeues atomically meets
// it just before the dequeue and must simply find the mailbox changed.
struct barrier_queue_t : safe::queue_t<gamepad_feedback_msg_t> {
    using base_t = safe::queue_t<gamepad_feedback_msg_t>;
    using base_t::queue_t;
    std::function<void()> after_ready;
    void barrier() {
        if (after_ready && base_t::peek()) {
            auto hook = std::move(after_ready);
            after_ready = nullptr;
            hook();
        }
    }
    bool peek() {
        const bool ready = base_t::peek();
        barrier();
        return ready;
    }
#if QUEUE_HAS_TRY_POP
    base_t::status_t try_pop() {
        barrier();
        return base_t::try_pop();
    }
#endif
};
using queue_t = barrier_queue_t;
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
    bool keyboard_mouse_owned = false, keyboard_mouse_faulted = false;
    bool keyboard_mouse_teardown_confirmed = false, keyboard_mouse_recovery_needs_probe = false;
    void observe_status(DWORD) {}
};
struct vhf_gamepad_t { impl_t* impl; void free(int); };
@RETIRE@
// The control thread's view of a session: only the feedback mailbox matters here.
struct control_t { std::shared_ptr<queue_t> feedback_queue; };
struct session_t { control_t control; };
std::vector<gamepad_feedback_msg_t> sent;
int send_feedback_msg(session_t*, gamepad_feedback_msg_t& msg) { sent.push_back(msg); return 0; }
void drain_feedback(session_t* session) {
@DRAIN@
}
int checks = 0, failures = 0;
bool hung = false;
void check(bool ok, const char* message) {
    ++checks; failures += !ok;
    std::cout << (ok ? "PASS " : "FAIL ") << message << '\n';
}
// Runs the drain on its own thread and reports whether it returned in time. A
// blocked drain is left behind (the process exits without joining it).
bool drain_finishes(session_t& session) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread([&session, done] { drain_feedback(&session); done->store(true); }).detach();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!done->load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    if (!done->load()) hung = true;
    return done->load();
}
int main() {
    std::cout.setf(std::ios::unitbuf);  // Keep results visible if a baseline drain aborts.
    // Two controllers share the stream mailbox; global and stream-relative indices differ.
    auto attach = [](impl_t& state, std::shared_ptr<queue_t> queue, int global, int relative) {
        state.slots[global].active = true;
        state.slots[global].client_relative_index = relative;
        state.slots[global].feedback_queue = queue;
        ++state.active_count;
    };
    {
        impl_t state; vhf_gamepad_t host {&state};
        auto queue = std::make_shared<queue_t>(256, false);
        attach(state, queue, 5, 1); attach(state, queue, 1, 7);
        session_t session {{queue}};
        queue->try_raise(gamepad_feedback_msg_t::make_rumble(1, 40, 50));
        queue->after_ready = [&] { host.free(5); };
        sent.clear();
        check(drain_finishes(session), "the drain returns when the only queued controller retires between readiness and dequeue");
        check(sent.empty() && !state.slots[5].active, "the retired controller's message is not sent");
        queue->try_raise(gamepad_feedback_msg_t::make_rumble(7, 1, 2));
        queue->try_raise(gamepad_feedback_msg_t::make_rumble(7, 3, 4));
        check(drain_finishes(session) && sent.size() == 2 && sent[0].id == 7 && sent[1].id == 7 &&
                  sent[0].data.rumble.lowfreq == 1 && sent[1].data.rumble.lowfreq == 3,
              "the surviving controller's later messages are delivered in order");
    }
    {
        impl_t state; vhf_gamepad_t host {&state};
        auto queue = std::make_shared<queue_t>(256, false);
        attach(state, queue, 5, 1); attach(state, queue, 1, 7);
        session_t session {{queue}};
        queue->try_raise(gamepad_feedback_msg_t::make_rumble(1, 40, 50));
        queue->try_raise(gamepad_feedback_msg_t::make_steam_settings(7, 0x87, 1, {8, 2, 0}));
        queue->try_raise(gamepad_feedback_msg_t::make_rumble(1, 0, 0));
        queue->after_ready = [&] { host.free(5); };
        sent.clear();
        check(drain_finishes(session) && sent.size() == 1 && sent[0].id == 7 &&
                  sent[0].type == gamepad_feedback_e::steam_settings,
              "a retirement in the window leaves only the other controller's message, which is delivered");
    }
    {
        impl_t state; vhf_gamepad_t host {&state};
        auto queue = std::make_shared<queue_t>(256, false);
        attach(state, queue, 3, 0);
        session_t session {{queue}};
        queue->try_raise(gamepad_feedback_msg_t::make_steam_haptic(0, 8, {0x81, 0, 1, 0, 1, 0, 1, 0}));
        queue->after_ready = [&] { host.free(3); };
        sent.clear();
        check(drain_finishes(session) && sent.empty() && state.active_count == 0 && state.client.closes == 1,
              "the drain returns when the last controller retires in the window and closes the driver client");
    }
    {
        impl_t state; vhf_gamepad_t host {&state};
        auto queue = std::make_shared<queue_t>(256, false);
        attach(state, queue, 5, 1);
        session_t session {{queue}};
        queue->try_raise(gamepad_feedback_msg_t::make_rumble(1, 40, 50));
        queue->try_raise(gamepad_feedback_msg_t::make_rumble(1, 0, 0));
        queue->after_ready = [&] { queue->stop(); };
        sent.clear();
        check(drain_finishes(session) && sent.empty(), "the drain returns when the mailbox stops in the window and sends nothing after the stop");
        queue->try_raise(gamepad_feedback_msg_t::make_rumble(1, 1, 1));
        check(drain_finishes(session) && sent.empty(), "a stopped mailbox stays drained");
    }
    {
        impl_t state;
        auto queue = std::make_shared<queue_t>(256, false);
        attach(state, queue, 5, 1); attach(state, queue, 1, 7);
        session_t session {{queue}};
        for (int i = 0; i < 6; ++i) queue->try_raise(gamepad_feedback_msg_t::make_rumble(i % 2 ? 1 : 7, i, i));
        sent.clear();
        bool ordered = drain_finishes(session) && sent.size() == 6;
        for (int i = 0; ordered && i < 6; ++i) ordered = sent[i].data.rumble.lowfreq == i;
        check(ordered && !queue->peek(), "without a retirement every message is delivered FIFO and the mailbox is left empty");
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    if (hung) {
        std::cout << "a drain never returned; exiting without joining it\n";
        std::_Exit(1);
    }
    return failures != 0;
}
'''.replace('@TYPES@', types).replace('@RETIRE@', retire).replace('@DRAIN@', drain)

with tempfile.TemporaryDirectory(prefix='vhf-drain-') as directory:
    work = Path(directory)
    queue_source = read('src/thread_safe.h')
    (work / 'thread_safe_test.h').write_text(queue_source)
    (work / 'test.cpp').write_text(code)
    args = ['g++', '-std=c++20', '-g', '-iquote' + str(ROOT / 'src'),
            '-DQUEUE_HAS_TRY_POP=' + ('1' if 'try_pop()' in queue_source else '0')]
    if os.name != 'nt':
        args += ['-fsanitize=address,undefined']
    if os.getenv('AUDIT_JSON_INCLUDE'):
        args += ['-I' + os.environ['AUDIT_JSON_INCLUDE']]
    subprocess.run(args + [str(work / 'test.cpp'), '-pthread', '-o', str(work / 'test')], check=True)
    raise SystemExit(subprocess.run([str(work / 'test')]).returncode)
