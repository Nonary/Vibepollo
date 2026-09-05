#!/usr/bin/env python3
"""Compile the production mouse/reset handlers with a deterministic input worker.

Run directly with Python; no platform backend, Moonlight, or configured build is
needed. --source can point at an older input.cpp to verify regression failures.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


PRELUDE = r'''
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
using namespace std::chrono_literals;
struct worker_t {
  using task_id_t = void *;
  struct timer_t { task_id_t task_id; };
  std::deque<std::function<void()>> tasks;
  std::map<task_id_t, std::function<void()>> timers;
  std::uintptr_t next_id = 10;
  void push(std::function<void()> f) { tasks.push_back(std::move(f)); }
  template<class Duration> timer_t pushDelayed(std::function<void()> f, Duration) {
    auto id = reinterpret_cast<task_id_t>(++next_id);
    timers.emplace(id, std::move(f));
    return {id};
  }
  bool cancel(task_id_t id) { return timers.erase(id) != 0; }
  void drain() {
    while (!tasks.empty()) {
      auto f = std::move(tasks.front()); tasks.pop_front(); f();
    }
  }
  std::function<void()> take_timer() {
    assert(timers.size() == 1);
    auto f = std::move(timers.begin()->second); timers.clear(); return f;
  }
  void fire() { take_timer()(); }
} task_pool;
namespace thread_pool_util { using ThreadPool = worker_t; }
#define DISABLE_LEFT_BUTTON_DELAY ((worker_t::task_id_t) 0x01)
#define ENABLE_LEFT_BUTTON_DELAY nullptr
constexpr int BUTTON_LEFT = 1, BUTTON_RIGHT = 3;
constexpr int MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5 = 1;
namespace config { struct { bool mouse = true; } input; }
namespace util::endian {
  template<class T> T big(T value) { return value; }
  template<class T> T little(T value) { return value; }
}
struct button_packet { struct { int magic; } header; int button; };
using PNV_MOUSE_BUTTON_PACKET = button_packet *;
struct relative_packet { int deltaX, deltaY; };
using PNV_REL_MOUSE_MOVE_PACKET = relative_packet *;
struct controller_t {
  void move_relative(std::array<int, 2>) {}
} controller;
auto mouse_controller = &controller;
struct input_t {
  worker_t::task_id_t mouse_left_button_timeout = nullptr;
  bool mouse_left_button_delay = true;
  std::mutex input_queue_lock;
  std::list<int> input_queue;
};
std::array<std::uint8_t, 5> mouse_press {};
std::array<input_t *, 5> mouse_press_owner {};
worker_t::task_id_t key_press_repeat_id {};
std::unordered_map<int, bool> key_press;
int platf_input;
int vk_from_kpid(int x) { return x; }
int flags_from_kpid(int) { return 0; }
struct event_t {
  int button; bool release;
  bool operator==(const event_t &) const = default;
};
std::vector<event_t> events;
namespace platf {
  void button_mouse(int, int button, bool release) { events.push_back({button, release}); }
  void keyboard_update(int, int, bool, int) {}
}
'''

TESTS = r'''
void button(std::shared_ptr<input_t> &input, int code, bool release) {
  button_packet packet {{release ? MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5 : 0}, code};
  passthrough(input, &packet);
}
void click(std::shared_ptr<input_t> &input) {
  button(input, BUTTON_LEFT, false);
  button(input, BUTTON_LEFT, true);
}
void expect(std::initializer_list<event_t> wanted) {
  if (events != std::vector<event_t>(wanted)) {
    std::cerr << "Unexpected host mouse events:";
    for (auto event : events) std::cerr << ' ' << event.button << (event.release ? " up" : " down");
    std::cerr << '\n'; std::exit(1);
  }
}
int main(int argc, char **argv) {
  assert(argc == 2);
  auto a = std::make_shared<input_t>();
  auto b = std::make_shared<input_t>();
  std::string test = argv[1];
  if (test == "cancelled_release") {
    click(a); reset(a); task_pool.drain();
    expect({{1, false}, {1, true}});
    assert(task_pool.timers.empty());
  } else if (test == "completed_release") {
    click(a); task_pool.fire(); reset(a); task_pool.drain();
    expect({{1, false}, {1, true}});
  } else if (test == "running_release") {
    click(a);
    auto running = task_pool.take_timer(); // cancellation can no longer find it
    reset(a); running(); task_pool.drain(); // single worker finishes it first
    expect({{1, false}, {1, true}});
  } else if (test == "queued_release") {
    button(a, 1, false);
    task_pool.push([&] { button(a, 1, true); });
    reset(a); task_pool.drain();
    expect({{1, false}, {1, true}});
    assert(task_pool.timers.empty());
  } else if (test == "relative_move_pending") {
    click(a); relative_packet move {1, 0}; passthrough(a, &move);
    reset(a); task_pool.drain();
    expect({{1, false}, {1, true}});
    assert(task_pool.timers.empty());
  } else if (test == "relative_release_immediate") {
    relative_packet move {1, 0}; passthrough(a, &move); click(a);
    expect({{1, false}, {1, true}});
    assert(task_pool.timers.empty());
  } else if (test == "right_click_order") {
    click(a); button(a, BUTTON_RIGHT, false); button(a, BUTTON_RIGHT, true);
    task_pool.fire(); reset(a); task_pool.drain();
    expect({{1, false}, {3, false}, {3, true}, {1, true}});
  } else if (test == "relative_right_press") {
    click(a); relative_packet move {1, 0}; passthrough(a, &move);
    button(a, BUTTON_RIGHT, false); task_pool.fire();
    expect({{1, false}, {3, false}, {1, true}});
    reset(a); task_pool.drain();
    expect({{1, false}, {3, false}, {1, true}, {3, true}});
  } else if (test == "newer_client_pending_release") {
    click(a); click(b); reset(a); task_pool.drain();
    expect({{1, false}, {1, false}});
    task_pool.fire(); expect({{1, false}, {1, false}, {1, true}});
  } else if (test == "newer_client_press_reset") {
    click(a); button(b, 1, false); reset(a); task_pool.drain();
    expect({{1, false}, {1, false}});
    reset(b); task_pool.drain();
    expect({{1, false}, {1, false}, {1, true}});
  } else if (test == "newer_client_press_timer") {
    click(a); button(b, 1, false); task_pool.fire(); reset(a); task_pool.drain();
    expect({{1, false}, {1, false}});
    assert(a->mouse_left_button_timeout == nullptr);
    reset(b); task_pool.drain();
    expect({{1, false}, {1, false}, {1, true}});
  } else if (test == "newer_press_timer") {
    click(a); button(a, 1, false); task_pool.fire();
    expect({{1, false}, {1, false}});
    assert(a->mouse_left_button_timeout == nullptr);
    button(a, 1, true); task_pool.fire();
    expect({{1, false}, {1, false}, {1, true}});
  } else if (test == "reset_is_serialized") {
    click(a); reset(a);
    assert(task_pool.timers.size() == 1); // no cross-thread cancellation
    task_pool.drain(); expect({{1, false}, {1, true}});
  } else if (test == "reset_discards_buffered_packets") {
    a->input_queue.push_back(1); reset(a); task_pool.drain();
    assert(a->input_queue.empty());
  } else { return 2; }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'src/input.cpp')
    args = parser.parse_args()
    source = args.source.read_text()
    handlers = '\n'.join(function(source, signature) for signature in (
        'void passthrough(std::shared_ptr<input_t> &input, PNV_REL_MOUSE_MOVE_PACKET packet)',
        'void passthrough(std::shared_ptr<input_t> &input, PNV_MOUSE_BUTTON_PACKET packet)',
        'void reset(std::shared_ptr<input_t> &input)',
    ))
    cases = [
        'cancelled_release', 'completed_release', 'running_release', 'queued_release',
        'relative_move_pending', 'relative_release_immediate', 'right_click_order',
        'relative_right_press', 'newer_client_pending_release',
        'newer_client_press_reset', 'newer_client_press_timer', 'newer_press_timer',
        'reset_is_serialized', 'reset_discards_buffered_packets',
    ]
    failures = []
    with tempfile.TemporaryDirectory(prefix='delayed-mouse-release-') as temp:
        cpp = Path(temp) / 'test.cpp'
        binary = Path(temp) / 'test'
        cpp.write_text(PRELUDE + handlers + TESTS)
        subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Wno-sign-compare',
                        str(cpp), '-o', str(binary)], check=True)
        for case in cases:
            result = subprocess.run([str(binary), case], capture_output=True, text=True)
            print(f'{"PASS" if result.returncode == 0 else "FAIL"}: {case}')
            if result.returncode:
                failures.append(case)
                print(result.stderr, end='')
    print(f'{len(cases) - len(failures)}/{len(cases)} passed')
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
