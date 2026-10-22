/**
 * @file tests/unit/platform/windows/test_vhf_gamepad_recovery.cpp
 * @brief Exercise the production recovery transaction with a fake driver transport.
 */
#include <array>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <src/platform/windows/vhf_gamepad_recovery.h>
#include <thread>
#include <utility>
#include <vector>

// Independent of GoogleTest and the Windows SDK, including in release builds.
#define CHECK(condition) \
  do { \
    if (!(condition)) { \
      std::cerr << "Failed at line " << __LINE__ << '\n'; \
      std::abort(); \
    } \
  } while (false)

namespace {
  using recovery_t = platf::vhf_gamepad::connection_recovery_t;
  using namespace std::chrono_literals;

  struct slot_t {
    bool active;
    int profile;
    int owner;
    int contacts;
    bool cached_feedback;
    std::optional<unsigned> last_input {};
    std::optional<unsigned> last_battery {};
    std::optional<unsigned> battery_level_seed {};
  };

  struct client_t {
    bool connected {true};
    unsigned connections {};
    unsigned closes {};
    unsigned connect_error {};
    int fail_slot {-1};
    std::vector<std::pair<unsigned, int>> live;
    std::vector<std::pair<unsigned, int>> created;

    void close() {
      connected = false;
      live.clear();
      ++closes;
    }

    unsigned connect() {
      ++connections;
      connected = connect_error == 0;
      return connect_error;
    }

    unsigned submit_battery_state(unsigned) {
      return 0;
    }

    unsigned submit_input_state(unsigned) {
      return 0;
    }

    unsigned create_controller(unsigned nr, int profile) {
      CHECK(connected);
      if (static_cast<int>(nr) == fail_slot) {
        return 50;  // The exact profile cannot be recreated; no profile fallback.
      }
      live.emplace_back(nr, profile);
      created.emplace_back(nr, profile);
      return 0;
    }
  };

  auto reset_transient = [](unsigned, slot_t &slot) {
    slot.contacts = 0;
    slot.cached_feedback = false;
  };

  auto slots() {
    return std::array {slot_t {true, 5, 101, 2, true}, slot_t {false, 7, 202, 2, true}, slot_t {true, 9, 303, 1, true}};
  }

  void restores_exact_owners_and_profiles() {
    recovery_t recovery;
    client_t client;
    auto pads = slots();
    const auto now = recovery_t::clock::time_point {10s};
    CHECK(!recovery.attempt(client, pads, now, reset_transient));
    CHECK(client.closes == 0);
    recovery.request();
    CHECK(recovery.pending());
    CHECK(recovery.attempt(client, pads, now, reset_transient) == 0u);
    CHECK(!recovery.pending());
    CHECK((client.live == std::vector<std::pair<unsigned, int>> {{0, 5}, {2, 9}}));
    CHECK(pads[0].owner == 101 && pads[2].owner == 303);
    CHECK(pads[0].contacts == 0 && !pads[2].cached_feedback);
    CHECK(pads[1].contacts == 2 && pads[1].cached_feedback);  // Another backend's slot is untouched.
  }

  void rolls_back_partial_recreation_and_throttles_retries() {
    recovery_t recovery;
    client_t client;
    auto pads = slots();
    auto now = recovery_t::clock::time_point {10s};
    recovery.request();
    client.fail_slot = 2;
    CHECK(recovery.attempt(client, pads, now, reset_transient) == 50u);
    CHECK(recovery.pending() && !client.connected && client.live.empty());
    CHECK(client.connections == 1 && client.closes == 2);
    recovery.request();  // Repeated removed errors must not defeat the retry delay.
    CHECK(!recovery.attempt(client, pads, now + 999ms, reset_transient));
    CHECK(client.connections == 1);
    client.fail_slot = -1;
    pads[2].active = false;  // Freed while recovery was pending: never resurrect it.
    CHECK(recovery.attempt(client, pads, now + 1s, reset_transient) == 0u);
    CHECK((client.live == std::vector<std::pair<unsigned, int>> {{0, 5}}));
  }

  void connection_failure_and_last_owner_reset() {
    recovery_t recovery;
    client_t client;
    auto pads = slots();
    const auto now = recovery_t::clock::time_point {10s};
    client.connect_error = 1168;
    recovery.request();
    CHECK(recovery.attempt(client, pads, now, reset_transient) == 1168u);
    CHECK(recovery.pending() && client.created.empty() && !client.connected);
    recovery.reset();  // Last owner freed under exclusive lifetime lock.
    CHECK(!recovery.attempt(client, pads, now + 2s, reset_transient));
    client.connect_error = 0;
    recovery.request();  // New ownership epoch is not throttled by the old attempt.
    CHECK(recovery.attempt(client, pads, now, reset_transient) == 0u);
  }

  void concurrent_requests_do_not_resurrect_freed_owners() {
    recovery_t recovery;
    client_t client;
    auto pads = slots();
    std::shared_mutex lifetime;
    std::vector<std::thread> reporters;
    for (int i = 0; i < 16; ++i) {
      reporters.emplace_back([&] {
        std::shared_lock lock {lifetime};
        recovery.request();  // Same shared-lock contract as input/motion/battery/feedback.
      });
    }
    for (auto &thread : reporters) {
      thread.join();
    }
    {
      std::unique_lock lock {lifetime};
      pads[0].active = false;
    }
    std::thread worker {[&] {
      std::unique_lock lock {lifetime};
      CHECK(recovery.attempt(client, pads, recovery_t::clock::time_point {10s}, reset_transient) == 0u);
    }};
    worker.join();
    CHECK((client.live == std::vector<std::pair<unsigned, int>> {{2, 9}}));
    CHECK(!recovery.pending());
  }
}  // namespace

int main() {
  restores_exact_owners_and_profiles();
  rolls_back_partial_recreation_and_throttles_retries();
  connection_failure_and_last_owner_reset();
  concurrent_requests_do_not_resurrect_freed_owners();
  std::cout << "VHF recovery behavioral tests passed\n";
}
