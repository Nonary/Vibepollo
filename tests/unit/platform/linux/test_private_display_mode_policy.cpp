/**
 * @file tests/unit/platform/linux/test_private_display_mode_policy.cpp
 * @brief Tests for Linux private-display refresh matching.
 */
#include <gtest/gtest.h>
#include <limits>
#include <src/platform/linux/private_display_mode_policy.h>

namespace policy = platf::linux_private_display::mode_policy;

TEST(LinuxPrivateDisplayModePolicy, AcceptsFractionalEquivalentRefresh) {
  EXPECT_TRUE(policy::refresh_matches(59.95, 60.0));
  EXPECT_TRUE(policy::refresh_matches(119.88, 120.0));
}

TEST(LinuxPrivateDisplayModePolicy, RejectsNearestDifferentRefresh) {
  EXPECT_FALSE(policy::refresh_matches(59.95, 138.0));
  EXPECT_FALSE(policy::refresh_matches(120.0, 138.0));
}

TEST(LinuxPrivateDisplayModePolicy, RejectsInvalidAndToleranceBoundary) {
  EXPECT_FALSE(policy::refresh_matches(59.8, 60.0));
  EXPECT_FALSE(policy::refresh_matches(std::numeric_limits<double>::quiet_NaN(), 60.0));
}
