"""Execute production VHF update/free/reset adapters with an inert driver boundary."""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2] if len(sys.argv) > 2 else "g++"


def block(source, marker):
    """Compile the real function body, rather than asserting source text exists."""
    start = source.index(marker)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


adapter = (root / "src/platform/windows/vhf_gamepad.cpp").read_text()
policy = (root / "src/platform/windows/vhf_gamepad_policy.cpp").read_text()
program = r'''
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string_view>
#include <thread>
#include <vector>
#include "src/platform/windows/vhf_gamepad_recovery.h"
using DWORD = std::uint32_t;
constexpr DWORD ERROR_SUCCESS = 0, ERROR_DEVICE_REMOVED = 1617;
constexpr int MAX_GAMEPADS = 4;
constexpr unsigned LI_BATTERY_PERCENTAGE_UNKNOWN = 255;
constexpr unsigned LI_BATTERY_STATE_NOT_PRESENT = 1, LI_BATTERY_STATE_DISCHARGING = 2,
  LI_BATTERY_STATE_CHARGING = 3, LI_BATTERY_STATE_FULL = 4, LI_BATTERY_STATE_NOT_CHARGING = 5;
struct logger {
  template<class T> logger &operator<<(const T &) { return *this; }
};
#define BOOST_LOG(level) logger{}
namespace util {
  struct hex { explicit hex(DWORD) {} std::string_view to_string_view() { return {}; } };
}
namespace lvg {
  enum class profile { xbox, dualsense };
  enum class battery_state { unknown, not_present, discharging, charging, full, not_charging };
  struct battery_state_request {
    struct header_t { unsigned size{}, version{}; } header;
    unsigned controller_id{}, percent{}, flags{};
  };
  constexpr unsigned k_protocol_version = 2;
  struct input_state_request {
    struct header_t { unsigned size{}, version{}; } header;
    unsigned controller_id{}, buttons{}, left_trigger{}, right_trigger{};
    int left_x{}, left_y{}, right_x{}, right_y{};
  };
}
namespace platf {
using namespace std::literals;
using feedback_queue_t = std::shared_ptr<int>;
struct gamepad_state_t {
  unsigned buttonFlags{}, lt{}, rt{};
  int lsX{}, lsY{}, rsX{}, rsY{};
};
struct gamepad_battery_t { unsigned percentage{}, state{}; };
bool has_motion(lvg::profile profile) { return profile == lvg::profile::dualsense; }
namespace dualsense_audio { void set_slot(int, bool) {} }
namespace vhf_gamepad {
  struct normalized_state_t {
    unsigned button_flags{}, left_trigger{}, right_trigger{};
    int left_x{}, left_y{}, right_x{}, right_y{};
  };
  constexpr unsigned supported_button_mask = 0xffffffff;
  struct rumble_rgb_t {};
  struct rumble_poll_summary_t {};
  void clear_rumble_poll_summary(rumble_poll_summary_t &) {}
  // REAL_BATTERY_NORMALIZER
  // REAL_MAKE_INPUT
}
// REAL_SLOT
struct client_t {
  std::map<unsigned, lvg::input_state_request> reports;
  std::map<unsigned, lvg::profile> controllers;
  std::map<unsigned, lvg::battery_state_request> batteries;
  unsigned battery_submissions{};
  DWORD battery_error{};
  int fail_battery_slot{-1};
  unsigned submissions{};
  DWORD next_error{};
  int fail_replay_slot{-1};
  DWORD replay_error{ERROR_DEVICE_REMOVED};
  void close() { controllers.clear(); reports.clear(); batteries.clear(); }
  DWORD connect() { return 0; }
  DWORD create_controller(unsigned nr, lvg::profile profile) {
    controllers[nr] = profile;
    batteries.erase(nr);
    reports.erase(nr); // The producer does not preserve input on creation.
    return 0;
  }
  DWORD destroy_controller(unsigned nr) {
    controllers.erase(nr); reports.erase(nr); batteries.erase(nr); return 0;
  }
  DWORD submit_battery_state(const lvg::battery_state_request &request) {
    ++battery_submissions;
    if (battery_error) { const auto status = battery_error; battery_error = 0; return status; }
    if (static_cast<int>(request.controller_id) == fail_battery_slot) { return replay_error; }
    batteries[request.controller_id] = request;
    return 0;
  }
  DWORD submit_input_state(const lvg::input_state_request &request) {
    ++submissions;
    if (next_error) { const auto status = next_error; next_error = 0; return status; }
    if (static_cast<int>(request.controller_id) == fail_replay_slot) { return replay_error; }
    reports[request.controller_id] = request;
    return 0;
  }
};
class vhf_gamepad_t {
public:
  struct impl_t {
    std::shared_mutex lifetime;
    client_t client;
    std::array<slot_t, MAX_GAMEPADS> slots;
    vhf_gamepad::connection_recovery_t recovery;
    std::atomic<unsigned> active_count{};
    bool recovery_failure_logged{};
    // REAL_OBSERVE
  };
  std::unique_ptr<impl_t> impl{std::make_unique<impl_t>()};
  void update(int, const gamepad_state_t &);
  void free(int);
  void battery(int, const gamepad_battery_t &);
  void allocate(int nr) {
    std::unique_lock lock{impl->lifetime};
    auto &slot = impl->slots[nr];
    slot.reset(); // Real reset used by production alloc(), not a test-owned cache reset.
    slot.active = true;
    slot.profile = lvg::profile::dualsense;
    ++impl->active_count;
    impl->client.create_controller(nr, slot.profile);
  }
  auto recover(std::chrono::seconds time) {
    std::unique_lock lock{impl->lifetime};
    return impl->recovery.attempt(impl->client, impl->slots,
      vhf_gamepad::connection_recovery_t::clock::time_point{time},
      [](auto, auto &) {});
  }
};
// REAL_BATTERY
// REAL_UPDATE
// REAL_FREE
}
using namespace platf;
using namespace std::chrono_literals;
void check_report(const lvg::input_state_request &report, const gamepad_state_t &state) {
  assert(report.buttons == state.buttonFlags);
  assert(report.left_x == state.lsX && report.left_y == state.lsY);
  assert(report.right_x == state.rsX && report.right_y == state.rsY);
  assert(report.left_trigger == state.lt && report.right_trigger == state.rt);
}
void battery_tests() {
  for (bool pending_change : {false, true}) {
    vhf_gamepad_t pad;
    pad.allocate(0);
    pad.battery(0, {80, LI_BATTERY_STATE_CHARGING});
    pad.impl->recovery.request();
    const auto calls = pad.impl->client.battery_submissions;
    if (pending_change) { pad.battery(0, {5, LI_BATTERY_STATE_DISCHARGING}); }
    assert(pad.impl->client.battery_submissions == calls);
    assert(pad.recover(10s) == 0u);
    const auto actual = pad.impl->client.batteries.at(0);
    assert(actual.percent == (pending_change ? 5u : 80u));
    assert(actual.flags == unsigned(pending_change ? lvg::battery_state::discharging : lvg::battery_state::charging));
  }
  {
    vhf_gamepad_t pad;
    pad.allocate(0);
    pad.impl->client.battery_error = ERROR_DEVICE_REMOVED;
    pad.battery(0, {LI_BATTERY_PERCENTAGE_UNKNOWN, LI_BATTERY_STATE_FULL});
    assert(pad.impl->recovery.pending());
    assert(pad.recover(10s) == 0u);
    assert(pad.impl->client.batteries.at(0).percent == 255u);
    assert(pad.impl->client.batteries.at(0).flags == unsigned(lvg::battery_state::full));
  }
  for (const auto error : {ERROR_DEVICE_REMOVED, DWORD{5}}) {
    vhf_gamepad_t pad;
    pad.allocate(0); pad.allocate(1);
    pad.battery(0, {20, LI_BATTERY_STATE_DISCHARGING});
    pad.battery(1, {90, LI_BATTERY_STATE_CHARGING});
    pad.impl->recovery.request(); pad.impl->client.fail_battery_slot = 1;
    pad.impl->client.replay_error = error;
    assert(pad.recover(10s) == error);
    assert(pad.impl->recovery.pending());
    assert(pad.impl->client.batteries.empty() && pad.impl->client.controllers.empty());
    pad.battery(0, {5, LI_BATTERY_STATE_DISCHARGING});
    assert(!pad.recover(10s));
    pad.impl->client.fail_battery_slot = -1;
    assert(pad.recover(11s) == 0u);
    assert(pad.impl->client.batteries.at(0).percent == 5u);
    pad.impl->recovery.request(); pad.free(0);
    assert(!pad.impl->slots[0].last_battery);
    assert(pad.recover(12s) == 0u);
    pad.allocate(0); pad.impl->recovery.request();
    assert(pad.recover(13s) == 0u);
    assert(!pad.impl->client.batteries.contains(0));
    assert(pad.impl->client.batteries.at(1).percent == 90u);
  }
  {
    vhf_gamepad_t pad;
    pad.allocate(0); pad.impl->recovery.request();
    std::vector<std::thread> threads;
    for (unsigned i = 1; i <= 16; ++i) {
      threads.emplace_back([&, i] { pad.battery(0, {i, LI_BATTERY_STATE_CHARGING}); });
    }
    for (auto &thread : threads) { thread.join(); }
    const auto expected = *pad.impl->slots[0].last_battery;
    assert(pad.recover(10s) == 0u);
    assert(pad.impl->client.batteries.at(0).percent == expected.percent);
  }
}
int main() {
  battery_tests();
  const gamepad_state_t held{0x1000, 123, 234, -12000, 22000, 16000, -7000};
  const gamepad_state_t changed{0x2000, 4, 5, 1111, -2222, 3333, -4444};
  {
    vhf_gamepad_t pad;
    pad.allocate(0);
    pad.update(0, held);
    pad.impl->recovery.request(); // Removal reported through feedback/motion, no more input.
    assert(pad.recover(10s) == 0u);
    check_report(pad.impl->client.reports.at(0), held);
    assert(!pad.impl->recovery.pending());
  }
  {
    vhf_gamepad_t pad;
    pad.allocate(0);
    pad.impl->client.next_error = ERROR_DEVICE_REMOVED;
    pad.update(0, held); // Failed report must itself be retained.
    assert(pad.impl->recovery.pending());
    const auto calls = pad.impl->client.submissions;
    pad.update(0, changed); // No transport call, but complete snapshot advances.
    assert(pad.impl->client.submissions == calls);
    assert(pad.recover(10s) == 0u);
    check_report(pad.impl->client.reports.at(0), changed);
  }
  for (const auto failure : {ERROR_DEVICE_REMOVED, DWORD{5}}) {
    vhf_gamepad_t pad;
    pad.allocate(0); pad.allocate(1);
    pad.update(0, held); pad.update(1, changed);
    pad.impl->recovery.request();
    pad.impl->client.fail_replay_slot = 1;
    pad.impl->client.replay_error = failure;
    assert(pad.recover(10s) == failure);
    assert(pad.impl->recovery.pending());
    assert(pad.impl->client.controllers.empty() && pad.impl->client.reports.empty());
    pad.update(0, changed); // Latest pending change survives failed replay rollback.
    assert(!pad.recover(10s)); // Retry throttling still applies.
    pad.impl->client.fail_replay_slot = -1;
    assert(pad.recover(11s) == 0u);
    check_report(pad.impl->client.reports.at(0), changed);
    check_report(pad.impl->client.reports.at(1), changed);
  }
  {
    vhf_gamepad_t pad;
    pad.allocate(0); pad.allocate(1);
    pad.update(0, held); pad.update(1, changed);
    pad.impl->recovery.request();
    pad.free(0); // Real free(), not just toggling a fake ownership bit.
    assert(!pad.impl->slots[0].last_input);
    assert(pad.recover(10s) == 0u);
    assert(!pad.impl->client.controllers.contains(0));
    pad.allocate(0); // Reuse the index for a different owner, with no new report.
    pad.impl->recovery.request();
    assert(pad.recover(11s) == 0u);
    assert(!pad.impl->client.reports.contains(0));
    check_report(pad.impl->client.reports.at(1), changed);
  }
  {
    vhf_gamepad_t pad;
    pad.allocate(0); pad.impl->recovery.request();
    std::vector<std::thread> input_threads;
    for (unsigned i = 1; i <= 16; ++i) {
      input_threads.emplace_back([&, i] { pad.update(0, {i, i, i, int(i), -int(i), int(i), -int(i)}); });
    }
    for (auto &thread : input_threads) { thread.join(); }
    const auto expected = *pad.impl->slots[0].last_input;
    assert(pad.recover(10s) == 0u);
    const auto actual = pad.impl->client.reports.at(0);
    assert(actual.buttons == expected.buttons && actual.left_x == int(actual.buttons));
    assert(actual.right_y == -int(actual.buttons));
  }
}
'''
program = program.replace("// REAL_MAKE_INPUT", block(policy, "  lvg::input_state_request make_input_state("))
program = program.replace("// REAL_SLOT", block(adapter, "    struct slot_t {") + ";")
program = program.replace("// REAL_OBSERVE", block(adapter, "    void observe_status("))
program = program.replace("// REAL_BATTERY_NORMALIZER", block(policy, "  std::uint8_t to_protocol_battery_state("))
program = program.replace("// REAL_BATTERY", block(adapter, "  void vhf_gamepad_t::battery("))
program = program.replace("// REAL_UPDATE", block(adapter, "  void vhf_gamepad_t::update("))
program = program.replace("// REAL_FREE", block(adapter, "  void vhf_gamepad_t::free("))
with tempfile.TemporaryDirectory(prefix="vhf-replay-") as directory:
    source = pathlib.Path(directory) / "test.cpp"
    binary = pathlib.Path(directory) / "test"
    source.write_text(program)
    subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
                    "-I", str(root), str(source), "-o", str(binary), *sys.argv[3:]], check=True)
    subprocess.run([str(binary)], check=True)
print("Production VHF input/battery replay adapter tests passed")
