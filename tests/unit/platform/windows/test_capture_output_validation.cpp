/**
 * @file tests/unit/platform/windows/test_capture_output_validation.cpp
 * @brief Tests for the background display-state validator used by WGC capture.
 */
#include "src/platform/windows/capture_output_validation.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>

using namespace std::chrono_literals;

namespace {
  using platf::dxgi::capture_policy::background_output_validator_t;

  template<class Predicate>
  bool wait_until_true(Predicate predicate, std::chrono::steady_clock::duration timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::sleep_for(1ms);
    }
    return true;
  }

  TEST(CaptureOutputValidation, ProbesOffTheCaptureThreadAndLatchesStructuralChange) {
    const auto capture_thread = std::this_thread::get_id();
    std::atomic<int> probes {0};
    std::atomic<bool> probed_on_capture_thread {false};
    background_output_validator_t validator(1ms, [&]() {
      if (std::this_thread::get_id() == capture_thread) {
        probed_on_capture_thread = true;
      }
      // Stable display state first, then a structural change.
      return ++probes >= 3;
    });

    ASSERT_TRUE(wait_until_true([&]() { return validator.structural_change_detected(); }, 5s));
    // A detected change ends probing; the capture session is about to be rebuilt.
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(probes, 3);
    EXPECT_FALSE(probed_on_capture_thread);
    EXPECT_TRUE(validator.structural_change_detected());
  }

  TEST(CaptureOutputValidation, ProbesImmediatelyAndStopsWithoutWaitingOutItsPeriod) {
    std::atomic<int> probes {0};
    std::optional<background_output_validator_t> validator;
    validator.emplace(std::chrono::hours(1), [&]() {
      ++probes;
      return false;
    });

    ASSERT_TRUE(wait_until_true([&]() { return probes.load() == 1; }, 5s));
    EXPECT_FALSE(validator->structural_change_detected());

    const auto teardown_started = std::chrono::steady_clock::now();
    validator.reset();
    EXPECT_LT(std::chrono::steady_clock::now() - teardown_started, 1s);
    EXPECT_EQ(probes, 1);
  }

  TEST(CaptureOutputValidation, PollingFollowsNotificationAvailability) {
    using platf::dxgi::capture_policy::keeps_polling;
    const auto now = std::chrono::steady_clock::now();
    EXPECT_TRUE(keeps_polling(false, now, now - 1h));
    EXPECT_TRUE(keeps_polling(true, now, now + 1s));
    EXPECT_FALSE(keeps_polling(true, now, now));
    EXPECT_FALSE(keeps_polling(true, now, now - 1s));
  }

  TEST(CaptureOutputValidation, WithNotificationsProbesOnlyAroundReportedChanges) {
    std::atomic<int> probes {0};
    background_output_validator_t validator(1ms, [&]() {
      ++probes;
      return false;
    });
    validator.rely_on_notifications(20ms);

    // Polling continues through the settle window after subscribing, then stops.
    std::this_thread::sleep_for(200ms);
    const int settled = probes.load();
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(probes.load(), settled);

    // A notification probes right away and polls through another window.
    validator.notify();
    ASSERT_TRUE(wait_until_true([&]() { return probes.load() > settled; }, 5s));
    std::this_thread::sleep_for(200ms);
    const int after_notification = probes.load();
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(probes.load(), after_notification);
  }

  TEST(CaptureOutputValidation, NotificationLeadsToDetectedChange) {
    std::atomic<bool> changed {false};
    background_output_validator_t validator(1ms, [&]() {
      return changed.load();
    });
    validator.rely_on_notifications(0ms);
    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(validator.structural_change_detected());

    changed = true;
    validator.notify();
    EXPECT_TRUE(wait_until_true([&]() { return validator.structural_change_detected(); }, 5s));
  }

  TEST(CaptureOutputValidation, StopsPromptlyWhileWaitingForANotification) {
    std::optional<background_output_validator_t> validator;
    validator.emplace(1ms, []() {
      return false;
    });
    validator->rely_on_notifications(0ms);
    std::this_thread::sleep_for(50ms);

    const auto teardown_started = std::chrono::steady_clock::now();
    validator.reset();
    EXPECT_LT(std::chrono::steady_clock::now() - teardown_started, 1s);
  }
}  // namespace
