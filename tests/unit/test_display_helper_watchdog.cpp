/**
 * @file tests/unit/test_display_helper_watchdog.cpp
 * @brief Unit tests for Sunshine display helper watchdog.
 */
#include "../tests_common.h"

#include "src/platform/windows/display_helper_watchdog.h"

#include <deque>

namespace {
  struct WatchdogHarness {
    bool feature_enabled = true;
    int ensure_calls = 0;
    bool ensure_result = true;
    int ping_calls = 0;
    std::deque<bool> ping_results;
    int reset_calls = 0;
    int session_count = 1;
    int running_processes = 0;

    display_helper_integration::DisplayHelperWatchdog watchdog;

    WatchdogHarness()
      : watchdog(display_helper_integration::DisplayHelperWatchdog::Hooks {
          .feature_enabled = [&]() { return feature_enabled; },
          .ensure_helper_started = [&]() {
            ensure_calls += 1;
            return ensure_result;
          },
          .send_ping = [&]() {
            ping_calls += 1;
            if (ping_results.empty()) {
              return true;
            }
            const bool result = ping_results.front();
            ping_results.pop_front();
            return result;
          },
          .reset_connection = [&]() { reset_calls += 1; },
          .session_count = [&]() { return session_count; },
          .running_processes = [&]() { return running_processes; },
        }) {}
  };
}  // namespace

TEST(DisplayHelperWatchdog, StartsHelperAndPings) {
  WatchdogHarness harness;
  harness.ping_results = {true};

  const auto interval = harness.watchdog.tick();

  EXPECT_EQ(interval, display_helper_integration::DisplayHelperWatchdog::active_interval());
  EXPECT_EQ(harness.ensure_calls, 1);
  EXPECT_EQ(harness.ping_calls, 1);
  EXPECT_TRUE(harness.watchdog.helper_ready());
}

TEST(DisplayHelperWatchdog, ResetsOnFeatureDisable) {
  WatchdogHarness harness;
  harness.ping_results = {true};

  harness.watchdog.tick();
  harness.feature_enabled = false;
  harness.watchdog.tick();

  EXPECT_EQ(harness.reset_calls, 1);
  EXPECT_FALSE(harness.watchdog.helper_ready());
}

TEST(DisplayHelperWatchdog, ReconnectsAfterPingFailure) {
  WatchdogHarness harness;
  harness.ping_results = {true};

  harness.watchdog.tick();

  harness.ping_results = {false, true};
  harness.watchdog.tick();

  EXPECT_EQ(harness.reset_calls, 1);
  EXPECT_EQ(harness.ensure_calls, 2);
  EXPECT_EQ(harness.ping_calls, 3);
  EXPECT_TRUE(harness.watchdog.helper_ready());
}

TEST(DisplayHelperWatchdog, UsesSuspendedIntervalWhenNoSessions) {
  WatchdogHarness harness;
  harness.session_count = 0;
  harness.running_processes = 1;
  harness.ping_results = {true};

  const auto interval = harness.watchdog.tick();
  EXPECT_EQ(interval, display_helper_integration::DisplayHelperWatchdog::suspended_interval());
}
