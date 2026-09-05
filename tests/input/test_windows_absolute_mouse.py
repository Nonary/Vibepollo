#!/usr/bin/env python3
"""Exercise production client mapping and Windows mouse injection without Win32.

The stubs capture SendInput arguments; they do not implement coordinate mapping.
--platform-source can point to an older Windows input.cpp for regression checks.
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
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
using namespace std::literals;
// Select the production Windows coordinate contract after including host headers.
#undef __linux__
#define BOOST_LOG(level) std::cerr
struct port_t {
  int offset_x = 0, offset_y = 0, width = 0, height = 0;
  float client_offsetX = 0, client_offsetY = 0, scalar_inv = 1, scalar_tpcoords = 1;
  explicit operator bool() const { return width && height; }
};
struct event_t {
  bool peek() const { return false; }
  std::optional<port_t> pop() const { return std::nullopt; }
};
struct input_t {
  std::shared_ptr<event_t> touch_port_event = std::make_shared<event_t>();
  port_t touch_port;
};
constexpr int INPUT_MOUSE = 0;
constexpr int MOUSEEVENTF_MOVE = 0x0001, MOUSEEVENTF_ABSOLUTE = 0x8000,
              MOUSEEVENTF_VIRTUALDESK = 0x4000;
struct INPUT { int type; struct { long dx, dy; int dwFlags; } mi; } last_input;
void send_input(const INPUT &i) { last_input = i; }
namespace platf {
  using input_t = int;
  struct touch_port_t { int offset_x, offset_y, width, height; };
  constexpr touch_port_t target_touch_port {0, 0, 65535, 65535};
}
'''

TESTS = r'''
int main() {
  struct case_t {
    const char *name;
    int offset_x, offset_y, desktop_w, desktop_h, monitor_w, monitor_h;
    int client_w, client_h;
    float x, y;
    int expected_x, expected_y;
  };
  const case_t cases[] {
    {"reported_right_virtual_display", 1920, 0, 3200, 1080, 1280, 720,
     1280, 720, 640, 360, 2560, 360},
    {"below_primary", 0, 1080, 1920, 1800, 1280, 720,
     1280, 720, 640, 360, 640, 1440},
    {"single_monitor", 0, 0, 1920, 1080, 1920, 1080,
     1920, 1080, 960, 540, 960, 540},
    // display_base already subtracts SM_X/YVIRTUALSCREEN: the left/top
    // monitor starts at zero and the primary monitor has a positive offset.
    {"monitor_left_of_primary", 0, 0, 3200, 1080, 1280, 720,
     1280, 720, 640, 360, 640, 360},
    {"primary_with_monitor_left", 1280, 0, 3200, 1080, 1920, 1080,
     1920, 1080, 960, 540, 2240, 540},
    {"primary_with_monitor_above", 0, 720, 1920, 1800, 1920, 1080,
     1920, 1080, 960, 540, 960, 1260},
    {"scaled_stream", 1920, 0, 4480, 1440, 2560, 1440,
     1280, 720, 640, 360, 3200, 720},
    {"letterboxed_stream", 1920, 0, 3200, 1080, 1280, 720,
     1280, 1024, 640, 512, 2560, 360},
    {"letterbox_top_clamped", 1920, 0, 3200, 1080, 1280, 720,
     1280, 1024, 640, 0, 2560, 0},
    {"monitor_origin", 1920, 0, 3200, 1080, 1280, 720,
     1280, 720, 0, 0, 1920, 0},
  };
  int failures = 0;
  int platform_input = 0;
  for (const auto &test : cases) {
    auto input = std::make_shared<input_t>();
    const float scalar = std::min(float(test.client_w) / test.monitor_w,
                                  float(test.client_h) / test.monitor_h);
    input->touch_port = {
      test.offset_x, test.offset_y, test.client_w, test.client_h,
      (test.client_w - scalar * test.monitor_w) * 0.5f,
      (test.client_h - scalar * test.monitor_h) * 0.5f,
      1.0f / scalar, 1.0f,
    };
    const auto coords = client_to_touchport(input, {test.x, test.y},
                                           {test.client_w, test.client_h});
    assert(coords);
    platf::abs_mouse(platform_input,
                     {test.offset_x, test.offset_y, test.desktop_w, test.desktop_h},
                     coords->first, coords->second);
    // Decode SendInput's normalized virtual-desktop coordinates to pixels.
    const float actual_x = last_input.mi.dx * test.desktop_w / 65535.0f;
    const float actual_y = last_input.mi.dy * test.desktop_h / 65535.0f;
    const bool ok = std::abs(actual_x - test.expected_x) < 0.1f &&
                    std::abs(actual_y - test.expected_y) < 0.1f &&
                    last_input.type == INPUT_MOUSE &&
                    last_input.mi.dwFlags == (MOUSEEVENTF_MOVE |
                      MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK);
    std::cout << (ok ? "PASS: " : "FAIL: ") << test.name << " got "
              << actual_x << ',' << actual_y << '\n';
    failures += !ok;
  }
  platf::move_mouse(platform_input, -17, 29);
  const bool relative_ok = last_input.mi.dx == -17 && last_input.mi.dy == 29 &&
                           last_input.mi.dwFlags == MOUSEEVENTF_MOVE;
  std::cout << (relative_ok ? "PASS: " : "FAIL: ") << "relative_motion_unchanged\n";
  return failures + !relative_ok ? 1 : 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform-source', type=Path,
                        default=ROOT / 'src/platform/windows/input.cpp')
    parser.add_argument('--input-source', type=Path, default=ROOT / 'src/input.cpp')
    args = parser.parse_args()
    common = function(args.input_source.read_text(),
                      'std::optional<std::pair<float, float>> client_to_touchport(')
    platform = args.platform_source.read_text()
    handlers = '\n'.join(function(platform, signature) for signature in (
        'void abs_mouse(', 'void move_mouse(',
    ))
    with tempfile.TemporaryDirectory(prefix='windows-absolute-mouse-') as temp:
        cpp, binary = Path(temp) / 'test.cpp', Path(temp) / 'test'
        cpp.write_text(PRELUDE + common + '\nnamespace platf {\n' + handlers + '\n}\n' + TESTS)
        subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter',
                        str(cpp), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)]).returncode


if __name__ == '__main__':
    raise SystemExit(main())
