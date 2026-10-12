// SPDX-License-Identifier: GPL-3.0-or-later
#include "src/platform/windows/dualsense_launch_policy.h"

#include <gtest/gtest.h>

using namespace std::chrono_literals;
using proc::dualsense_launch::decision;
using proc::dualsense_launch::evaluate;

TEST(DualSenseLaunch, KeepsApplicationAliveUntilControllerAndAudioAreReady) {
  const auto start = std::chrono::steady_clock::time_point {};
  const auto deadline = start + 30s;
  EXPECT_EQ(evaluate(true, false, deadline, start), decision::wait);
  EXPECT_EQ(evaluate(true, false, deadline, start + 15s), decision::wait);
  EXPECT_EQ(evaluate(true, true, deadline, start + 15s), decision::resume);
}

TEST(DualSenseLaunch, BoundsMissingControllerAndDoesNotRestartExpiredLaunch) {
  const auto deadline = std::chrono::steady_clock::time_point {} + 30s;
  EXPECT_EQ(evaluate(true, false, deadline, deadline), decision::timed_out);
  EXPECT_EQ(evaluate(true, true, deadline, deadline + 1ms), decision::timed_out);
}

TEST(DualSenseLaunch, OrdinaryDeferredLoginDoesNotRequireController) {
  EXPECT_EQ(evaluate(false, false, {}, {}), decision::resume);
}
