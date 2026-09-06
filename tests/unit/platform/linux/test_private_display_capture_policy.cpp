/**
 * @file tests/unit/platform/linux/test_private_display_capture_policy.cpp
 * @brief Tests for Linux private-display capture routing.
 */
#include "../../../tests_common.h"

#include <src/platform/linux/private_display_capture_policy.h>

namespace policy = platf::linux_private_display_capture;

TEST(PrivateDisplayCapturePolicy, ExplicitKmsSurvivesDormantStartupAndRecovery) {
  EXPECT_TRUE(policy::enable_kms(true, false));
  EXPECT_TRUE(policy::enable_kms(true, true));
  EXPECT_FALSE(policy::enable_kms(false, false));
  EXPECT_TRUE(policy::enable_kms(false, true));
}

TEST(PrivateDisplayCapturePolicy, PrefersKmsOnlyForPrivateDynamicRangeSessions) {
  EXPECT_TRUE(policy::prefer_kms(1, false, false, true));
  EXPECT_TRUE(policy::prefer_kms(2, false, false, true));

  EXPECT_FALSE(policy::prefer_kms(0, false, false, true));
  EXPECT_FALSE(policy::prefer_kms(1, false, false, false));
  EXPECT_FALSE(policy::prefer_kms(0, false, false, false));
}

TEST(PrivateDisplayCapturePolicy, KeepsEffectiveSdrOnTheCompositorPath) {
  EXPECT_FALSE(policy::prefer_kms(1, true, false, true));
  EXPECT_FALSE(policy::prefer_kms(1, false, true, true));
  EXPECT_FALSE(policy::prefer_kms(1, true, true, true));
}

TEST(PrivateDisplayCapturePolicy, RetainsKmsCapabilityOnlyForHdrPool) {
  EXPECT_TRUE(policy::retain_kms_capability(true));
  EXPECT_FALSE(policy::retain_kms_capability(false));

  EXPECT_FALSE(policy::use_dummy_compositor_names(true, true));
  EXPECT_TRUE(policy::use_dummy_compositor_names(true, false));
  EXPECT_FALSE(policy::use_dummy_compositor_names(false, true));
  EXPECT_FALSE(policy::use_dummy_compositor_names(false, false));
}
