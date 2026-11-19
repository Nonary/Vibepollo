/**
 * @file tests/unit/platform/linux/test_private_display_capture_policy.cpp
 * @brief Tests for Linux private-display capture routing.
 */
#include "../../../tests_common.h"

#include <src/platform/linux/private_display_capture_policy.h>

namespace policy = platf::linux_private_display_capture;

TEST(PrivateDisplayCapturePolicy, ExclusiveCaptureRequiresTheReservedOutputAndAllowsMirrors) {
  const std::map<std::string, std::string> mirrors {{"owner", "Virtual-1"}, {"mirror", "Virtual-1"}};
  EXPECT_TRUE(policy::exclusive_output(true, "Virtual-1", mirrors));
  EXPECT_FALSE(policy::exclusive_output(false, "Virtual-1", mirrors));
  EXPECT_FALSE(policy::exclusive_output(true, "Virtual-2", mirrors));
  EXPECT_FALSE(policy::exclusive_output(true, "Virtual-1", {}));
  auto peers = mirrors;
  peers.emplace("remote-monitor", "Virtual-2");
  EXPECT_FALSE(policy::exclusive_output(true, "Virtual-1", peers));
}

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

TEST(PrivateDisplayCapturePolicy, AutomaticKmsSkipsPortalConsentWhileExplicitPortalRemainsAllowed) {
  EXPECT_FALSE(policy::probe_portal(true, false, false, true));
  EXPECT_FALSE(policy::probe_portal(true, false, true, false));
  EXPECT_TRUE(policy::probe_portal(true, false, false, false));
  EXPECT_TRUE(policy::probe_portal(false, true, false, true));
  EXPECT_TRUE(policy::probe_portal(false, true, true, false));
  EXPECT_FALSE(policy::probe_portal(false, false, false, false));
}

TEST(PrivateDisplayCapturePolicy, RetainsKmsCapabilityForAutomaticPrivatePool) {
  EXPECT_TRUE(policy::retain_kms_capability(true));
  EXPECT_FALSE(policy::retain_kms_capability(false));

  EXPECT_FALSE(policy::use_dummy_compositor_names(true, true));
  EXPECT_TRUE(policy::use_dummy_compositor_names(true, false));
  EXPECT_FALSE(policy::use_dummy_compositor_names(false, true));
  EXPECT_FALSE(policy::use_dummy_compositor_names(false, false));
}
