/**
 * @file tests/unit/platform/linux/test_private_display_restore_policy.cpp
 * @brief Tests for Linux private-display restore guards.
 */
#include <array>
#include <gtest/gtest.h>
#include <map>
#include <src/platform/linux/private_display_restore_policy.h>
#include <vector>

namespace policy = platf::linux_private_display::restore_policy;

TEST(LinuxPrivateDisplayRestorePolicy, PrefersConnectedPhysicalGuard) {
  constexpr std::array candidates {
    policy::candidate_t {"Virtual-1", true, true, true},
    policy::candidate_t {"HDMI-A-1", true, true, false},
  };

  EXPECT_EQ(policy::select_guard(candidates), "HDMI-A-1");
}

TEST(LinuxPrivateDisplayRestorePolicy, FallsBackToPrivateGuardForPrivateBaseline) {
  constexpr std::array candidates {
    policy::candidate_t {"Virtual-2", true, true, true},
  };

  EXPECT_EQ(policy::select_guard(candidates), "Virtual-2");
}

TEST(LinuxPrivateDisplayRestorePolicy, RetiringPrivateOutputCannotGuardItsOwnDisconnect) {
  constexpr std::array candidates {
    policy::candidate_t {"Virtual-1", true, true, true, true},
  };

  EXPECT_FALSE(policy::select_guard(candidates).has_value());
}

TEST(LinuxPrivateDisplayRestorePolicy, DistinctPrivateOutputCanGuardRetirement) {
  constexpr std::array candidates {
    policy::candidate_t {"Virtual-1", true, true, true, true},
    policy::candidate_t {"Virtual-2", true, true, true, false},
  };

  EXPECT_EQ(policy::select_guard(candidates), "Virtual-2");
}

TEST(LinuxPrivateDisplayRestorePolicy, RejectsDisabledAndDisconnectedGuards) {
  constexpr std::array candidates {
    policy::candidate_t {"HDMI-A-1", false, true, false},
    policy::candidate_t {"DP-1", true, false, false},
  };

  EXPECT_FALSE(policy::select_guard(candidates).has_value());
}

TEST(LinuxPrivateDisplayRestorePolicy, EnabledDisconnectedBaselineHasNoUsableGuard) {
  constexpr std::array candidates {
    policy::candidate_t {"HDMI-A-1", true, false, false},
  };

  EXPECT_FALSE(policy::select_guard(candidates).has_value());
}

TEST(LinuxPrivateDisplayRestorePolicy, HeadlessBaselineHasNoUsableGuard) {
  constexpr std::array candidates {
    policy::candidate_t {"HDMI-A-1", false, true, false},
    policy::candidate_t {"Virtual-1", false, false, true},
  };

  EXPECT_FALSE(policy::select_guard(candidates).has_value());
}

TEST(LinuxPrivateDisplayRestorePolicy, MissingGuardActivationFailsSafely) {
  const std::map<std::string, std::vector<std::string>> activations {
    {"HDMI-A-1", {"output.HDMI-A-1.enable"}},
  };

  EXPECT_FALSE(policy::guard_activation(std::make_optional<std::string>("DP-1"), activations).has_value());
}

TEST(LinuxPrivateDisplayRestorePolicy, ResolvesKnownGuardActivation) {
  const std::map<std::string, std::vector<std::string>> activations {
    {"HDMI-A-1", {"output.HDMI-A-1.enable"}},
  };

  const auto activation = policy::guard_activation(std::make_optional<std::string>("HDMI-A-1"), activations);
  ASSERT_TRUE(activation.has_value());
  EXPECT_EQ(*activation, activations.at("HDMI-A-1"));
}

TEST(LinuxPrivateDisplayRestorePolicy, PreservesOnlyWorkingPrivateScanoutAtStartup) {
  EXPECT_TRUE(policy::preserve_private_scanout(false, true));
}

TEST(LinuxPrivateDisplayRestorePolicy, CleansPrivateScanoutWhenPhysicalCaptureIsReady) {
  EXPECT_FALSE(policy::preserve_private_scanout(true, true));
  EXPECT_FALSE(policy::preserve_private_scanout(true, false));
}

TEST(LinuxPrivateDisplayRestorePolicy, DoesNotPreserveUncapturablePrivateConnector) {
  EXPECT_FALSE(policy::preserve_private_scanout(false, false));
}
