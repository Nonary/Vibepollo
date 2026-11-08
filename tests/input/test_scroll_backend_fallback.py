#!/usr/bin/env python3
"""Exercise the real scroll handlers across a VHF-to-SendInput fallback."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'src/input.cpp').read_text(encoding='utf-8')


def block(marker):
    start = source.index(marker)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


prefix = r'''
#ifndef _WIN32
#define _WIN32 1
#endif
#include "src/input_scroll_policy.h"
#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>
struct input_t { std::int32_t accumulated_vscroll_delta = 0, accumulated_hscroll_delta = 0; };
struct nv_scroll { int scrollAmt1; }; using PNV_SCROLL_PACKET = nv_scroll *;
struct ss_scroll { int scrollAmount; }; using PSS_HSCROLL_PACKET = ss_scroll *;
namespace config { struct { bool mouse = true, high_resolution_scrolling = true; } input; }
namespace util::endian { int big(int value) { return value; } }
namespace platf {
  struct input_t {};
  bool vhf = true;
  std::vector<int> vertical, horizontal;
  bool mouse_requires_scroll_ticks(input_t &) { return vhf; }
  void scroll(input_t &, int delta) { vertical.push_back(delta); }
  void hscroll(input_t &, int delta) { horizontal.push_back(delta); }
}
platf::input_t platf_input;
namespace scroll_policy = input::scroll_policy;
'''

main = r'''
int main() {
  auto first = std::make_shared<input_t>();
  auto second = std::make_shared<input_t>();
  nv_scroll v {60}; ss_scroll h {-40};
  passthrough(first, &v); passthrough(first, &h);
  v.scrollAmt1 = 30; passthrough(second, &v);
  assert(platf::vertical.empty() && platf::horizontal.empty());
  // The VHF child fails and the active backend changes to high-resolution
  // SendInput. Its unsubmitted fractions must join the next packet for each
  // client and axis, rather than being lost or combined across clients.
  platf::vhf = false;
  v.scrollAmt1 = 15; passthrough(first, &v);
  h.scrollAmount = 10; passthrough(first, &h);
  v.scrollAmt1 = 90; passthrough(second, &v);
  assert((platf::vertical == std::vector<int>{75, 120}));
  assert((platf::horizontal == std::vector<int>{-30}));
  assert(first->accumulated_vscroll_delta == 0 && first->accumulated_hscroll_delta == 0);
  assert(second->accumulated_vscroll_delta == 0);
  // Subsequent high-resolution packets do not re-send the saved fraction.
  v.scrollAmt1 = 5; passthrough(first, &v);
  assert((platf::vertical == std::vector<int>{75, 120, 5}));
  // Explicit low-resolution mode still emits only full detents after fallback.
  platf::vhf = true; v.scrollAmt1 = 50; passthrough(first, &v);
  platf::vhf = false; config::input.high_resolution_scrolling = false;
  v.scrollAmt1 = 70; passthrough(first, &v);
  assert((platf::vertical == std::vector<int>{75, 120, 5, 120}));
}
'''

program = prefix + block('  void passthrough(std::shared_ptr<input_t> &input, PNV_SCROLL_PACKET packet)') + '\n' + block('  void passthrough(std::shared_ptr<input_t> &input, PSS_HSCROLL_PACKET packet)') + '\n' + main
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'test.cpp'
    exe = Path(tmp) / 'test.exe'
    src.write_text(program, encoding='utf-8')
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT), str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('Scroll backend fallback tests passed')
