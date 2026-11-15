#!/usr/bin/env python3
"""Compile unchanged production batching and queue dispatch with recording edges.

Uses real Moonlight wire packets and production endian conversion. No desktop,
configured application build, or platform injection is required. --source-file
can select the original input.cpp to demonstrate the regression.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


PRELUDE = r'''
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <list>
#include <stdexcept>
#include "src/utility.h"
#include "src/controller_speaker_pcm.h"
extern "C" {
#include <moonlight-common-c/src/Input.h>
#include <moonlight-common-c/src/Limelight.h>
}
#include "src/steam_controller_protocol.h"
namespace config { struct { bool controller = true; } input; }
struct worker_t {
  std::deque<std::function<void()>> tasks;
  template<class Function, class... Args>
  void push(Function function, Args... args) {
    tasks.emplace_back([=]() mutable { function(std::move(args)...); });
  }
  void drain() {
    while (!tasks.empty()) {
      auto function = std::move(tasks.front());
      tasks.pop_front();
      function();
    }
  }
} task_pool;
namespace input {
  using bytes_t = std::vector<std::uint8_t>;
  std::vector<bytes_t> dispatched;
  struct input_t {
    std::list<bytes_t> input_queue;
    std::mutex input_queue_lock;
    std::atomic_bool input_queue_task_scheduled {true};
    struct gamepad_t { int id = -1; };
    std::vector<gamepad_t> gamepads {16};
    struct bindings_t {
      template<class Predicate> void bind(const bytes_t &, Predicate) {
        throw std::runtime_error("unexpected speaker binding");
      }
    } speaker_bindings;
  };
  void refresh_input_geometry(const std::shared_ptr<input_t> &) {}
  void print(void *) {}
  // Only the OS/platform edge is replaced. Capture the exact packet passed by
  // the production switch, including its size, magic, and network byte order.
  template<class Packet>
  void passthrough(std::shared_ptr<input_t> &, Packet *packet) {
    const auto begin = reinterpret_cast<const std::uint8_t *>(packet);
    dispatched.emplace_back(begin, begin + sizeof(Packet));
  }
  void passthrough(PNV_UNICODE_PACKET packet) {
    const auto begin = reinterpret_cast<const std::uint8_t *>(packet);
    dispatched.emplace_back(begin, begin + sizeof(*packet));
  }
'''

TESTS = r'''
} // namespace input
using namespace input;
int failures = 0;
void check(bool condition, const std::string &message) {
  if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
template<class Packet> bytes_t bytes(const Packet &packet) {
  const auto begin = reinterpret_cast<const std::uint8_t *>(&packet);
  return {begin, begin + sizeof(packet)};
}
template<class Packet> Packet packet(std::uint32_t magic) {
  Packet result {};
  result.header.size = util::endian::big(static_cast<std::uint32_t>(sizeof(Packet) - 4));
  result.header.magic = util::endian::little(magic);
  return result;
}
NV_REL_MOUSE_MOVE_PACKET move(short x, short y) {
  auto result = packet<NV_REL_MOUSE_MOVE_PACKET>(MOUSE_MOVE_REL_MAGIC_GEN5);
  result.deltaX = util::endian::big(x);
  result.deltaY = util::endian::big(y);
  return result;
}
NV_SCROLL_PACKET vertical(short amount) {
  auto result = packet<NV_SCROLL_PACKET>(SCROLL_MAGIC_GEN5);
  result.scrollAmt1 = result.scrollAmt2 = util::endian::big(amount);
  result.zero3 = util::endian::big<short>(0x1234);
  return result;
}
SS_HSCROLL_PACKET horizontal(short amount) {
  auto result = packet<SS_HSCROLL_PACKET>(SS_HSCROLL_MAGIC);
  result.scrollAmount = util::endian::big(amount);
  return result;
}
NV_MOUSE_BUTTON_PACKET button(bool down) {
  auto result = packet<NV_MOUSE_BUTTON_PACKET>(down ? MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5 : MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5);
  result.button = 1;
  return result;
}
NV_KEYBOARD_PACKET key(bool down) {
  auto result = packet<NV_KEYBOARD_PACKET>(down ? KEY_DOWN_EVENT_MAGIC : KEY_UP_EVENT_MAGIC);
  result.keyCode = util::endian::little<short>(0x41);
  return result;
}
template<class Packet>
void expect_batch(Packet destination, Packet source, const Packet &expected,
                  batch_result_e wanted, const std::string &name) {
  const auto before_source = bytes(source);
  // Exercise the actual header dispatch, not just a test arithmetic helper.
  const auto result = batch(reinterpret_cast<PNV_INPUT_HEADER>(&destination),
                            reinterpret_cast<PNV_INPUT_HEADER>(&source));
  check(result == wanted, name + ": result");
  check(bytes(destination) == bytes(expected), name + ": complete destination");
  check(bytes(source) == before_source, name + ": unchanged source");
}
void arithmetic_tests() {
  struct safe_case_t { short first, second, sum; const char *name; };
  for (const auto &test : {
         safe_case_t {120, 30, 150, "positive"},
         safe_case_t {-120, -30, -150, "negative"},
         safe_case_t {120, -120, 0, "cancellation"},
         safe_case_t {0, 0, 0, "zero"},
         safe_case_t {32760, 7, 32767, "upper boundary"},
         safe_case_t {-32760, -8, -32768, "lower boundary"},
         safe_case_t {32767, -32768, -1, "opposite extremes"}}) {
    expect_batch(move(test.first, test.second), move(test.second, test.first),
                 move(test.sum, test.sum), batch_result_e::batched, std::string("relative ") + test.name);
    expect_batch(vertical(test.first), vertical(test.second), vertical(test.sum),
                 batch_result_e::batched, std::string("vertical ") + test.name);
    expect_batch(horizontal(test.first), horizontal(test.second), horizontal(test.sum),
                 batch_result_e::batched, std::string("horizontal ") + test.name);
  }
  for (const auto &test : {safe_case_t {32767, 1, 0, "positive overflow"},
                           safe_case_t {-32768, -1, 0, "negative overflow"}}) {
    expect_batch(move(test.first, 100), move(test.second, 1), move(test.first, 100),
                 batch_result_e::terminate_batch, std::string("relative X ") + test.name);
    expect_batch(move(100, test.first), move(1, test.second), move(100, test.first),
                 batch_result_e::terminate_batch, std::string("relative Y ") + test.name);
    expect_batch(move(test.first, test.first), move(test.second, test.second), move(test.first, test.first),
                 batch_result_e::terminate_batch, std::string("relative XY ") + test.name);
    expect_batch(vertical(test.first), vertical(test.second), vertical(test.first),
                 batch_result_e::terminate_batch, std::string("vertical ") + test.name);
    expect_batch(horizontal(test.first), horizontal(test.second), horizontal(test.first),
                 batch_result_e::terminate_batch, std::string("horizontal ") + test.name);
  }
}
void expect_queue(std::initializer_list<bytes_t> queued, std::initializer_list<bytes_t> expected,
                  const std::string &name) {
  auto context = std::make_shared<input_t>();
  context->input_queue.assign(queued);
  dispatched.clear();
  task_pool.push(passthrough_next_message, context);
  task_pool.drain();
  check(dispatched == std::vector<bytes_t>(expected), name + ": dispatched packet order/content");
  check(context->input_queue.empty(), name + ": queue drained");
  check(!context->input_queue_task_scheduled.load(), name + ": worker flag cleared");
  check(task_pool.tasks.empty(), name + ": no duplicate worker tasks");
}
void queue_tests() {
  expect_queue({bytes(move(120, -4)), bytes(move(30, 2)), bytes(move(-10, 3))},
               {bytes(move(140, 1))}, "adjacent relative");
  expect_queue({bytes(vertical(120)), bytes(vertical(-30)), bytes(vertical(10))},
               {bytes(vertical(100))}, "adjacent vertical");
  expect_queue({bytes(horizontal(-120)), bytes(horizontal(30)), bytes(horizontal(-10))},
               {bytes(horizontal(-100))}, "adjacent horizontal");
  expect_queue({bytes(move(32760, 0)), bytes(move(7, 0)), bytes(move(1, 1)), bytes(move(2, 2))},
               {bytes(move(32767, 0)), bytes(move(3, 3))}, "relative overflow preserves safe prefix");
  expect_queue({bytes(move(100, -32768)), bytes(move(1, -1)), bytes(move(2, -2))},
               {bytes(move(100, -32768)), bytes(move(3, -3))}, "relative Y overflow preserves both axes");
  expect_queue({bytes(vertical(32767)), bytes(vertical(1)), bytes(vertical(2))},
               {bytes(vertical(32767)), bytes(vertical(3))}, "vertical overflow leaves later packets");
  expect_queue({bytes(horizontal(-32768)), bytes(horizontal(-1)), bytes(horizontal(-2))},
               {bytes(horizontal(-32768)), bytes(horizontal(-3))}, "horizontal overflow leaves later packets");
  expect_queue({bytes(move(1, 2)), bytes(button(true)), bytes(move(3, 4)), bytes(button(false))},
               {bytes(move(1, 2)), bytes(button(true)), bytes(move(3, 4)), bytes(button(false))},
               "mouse button boundaries");
  expect_queue({bytes(move(1, 2)), bytes(key(true)), bytes(move(3, 4)), bytes(key(false))},
               {bytes(move(1, 2)), bytes(key(true)), bytes(move(3, 4)), bytes(key(false))},
               "keyboard boundaries");
  expect_queue({bytes(vertical(1)), bytes(horizontal(2)), bytes(vertical(3)), bytes(horizontal(4))},
               {bytes(vertical(1)), bytes(horizontal(2)), bytes(vertical(3)), bytes(horizontal(4))},
               "scroll axis boundaries");
  expect_queue({bytes(move(1, 2)), bytes(vertical(1)), bytes(move(3, 4))},
               {bytes(move(1, 2)), bytes(vertical(1)), bytes(move(3, 4))}, "move scroll boundaries");
  expect_queue({bytes(button(true)), bytes(button(true)), bytes(button(false))},
               {bytes(button(true)), bytes(button(true)), bytes(button(false))}, "nonbatchable messages");
  expect_queue({}, {}, "empty queue");
}
int main() {
  static_assert(sizeof(short) == 2);
  arithmetic_tests();
  queue_tests();
  if (failures) { std::cerr << failures << " batching checks failed\n"; return 1; }
  std::cout << "Production input batching and dispatch checks passed\n";
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-file', type=Path, default=ROOT / 'src/input.cpp')
    parser.add_argument('--moonlight-root', type=Path, default=ROOT / 'third-party')
    parser.add_argument('--compiler', default='g++')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    source = args.source_file.read_text(encoding='utf-8')
    signatures = (
        '  enum class batch_result_e {',
        '  batch_result_e batch(PNV_REL_MOUSE_MOVE_PACKET',
        '  batch_result_e batch(PNV_ABS_MOUSE_MOVE_PACKET',
        '  batch_result_e batch(PNV_SCROLL_PACKET',
        '  batch_result_e batch(PSS_HSCROLL_PACKET',
        '  batch_result_e batch(PNV_MULTI_CONTROLLER_PACKET',
        '  batch_result_e batch(PSS_TOUCH_PACKET',
        '  batch_result_e batch(PSS_PEN_PACKET',
        '  batch_result_e batch(PSS_CONTROLLER_TOUCH_PACKET',
        '  batch_result_e batch(PSS_CONTROLLER_MOTION_PACKET',
        '  batch_result_e batch(PNV_INPUT_HEADER',
        '  void passthrough_next_message(std::shared_ptr<input_t> input)',
    )
    bodies = [function(source, signature) for signature in signatures]
    bodies[0] += ';'
    with tempfile.TemporaryDirectory(prefix='input-batching-') as temporary:
        build = Path(temporary)
        cpp, executable = build / 'test.cpp', build / 'test'
        cpp.write_text(PRELUDE + '\n'.join(bodies) + TESTS, encoding='utf-8')
        flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else []
        subprocess.run([args.compiler, '-std=c++23', '-O0', '-g', '-Wall', '-Wextra', '-Werror',
                        *flags, '-I', str(ROOT), '-I', str(args.moonlight_root),
                        str(cpp), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == '__main__':
    main()
