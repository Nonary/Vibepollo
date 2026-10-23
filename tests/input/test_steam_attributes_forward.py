#!/usr/bin/env python3
"""Compile the production Steam Controller attribute relay with fake driver and slot boundaries.

The client reads the real controller's firmware attributes at link setup and sends them after
the arrival event. input.cpp decodes the packet, gates it on that arrival (LI_CTYPE_STEAM with
LI_CCAP_STEAM_ATTRIBUTES) and hands it to the platform; the VHF backend maps it to the driver
call, treats ERROR_REVISION_MISMATCH as an old driver and logs the rest. The decode, the gate,
the relay and the policy are the production functions; only the driver client, the slot table
and the log are faked. Requires the moonlight-common-c and libvirtualgamepad submodules.
--baseline reads HEAD.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / 'third-party/libvirtualgamepad'
WARNING = 'Do not start a Steam Controller firmware update from Steam while streaming'


def read(name):
    return (subprocess.check_output(['git', 'show', 'HEAD:' + name], cwd=ROOT, text=True)
            if '--baseline' in sys.argv else (ROOT / name).read_text(encoding='utf-8'))


def block(text, marker):
    start = text.index(marker)
    pos = text.index('{', start)
    depth = 1
    end = pos + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def struct(text, name):
    return block(text, f'  struct {name} {{') + ';\n'


inp = read('src/input.cpp')
common = read('src/platform/common.h')
vhf = read('src/platform/windows/vhf_gamepad.cpp')
client_h = (DRIVER / 'include/libvirtualgamepad/client.h').read_text(encoding='utf-8')

platf_types = ''.join(struct(common, n) for n in ('gamepad_id_t', 'gamepad_arrival_t', 'steam_controller_attributes_t'))
lvg_attrs = block(client_h, 'struct steam_controller_attributes {') + ';\n'
relay = block(vhf, '  void vhf_gamepad_t::attributes(').replace('vhf_gamepad_t::', '')
decode = '\n'.join(block(inp, m) for m in (
    '  std::optional<packet_size_bounds_t> packet_size_bounds(',
    '  void copy_steam_attribute_string(',
    '  void forward_steam_attributes(',
    '  void passthrough(std::shared_ptr<input_t> &input, PSS_CONTROLLER_ARRIVAL_PACKET packet)',
    '  void passthrough(std::shared_ptr<input_t> &input, PSS_STEAM_CONTROLLER_ATTRIBUTES_PACKET packet)',
))
size_types = struct(inp, 'validated_input_packet_t') + struct(inp, 'packet_size_bounds_t')

code = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <bitset>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include "src/steam_controller_protocol.h"
#include "src/platform/windows/vhf_gamepad_policy.h"
@LVG_ATTRS@
using namespace std::literals;

struct log_entry_t { std::string level, text; };
std::vector<log_entry_t> logs;
struct log_sink_t {
  std::string level; std::ostringstream text;
  ~log_sink_t() { logs.push_back({level, text.str()}); }
  template<class T> log_sink_t &operator<<(const T &v) { text << v; return *this; }
};
#define BOOST_LOG(level) log_sink_t{#level}
namespace util {
  struct hex_t { std::string value; std::string to_string_view() const { return value; } };
  template<class T> hex_t hex(T v) { std::ostringstream s; s << std::hex << (unsigned long long) v; return {s.str()}; }
  namespace endian { template<class T> T little(T v) { static_assert(std::endian::native == std::endian::little); return v; } }
}
namespace config { struct { bool controller = true; } input; }
constexpr int MAX_GAMEPADS = 16;

namespace platf {
@PLATF_TYPES@
  // The slot table and driver client vhf_gamepad_t::attributes() runs against.
  struct slot_t {
    bool active {}; lvg::profile profile {}; std::uint16_t client_capabilities {};
    // The relay keeps the last attributes per slot for replay after a device-removal recovery.
    std::optional<lvg::steam_controller_attributes> steam_attributes;
  };
  struct driver_call_t { std::uint32_t controller_id; lvg::steam_controller_attributes attributes; };
  struct fake_client_t {
    DWORD status = ERROR_SUCCESS;
    std::vector<driver_call_t> calls;
    DWORD set_steam_controller_attributes(std::uint32_t id, const lvg::steam_controller_attributes &a) { calls.push_back({id, a}); return status; }
  };
  // No device-removal recovery is ever pending here: every relay reaches the driver client.
  struct recovery_t { bool pending() const { return false; } };
  struct impl_t {
    std::shared_mutex lifetime; std::array<slot_t, MAX_GAMEPADS> slots; fake_client_t client;
    recovery_t recovery;
    void observe_status(DWORD) {}
  };
  impl_t impl_storage; impl_t *impl = &impl_storage;
@RELAY@
  // The platform boundary input.cpp calls; the Windows backend routes it to the VHF slot.
  std::vector<steam_controller_attributes_t> forwarded;
  void gamepad_attributes(int, const steam_controller_attributes_t &a) { forwarded.push_back(a); attributes(a.id.globalIndex, a); }
  int alloc_result = 0, alloc_calls = 0;
  int alloc_gamepad(int, gamepad_id_t id, const gamepad_arrival_t &arrival, int) {
    ++alloc_calls;
    if (alloc_result) return alloc_result;
    auto &slot = impl->slots[id.globalIndex];
    slot.active = true;
    slot.profile = arrival.type == LI_CTYPE_STEAM ? lvg::profile::steam_controller : lvg::profile::xbox_series;
    slot.client_capabilities = arrival.capabilities;
    return 0;
  }
}
int platf_input;
std::bitset<MAX_GAMEPADS> gamepadMask;
int alloc_id(std::bitset<MAX_GAMEPADS> &mask) { for (int i = 0; i < MAX_GAMEPADS; ++i) if (!mask[i]) { mask[i] = true; return i; } return -1; }
void free_id(std::bitset<MAX_GAMEPADS> &mask, int id) { mask[id] = false; }
struct gamepad_t {
  int id = -1;
  bool alloc_failed {};
  std::uint8_t arrival_type {};
  std::uint16_t arrival_capabilities {};
  std::optional<platf::steam_controller_attributes_t> pending_steam_attributes;
};
struct input_t {
  std::vector<gamepad_t> gamepads {MAX_GAMEPADS};
  std::atomic_uint32_t allocated_gamepads {0};
  // Stands in for controller_speaker_pcm::owner_bindings: no speaker binding is ever queued here.
  struct { std::uint64_t take(std::uint8_t) { return 0; } } speaker_bindings;
  bool persist_gamepads = false;
  int feedback_queue = 0;
};
@SIZE_TYPES@
@DECODE@

SS_CONTROLLER_ARRIVAL_PACKET arrival_packet(std::uint8_t n, std::uint8_t type, std::uint16_t caps) {
  SS_CONTROLLER_ARRIVAL_PACKET p {}; p.controllerNumber = n; p.type = type; p.capabilities = caps; p.supportedButtonFlags = 0xFFFF; return p;
}
SS_STEAM_CONTROLLER_ATTRIBUTES_PACKET attributes_packet(std::uint8_t n, std::uint8_t transport = LI_SCATTR_TRANSPORT_BLE, std::uint8_t mask = 0xFF) {
  SS_STEAM_CONTROLLER_ATTRIBUTES_PACKET p {};
  p.header.magic = SS_STEAM_CONTROLLER_ATTRIBUTES_MAGIC;
  p.controllerNumber = n; p.transport = transport; p.validMask = mask;
  p.firmwareBuildTime = 0x6A4D85E3; p.bootloaderBuildTime = 0x68D2F92E; p.boardRevision = 0x4A; p.capabilities = 3;
  p.radioFirmwareBuildTime = 0x6A4D0000; p.dongleFirmwareBuildTime = 0;
  std::memcpy(p.buildId, "26e1f7ef1955", 13);
  const char odd[16] = {'F', 'X', 'A', '\t', 'Q', static_cast<char>(0xC3), '7', '\0', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'};
  std::memcpy(p.unitSerial, odd, 16);
  std::memcpy(p.boardSerial, "MXA0123456789ABC", 16);  // no terminator anywhere in the field
  return p;
}
int main() {
  int failures = 0;
  auto check = [&](bool ok, const char *msg) { std::cout << (ok ? "PASS " : "FAIL ") << msg << '\n'; failures += !ok; };
  auto count_logs = [&](const char *level, const char *needle) {
    int n = 0; for (auto &l : logs) if (l.level == level && l.text.find(needle) != std::string::npos) ++n; return n;
  };
  auto reset = [&] { logs.clear(); platf::forwarded.clear(); platf::impl->client.calls.clear(); platf::impl->client.status = ERROR_SUCCESS;
                     for (auto &s : platf::impl->slots) s = {}; gamepadMask.reset(); platf::alloc_result = 0; platf::alloc_calls = 0; };

  const auto bounds = packet_size_bounds(SS_STEAM_CONTROLLER_ATTRIBUTES_MAGIC);
  check(bounds && bounds->min_total_size == 84 && bounds->max_total_size == 84 && sizeof(SS_STEAM_CONTROLLER_ATTRIBUTES_PACKET) == 84,
        "the size table admits exactly the 84-byte attributes packet");

  // The normal order: arrival, then attributes, for a Steam Controller client that reads them.
  reset();
  auto input = std::make_shared<input_t>();
  auto arrival = arrival_packet(2, LI_CTYPE_STEAM, LI_CCAP_STEAM_ATTRIBUTES | LI_CCAP_STEAM_SETTINGS);
  passthrough(input, &arrival);
  auto attrs = attributes_packet(2);
  passthrough(input, &attrs);
  bool decoded = platf::forwarded.size() == 1 && platf::forwarded[0].id.globalIndex == input->gamepads[2].id &&
                 platf::forwarded[0].id.clientRelativeIndex == 2 && platf::forwarded[0].transport == LI_SCATTR_TRANSPORT_BLE &&
                 platf::forwarded[0].valid_mask == 0xFF && platf::forwarded[0].firmware_build_time == 0x6A4D85E3 &&
                 platf::forwarded[0].bootloader_build_time == 0x68D2F92E && platf::forwarded[0].board_revision == 0x4A &&
                 platf::forwarded[0].capabilities == 3 && platf::forwarded[0].radio_firmware_build_time == 0x6A4D0000;
  check(decoded, "a packet after a Steam arrival with LI_CCAP_STEAM_ATTRIBUTES reaches the platform with its fields decoded");
  bool sanitised = decoded && std::string(platf::forwarded[0].build_id.data()) == "26e1f7ef1955" &&
                   std::string(platf::forwarded[0].unit_serial.data()) == "FXA?Q?7" &&
                   std::string(platf::forwarded[0].board_serial.data()) == "MXA0123456789AB" && platf::forwarded[0].board_serial[15] == '\0';
  check(sanitised, "strings keep at most 15 printable ASCII characters and are NUL terminated");
  bool relayed = platf::impl->client.calls.size() == 1 && platf::impl->client.calls[0].controller_id == (unsigned) input->gamepads[2].id &&
                 platf::impl->client.calls[0].attributes.flags == 0xCF && platf::impl->client.calls[0].attributes.firmware_build_time == 0x6A4D85E3 &&
                 platf::impl->client.calls[0].attributes.board_revision == 0x4A &&
                 std::string(platf::impl->client.calls[0].attributes.build_id) == "26e1f7ef1955" &&
                 std::string(platf::impl->client.calls[0].attributes.unit_serial) == "FXA?Q?7" &&
                 std::string(platf::impl->client.calls[0].attributes.board_serial) == "MXA0123456789AB";
  check(relayed, "the VHF slot hands the driver the six fields it takes, flags straight through, radio/dongle times left out");
  check(count_logs("info", "real controller firmware build 2026-07-07 23:04:03 UTC (0x6a4d85e3), bootloader 2025-09-23 19:46:54 UTC (0x68d2f92e), board rev 74, build id 26e1f7ef1955, serial FXA?Q?7, via ble, radio firmware build") == 1,
        "one info line names the real controller's firmware build, bootloader, board, build id, serial and transport");
  check(count_logs("warning", "") == 0, "a successful apply warns about nothing");

  // Steam re-polls; a later packet for the same live slot is applied again.
  passthrough(input, &attrs);
  check(platf::impl->client.calls.size() == 2, "a repeated packet for a live slot is relayed again");

  // Old driver: one info line, no warning, nothing else changes.
  reset(); logs.clear();
  input = std::make_shared<input_t>();
  passthrough(input, &arrival);
  platf::impl->client.status = ERROR_REVISION_MISMATCH;
  passthrough(input, &attrs);
  check(platf::impl->client.calls.size() == 1 && count_logs("info", "driver predates firmware attributes") == 1 && count_logs("warning", "") == 0,
        "ERROR_REVISION_MISMATCH is one info line (the driver predates the call) and no warning");
  platf::impl->client.status = ERROR_NOT_FOUND;
  passthrough(input, &attrs);
  check(count_logs("warning", "couldn't apply the real controller's firmware attributes") == 1, "any other driver error is a warning");

  // The gate: not a Steam Controller, or a Steam client that never said it sends attributes.
  reset();
  input = std::make_shared<input_t>();
  auto xbox = arrival_packet(0, LI_CTYPE_XBOX, LI_CCAP_STEAM_ATTRIBUTES);
  passthrough(input, &xbox);
  auto attrs0 = attributes_packet(0);
  passthrough(input, &attrs0);
  check(platf::forwarded.empty() && platf::impl->client.calls.empty(), "a packet for a controller not announced as LI_CTYPE_STEAM is dropped");
  auto quiet = arrival_packet(1, LI_CTYPE_STEAM, LI_CCAP_STEAM_SETTINGS | LI_CCAP_STEAM_HAPTIC);
  passthrough(input, &quiet);
  auto attrs1 = attributes_packet(1);
  passthrough(input, &attrs1);
  check(platf::forwarded.empty() && platf::impl->client.calls.empty(), "a packet from a Steam client without LI_CCAP_STEAM_ATTRIBUTES is dropped");

  // The VHF layer gates on its own slot state too (a slot can change hands between threads).
  reset();
  platf::steam_controller_attributes_t direct {}; direct.id = {5, 5}; direct.valid_mask = 0xFF;
  platf::impl->slots[5] = {true, lvg::profile::dualsense, LI_CCAP_STEAM_ATTRIBUTES};
  platf::attributes(5, direct);
  platf::impl->slots[5] = {true, lvg::profile::steam_controller, 0};
  platf::attributes(5, direct);
  platf::impl->slots[5] = {false, lvg::profile::steam_controller, LI_CCAP_STEAM_ATTRIBUTES};
  platf::attributes(5, direct);
  platf::attributes(-1, direct); platf::attributes(MAX_GAMEPADS, direct);
  check(platf::impl->client.calls.empty(), "the VHF slot never calls the driver for another profile, a client without the capability, an inactive slot or a bad index");

  // Early packet: held until the arrival says what the controller is.
  reset();
  input = std::make_shared<input_t>();
  auto attrs3 = attributes_packet(3);
  passthrough(input, &attrs3);
  check(platf::forwarded.empty() && input->gamepads[3].pending_steam_attributes.has_value(), "a packet before the arrival is held, not forwarded");
  auto arrival3 = arrival_packet(3, LI_CTYPE_STEAM, LI_CCAP_STEAM_ATTRIBUTES);
  passthrough(input, &arrival3);
  check(platf::forwarded.size() == 1 && platf::impl->client.calls.size() == 1 && !input->gamepads[3].pending_steam_attributes &&
        platf::forwarded[0].id.globalIndex == input->gamepads[3].id, "the held packet is applied once the Steam arrival allocates the slot");
  auto attrs4 = attributes_packet(4);
  passthrough(input, &attrs4);
  auto arrival4 = arrival_packet(4, LI_CTYPE_PS, LI_CCAP_STEAM_ATTRIBUTES);
  passthrough(input, &arrival4);
  check(platf::forwarded.size() == 1 && !input->gamepads[4].pending_steam_attributes, "a held packet is dropped when the arrival is not a Steam Controller");

  // A failed arrival ends the lifetime of any attributes held before it. Retrying
  // that controller number must not identify the new virtual device as the old one.
  reset();
  input = std::make_shared<input_t>();
  auto arrival6 = arrival_packet(6, LI_CTYPE_STEAM, LI_CCAP_STEAM_ATTRIBUTES);
  auto attrs6 = attributes_packet(6);
  passthrough(input, &attrs6);
  platf::alloc_result = -1;
  passthrough(input, &arrival6);
  check(input->gamepads[6].alloc_failed && !input->gamepads[6].pending_steam_attributes,
        "a platform allocation failure discards attributes held before arrival");
  platf::alloc_result = 0;
  passthrough(input, &arrival6);
  check(input->gamepads[6].id >= 0 && platf::forwarded.empty(),
        "retrying after platform allocation failure cannot replay the failed arrival's attributes");
  const auto before_retry_attributes = platf::forwarded.size();
  passthrough(input, &attrs6);
  check(platf::forwarded.size() == before_retry_attributes + 1, "the successful retry accepts its own attribute reading");

  // Global controller IDs can be occupied by other streaming sessions. This is
  // an allocation failure too, rather than an arrival that has not happened yet.
  reset();
  input = std::make_shared<input_t>();
  passthrough(input, &attrs6);
  gamepadMask.set();
  passthrough(input, &arrival6);
  check(input->gamepads[6].alloc_failed && !input->gamepads[6].pending_steam_attributes && platf::alloc_calls == 0,
        "exhausting global controller IDs discards pre-arrival attributes and records failure");
  passthrough(input, &attrs6);
  check(!input->gamepads[6].pending_steam_attributes && platf::forwarded.empty(),
        "attributes following an arrival rejected for exhausted IDs are dropped");
  gamepadMask.reset(9);
  passthrough(input, &arrival6);
  check(input->gamepads[6].id == 9 && !input->gamepads[6].alloc_failed && platf::forwarded.empty(),
        "a later arrival after capacity returns cannot inherit the failed controller's attributes");

  // Nothing to hold for a slot whose allocation failed; nothing at all when controllers are off or the number is out of range.
  reset();
  input = std::make_shared<input_t>();
  platf::alloc_result = -1;
  passthrough(input, &arrival6);
  passthrough(input, &attrs6);
  check(input->gamepads[6].alloc_failed && !input->gamepads[6].pending_steam_attributes && platf::forwarded.empty(), "a packet for a slot whose allocation failed is dropped, not held");
  platf::alloc_result = 0;
  auto far = attributes_packet(MAX_GAMEPADS);
  passthrough(input, &far);
  check(platf::forwarded.empty() && count_logs("warning", "out of range") == 1, "an out-of-range controller number is refused");
  config::input.controller = false;
  passthrough(input, &arrival6);
  passthrough(input, &attrs6);
  check(platf::forwarded.empty() && platf::alloc_calls == 1, "nothing is decoded while controller input is disabled");
  return failures != 0;
}
'''.replace('@PLATF_TYPES@', platf_types).replace('@RELAY@', relay).replace('@SIZE_TYPES@', size_types).replace('@DECODE@', decode)
code = code.replace('@LVG_ATTRS@', 'namespace lvg {\n' + lvg_attrs + '}\n')

# The user-facing warning is one sentence everywhere it appears.
for name in ('src/platform/windows/vhf_gamepad.cpp', 'docs/configuration.md', 'docs/steam_controller.md',
             'src_assets/common/assets/web/public/assets/locale/en.json',
             'src_assets/common/assets/web-legacy/public/assets/locale/en.json'):
    present = WARNING in read(name)
    print(('PASS ' if present else 'FAIL ') + f'firmware-update warning present in {name}')
    if not present:
        raise SystemExit(1)

with tempfile.TemporaryDirectory(prefix='steam-attrs-') as directory:
    work = Path(directory)
    (work / 'windows.h').write_text('#pragma once\n#include <cstdint>\nusing DWORD=std::uint32_t;\nstruct GUID {std::uint32_t Data1; std::uint16_t Data2,Data3; std::uint8_t Data4[8];};\n'
                                    'constexpr DWORD ERROR_SUCCESS=0, ERROR_NOT_FOUND=1168, ERROR_REVISION_MISMATCH=1306;\n')
    (work / 'winioctl.h').write_text('#pragma once\n#define FILE_DEVICE_UNKNOWN 0x22\n#define METHOD_BUFFERED 0\n#define FILE_READ_DATA 1\n#define FILE_WRITE_DATA 2\n#define CTL_CODE(d,f,m,a) (((d)<<16)|((a)<<14)|((f)<<2)|(m))\n')
    (work / 'test.cpp').write_text(code)
    subprocess.run(['g++', '-std=c++20', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    f'-I{work}', f'-I{ROOT}', f'-I{ROOT / "third-party"}', f'-I{DRIVER / "include"}',
                    str(work / 'test.cpp'), str(ROOT / 'src/platform/windows/vhf_gamepad_policy.cpp'),
                    '-o', str(work / 'test')], check=True)
    result = subprocess.run([str(work / 'test')], capture_output=True, text=True, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))
    print(result.stdout, end='')
    if result.returncode:
        print(result.stderr[:4000], file=sys.stderr)
    raise SystemExit(result.returncode)
