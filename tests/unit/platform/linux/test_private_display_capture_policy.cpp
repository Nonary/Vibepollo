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

TEST(PrivateDisplayCapturePolicy, AutomaticVirtualCaptureUsesKmsAndExplicitChoicesAreHonored) {
  EXPECT_TRUE(policy::prefer_kms(true, true));
  EXPECT_FALSE(policy::prefer_kms(false, true));
  EXPECT_FALSE(policy::prefer_kms(true, false));
  EXPECT_FALSE(policy::prefer_kms(false, false));
}

TEST(PrivateDisplayCapturePolicy, RetainsKmsCapabilityForAutomaticPrivatePool) {
  EXPECT_TRUE(policy::retain_kms_capability(true));
  EXPECT_FALSE(policy::retain_kms_capability(false));

  EXPECT_FALSE(policy::use_dummy_compositor_names(true, true));
  EXPECT_TRUE(policy::use_dummy_compositor_names(true, false));
  EXPECT_FALSE(policy::use_dummy_compositor_names(false, true));
  EXPECT_FALSE(policy::use_dummy_compositor_names(false, false));
}
