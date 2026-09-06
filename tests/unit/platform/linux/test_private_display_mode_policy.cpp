/**
 * @file tests/unit/platform/linux/test_private_display_mode_policy.cpp
 * @brief Tests for Linux private-display refresh matching.
 */
#include <gtest/gtest.h>
#include <limits>
#include <src/platform/linux/private_display_configuration_policy.h>
#include <src/platform/linux/private_display_mode_policy.h>

namespace policy = platf::linux_private_display::mode_policy;
namespace configuration = platf::linux_private_display::configuration_policy;

TEST(LinuxPrivateDisplayConfigurationPolicy, RejectsKScreenFailureWithSuccessfulExit) {
  EXPECT_FALSE(configuration::command_succeeded(true, "applying config failed! The session is not active\n", "locale warning"));
  EXPECT_FALSE(configuration::command_succeeded(true, "", "applying config failed! rejected"));
  EXPECT_FALSE(configuration::command_succeeded(false, "", ""));
  EXPECT_TRUE(configuration::command_succeeded(true, "Output 1 set mode 1", "locale warning"));
}

TEST(LinuxPrivateDisplayConfigurationPolicy, AllowsUsableExistingModeWithoutMatchingPreference) {
  EXPECT_TRUE(configuration::usable_current_mode(true, true, 3840, 2160, 115.88, 2.5));
  EXPECT_TRUE(configuration::usable_current_mode(true, true, 1920, 1080, 60, 1));
  EXPECT_FALSE(configuration::usable_current_mode(false, true, 3840, 2160, 120, 1));
  EXPECT_FALSE(configuration::usable_current_mode(true, false, 3840, 2160, 120, 1));
  EXPECT_FALSE(configuration::usable_current_mode(true, true, 0, 2160, 120, 1));
  EXPECT_FALSE(configuration::usable_current_mode(true, true, 3840, 0, 120, 1));
  EXPECT_FALSE(configuration::usable_current_mode(true, true, 3840, 2160, 0, 1));
  EXPECT_FALSE(configuration::usable_current_mode(true, true, 3840, 2160, 120, 0));
  EXPECT_FALSE(configuration::usable_current_mode(true, true, 3840, 2160, std::numeric_limits<double>::quiet_NaN(), 1));
}

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

TEST(LinuxPrivateDisplayModePolicy, PreservesExactClientModeIncludingFractionalRefresh) {
  const policy::requested_mode_t mac {3024, 1890, 120000};
  const policy::requested_mode_t custom {3025, 1891, 119880};
  EXPECT_TRUE(mac.valid());
  EXPECT_TRUE(custom.valid());
  EXPECT_EQ(custom.width, 3025u);
  EXPECT_EQ(custom.height, 1891u);
  EXPECT_EQ(custom.refresh_millihz, 119880u);
}

TEST(LinuxPrivateDisplayModePolicy, RejectsModesOutsideDriverLimits) {
  EXPECT_FALSE((policy::requested_mode_t {63, 1890, 120000}).valid());
  EXPECT_FALSE((policy::requested_mode_t {3024, 8193, 120000}).valid());
  EXPECT_FALSE((policy::requested_mode_t {3024, 1890, 999}).valid());
  EXPECT_FALSE((policy::requested_mode_t {3024, 1890, 1000001}).valid());
  EXPECT_TRUE((policy::requested_mode_t {64, 8192, 1000000}).valid());
}

TEST(LinuxPrivateDisplayModePolicy, RecognizesKScreenFailuresDespiteSuccessfulExitStatus) {
  EXPECT_TRUE(policy::doctor_reported_failure("Unable to parse arguments: output.Virtual-1.addCustomMode.3024.1890.120000.reduced\n"));
  EXPECT_TRUE(policy::doctor_reported_failure("applying config failed! The request was rejected\n"));
  EXPECT_TRUE(policy::doctor_reported_failure("kscreen.doctor: Invalid config.\n"));
  EXPECT_FALSE(policy::doctor_reported_failure("kscreen.doctor: Set output position QPoint(0,0)\n"));
}
