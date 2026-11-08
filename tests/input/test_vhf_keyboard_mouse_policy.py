"""Exercise the host VHF key mapping and ordered relative-input policy."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = r'''
#include "src/platform/windows/vhf_keyboard_mouse_policy.h"
#include "src/input_scroll_policy.h"
#include <cassert>
#include <vector>
using namespace platf::vhf_keyboard_mouse;
int main() {
  // Single monitor, and a second monitor to the left of the primary: the HID
  // absolute collection targets primary coordinates, not the whole desktop.
  auto primary = map_primary_absolute(0, 0, 0, 0, 1920, 1080, 1920, 1080);
  assert(primary.inside_primary && primary.x == 0 && primary.y == 0);
  primary = map_primary_absolute(65535, 65535, 0, 0, 1920, 1080, 1920, 1080);
  assert(primary.inside_primary && primary.x == 65535 && primary.y == 65535);
  auto secondary = map_primary_absolute(0, 32768, -1920, 0, 3840, 1080, 1920, 1080);
  assert(!secondary.inside_primary);
  primary = map_primary_absolute(65535, 65535, -1920, 0, 3840, 1080, 1920, 1080);
  assert(primary.inside_primary && primary.x == 65535 && primary.y == 65535);
  assert(!map_primary_absolute(0, 0, 0, 0, 0, 1080, 1920, 1080).inside_primary);
  assert(key_usage('A') == 4 && key_usage('Z') == 29);
  assert(key_usage('1') == 30 && key_usage('0') == 39);
  assert(key_usage(0xa0) == 0xe1 && key_usage(0xa1) == 0xe5);
  assert(key_usage(0xa2) == 0xe0 && key_usage(0xa3) == 0xe4);
  assert(key_usage(0xa4) == 0xe2 && key_usage(0xa5) == 0xe6);
  assert(key_usage(0x0d) == 0x28 && key_usage(0x010d) == 0x58);
  assert(key_usage(0x25) == 0x50 && key_usage(0x6f) == 0x54);
  assert(key_usage(0xad) == 0); // Volume mute uses the fallback path.
  assert(consumer_usage(0xad) == 0xe2 && consumer_usage(0xae) == 0xea);
  assert(consumer_usage(0xaf) == 0xe9 && consumer_usage(0xb3) == 0xcd);
  assert(consumer_usage(0x41) == 0);
  assert(scan_usage(0x1e) == 4 && scan_usage(0x2c) == 29);
  assert(scan_usage(0x1d) == 0xe0 && scan_usage(0xe01d) == 0xe4);
  assert(scan_usage(0x1c) == 0x28 && scan_usage(0xe01c) == 0x58);
  assert(scan_usage(0x48) == 0x60 && scan_usage(0xe048) == 0x52);
  assert(scan_usage(0xe077) == 0);
  std::uint16_t route = 0;
  assert(pin_key_route(route, false, 4) == 4);
  assert(pin_key_route(route, false, 29) == 4); // Layout changed on repeat.
  assert(pin_key_route(route, true, 0) == 4 && route == 0);
  assert(pin_key_route(route, false, 0) == win32_fallback);
  assert(pin_key_route(route, false, 4) == win32_fallback);
  assert(pin_key_route(route, true, 4) == win32_fallback && route == 0);
  keyboard_routes keys;
  auto first = keys.update(0x0d, false, 0x28);
  auto second = keys.update(0x010d, false, 0x58);
  assert(first.emit && first.route == 0x28 && second.emit && second.route == 0x58);
  assert(keys.update(0x0d, true, 0).route == 0x28);
  assert(keys.update(0x010d, true, 0).route == 0x58);
  first = keys.update(0x41, false, 0x04);
  second = keys.update(0x51, false, 0x04); // Different VK, same physical key.
  assert(first.emit && !second.emit);
  assert(!keys.update(0x41, false, 0x04).emit); // Repeat does not own again.
  assert(!keys.update(0x41, true, 0).emit);
  assert(keys.update(0x51, true, 0).emit);
  first = keys.update(0xad, false, consumer_route | consumer_usage(0xad));
  second = keys.update(0xad, true, 0);
  assert(first.emit && second.emit && first.route == (consumer_route | 0xe2) && second.route == first.route);
  // On confirmed child removal an already-held HID key must not have its
  // subsequent repeat/up replayed through SendInput. A fallback key still
  // receives its Win32 up, and a fresh key remains usable.
  keys.update(0x41, false, 0x04);
  keys.update(0x42, false, 0);
  assert(consume_removed_key(keys, 0x41, false));
  assert(consume_removed_key(keys, 0x41, true));
  assert(!consume_removed_key(keys, 0x41, false));
  assert(!consume_removed_key(keys, 0x42, true));
  std::uint8_t held_buttons = 1;
  assert(consume_removed_button(held_buttons, 1, false));
  assert(consume_removed_button(held_buttons, 1, true));
  assert(!consume_removed_button(held_buttons, 1, false));
  assert(mouse_button(1) == 1 && mouse_button(2) == 3 && mouse_button(3) == 2);
  assert(mouse_button(4) == 4 && mouse_button(5) == 5 && mouse_button(6) == 0);
  assert(!retain_control_handle(0, false));
  assert(retain_control_handle(1, false));
  assert(retain_control_handle(0, true));

  constexpr unsigned busy = 7, lost = 9;
  int calls = 0, waits = 0;
  assert(submit_with_backpressure([&] { return ++calls < 3 ? busy : 0u; },
                                  [&] { ++waits; }, busy) == 0);
  assert(calls == 3 && waits == 2);
  calls = waits = 0;
  assert(submit_with_backpressure([&] { ++calls; return lost; },
                                  [&] { ++waits; }, busy) == lost);
  assert(calls == 1 && waits == 0); // An uncertain result is never replayed.
  calls = waits = 0;
  assert(submit_with_backpressure([&] { ++calls; return busy; },
                                  [&] { ++waits; }, busy) == busy);
  assert(calls == 20 && waits == 19);

  std::int32_t first_client = 0, second_client = 0;
  assert(input::scroll_policy::accumulate(first_client, 60) == 0);
  assert(input::scroll_policy::accumulate(second_client, 60) == 0);
  assert(first_client == 60 && second_client == 60);
  assert(input::scroll_policy::accumulate(first_client, 60) == 120);
  assert(first_client == 0 && second_client == 60);
  second_client = 0; // Disconnect discards this client's partial detent.
  assert(input::scroll_policy::accumulate(second_client, -60) == 0);
  assert(input::scroll_policy::accumulate(second_client, -60) == -120);
  std::vector<relative_fragment> sent;
  auto push = [&](relative_fragment part) { sent.push_back(part); return true; };
  assert(send_relative(40000, -40000, 1, -1, push));
  assert(sent.size() == 2 && sent[0].x == 32767 && sent[0].y == -32768);
  assert(sent[0].wheel == 1 && sent[0].pan == -1);
  assert(sent[1].x == 7233 && sent[1].y == -7232);
  sent.clear();
  assert(!send_relative(100000, 0, 0, 0, [&](relative_fragment part) {
    sent.push_back(part);
    return false;
  }));
  assert(sent.size() == 1); // Do not send subsequent pieces after a failure.
}
'''

with tempfile.TemporaryDirectory() as directory:
    source = Path(directory) / 'test.cpp'
    program = Path(directory) / 'test.exe'
    source.write_text(SOURCE, encoding='utf-8')
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-I', str(ROOT), str(source), '-o', str(program)], check=True)
    subprocess.run([str(program)], check=True)
print('VHF keyboard/mouse policy tests passed')
