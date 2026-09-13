#!/usr/bin/env python3
"""Compile browser input lifetime helpers and shared touch-port reads with fake devices."""
from pathlib import Path
import subprocess
import tempfile

from test_delayed_mouse_release import ROOT, function

PRELUDE = r'''
#include <algorithm>
#include <atomic>
#include <bitset>
#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <functional>
#include <utility>
using namespace std::literals;
#ifdef _WIN32
constexpr int SM_CXVIRTUALSCREEN = 78;
constexpr int SM_CYVIRTUALSCREEN = 79;
int GetSystemMetrics(int metric) { return metric == SM_CXVIRTUALSCREEN ? 1920 : 1080; }
#endif
#define BOOST_LOG(level) std::clog
namespace input {
  struct touch_port_t {
    int offset_x = 0, offset_y = 0, width = 0, height = 0, env_width = 0, env_height = 0;
    float client_offsetX = 0, client_offsetY = 0, scalar_inv = 1, scalar_tpcoords = 1;
    explicit operator bool() const { return width && height && env_width && env_height; }
  };
}
namespace mail { constexpr int touch_port = 0; }
namespace safe {
  struct event_t {
    std::optional<input::touch_port_t> value;
    void raise(input::touch_port_t port) { value = port; }
    auto view(std::chrono::milliseconds wait) { assert(wait == 0ms); return value; }
  };
  struct mail_raw_t {
    std::shared_ptr<event_t> port = std::make_shared<event_t>();
    template<class T> std::shared_ptr<event_t> event(int) { return port; }
  };
}
namespace input {
  struct input_t {
    std::shared_ptr<safe::event_t> touch_port_event;
    touch_port_t touch_port;
    bool closed = false;
    int resets = 0;
  };
  void reset(std::shared_ptr<input_t> &input) { input->closed = true; ++input->resets; }
  std::shared_ptr<input_t> alloc(std::shared_ptr<safe::mail_raw_t> mail) {
    auto result = std::make_shared<input_t>(); result->touch_port_event = mail->port; return result;
  }
}
std::shared_ptr<safe::mail_raw_t> capture_mail;
std::shared_ptr<safe::mail_raw_t> current_capture_mail() { return capture_mail; }
'''
CALLBACK_STUBS = r'''
struct SessionDataChannelContext { std::string id; std::atomic_bool active {true}; };
struct worker_t {
  std::vector<std::function<void()>> tasks;
  void push(std::function<void()> f) { tasks.push_back(std::move(f)); }
} task_pool;
std::vector<std::string> closed_sessions;
void close_session(const std::string &id) { closed_sessions.push_back(id); }
'''
TESTS = r'''
int main() {
  capture_mail = std::make_shared<safe::mail_raw_t>();
  assert(!current_input_context("unknown"));
  browser_inputs.emplace("a", nullptr); browser_inputs.emplace("b", nullptr);
  auto a = current_input_context("a"), b = current_input_context("b");
  assert(a->context != b->context);
  a->gamepads.set(0); assert(!b->gamepads.test(0));
  input::touch_port_t port;
  port.width = port.env_width = 1920; port.height = port.env_height = 1080;
  capture_mail->port->raise(port);
  const auto coords_a = input::client_to_touchport(a->context, {0.5, 0.5}, {1, 1});
  const auto coords_b = input::client_to_touchport(b->context, {0.5, 0.5}, {1, 1});
  assert(coords_a == coords_b && coords_a->first == 960);
  port.width = port.env_width = 2560; capture_mail->port->raise(port);
  assert(input::client_to_touchport(a->context, {0.5, 0.5}, {1, 1})->first == 1280);
  assert(input::client_to_touchport(b->context, {0.5, 0.5}, {1, 1})->first == 1280);
  SessionDataChannelContext ctx_a {"a"};
  on_peer_state(&ctx_a, LWRTC_PEER_DISCONNECTED);
  assert(a->context->closed && !current_input_context("a") && !b->context->closed);
  on_peer_state(&ctx_a, LWRTC_PEER_CONNECTED);
  auto reconnected = current_input_context("a");
  assert(reconnected && reconnected != a && !reconnected->context->closed);
  on_peer_state(&ctx_a, LWRTC_PEER_FAILED);
  assert(reconnected->context->closed && !current_input_context("a"));
  assert(closed_sessions.empty() && task_pool.tasks.size() == 1);
  task_pool.tasks.front()(); task_pool.tasks.clear();
  assert(closed_sessions == std::vector<std::string>{"a"});
  on_peer_state(&ctx_a, LWRTC_PEER_CONNECTED);
  assert(!current_input_context("a"));
  reset_session_input("a");
  assert(a->context->closed && !b->context->closed);
  assert(!current_input_context("a")); // late callback cannot resurrect a
  reset_session_input("a"); assert(a->context->resets == 1);
  assert(current_input_context("b") == b);
  capture_mail = std::make_shared<safe::mail_raw_t>();
  auto replacement = current_input_context("b");
  assert(b->context->closed && replacement != b && !replacement->context->closed);
  reset_input_context(); assert(replacement->context->closed);
  reset_session_input("b"); assert(!current_input_context("b"));
  // No-capture input still gets usable default coordinates.
  capture_mail.reset(); browser_inputs.emplace("fallback", nullptr);
  auto fallback = current_input_context("fallback");
  assert(input::client_to_touchport(fallback->context, {0.5, 0.5}, {1, 1})->first == 960);
  reset_session_input("fallback"); assert(fallback->context->closed);
  std::cout << "PASS: independent browser input, gamepad slots, late callbacks, capture replacement, shared coordinates, connection loss/recovery and fallback\n";
}
'''


def main():
    source = (ROOT / 'src/webrtc_stream.cpp').read_text()
    definitions = source[source.index('    struct BrowserInput {'):source.index('    std::shared_ptr<safe::mail_raw_t> current_capture_mail();')]
    helpers = '\n'.join(function(source, signature) for signature in (
        'std::shared_ptr<BrowserInput> current_input_context(std::string_view session_id)',
        'void reset_session_input(std::string_view session_id)',
        '[[maybe_unused]] void suspend_session_input(std::string_view session_id)',
        '[[maybe_unused]] void resume_session_input(std::string_view session_id)',
        '[[maybe_unused]] void reset_input_context()',
    ))
    input_source = (ROOT / 'src/input.cpp').read_text()
    port_helper = function(input_source, 'std::optional<std::pair<float, float>> client_to_touchport(')
    with tempfile.TemporaryDirectory(prefix='browser-input-') as temp:
        cpp, binary = Path(temp) / 'test.cpp', Path(temp) / 'test'
        header = (ROOT / 'third-party/libwebrtc/include/libwebrtc_c.h').read_text()
        peer_enum = header[header.index('typedef enum lwrtc_peer_state {'):header.index('} lwrtc_peer_state_t;') + len('} lwrtc_peer_state_t;')]
        callback = function(source, 'void on_peer_state(void *user, int state)')
        cpp.write_text(PRELUDE + peer_enum + '\nnamespace input {\n' + port_helper + '\n}\n' + definitions + helpers + CALLBACK_STUBS + callback + TESTS)
        subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        # Arch disables WebRTC, leaving these anonymous-namespace helpers
        # without callback callers. Keep that configuration warning-clean.
        disabled_helpers = helpers
        for name in ('suspend_session_input', 'resume_session_input'):
            helper = function(source, f'[[maybe_unused]] void {name}(std::string_view session_id)')
            disabled_helpers = disabled_helpers.replace(helper, '\nnamespace {\n' + helper + '\n}\n')
        cpp.write_text(PRELUDE + '\nnamespace input {\n' + port_helper + '\n}\n' + definitions + disabled_helpers)
        subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror=unused-function', '-c', str(cpp), '-o', str(Path(temp) / 'disabled.o')], check=True)


if __name__ == '__main__':
    main()
