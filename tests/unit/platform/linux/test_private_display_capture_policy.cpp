/**
 * @file tests/unit/platform/linux/test_private_display_capture_policy.cpp
 * @brief Tests for Linux private-display capture routing.
 */
#include "../../../tests_common.h"

#include <src/platform/linux/private_display_capture_policy.h>

#include <cstdlib>
#include <vector>

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

TEST(PrivateDisplayCapturePolicy, FindsOnlyTheUniquelyConnectedPortAcrossGpus) {
  char scratch[] = "/tmp/private-capture-connector-XXXXXX";
  ASSERT_NE(mkdtemp(scratch), nullptr);
  const std::filesystem::path root {scratch};
  const auto first = root / "card0-DP-1";
  const auto second = root / "card1-DP-1";
  std::filesystem::create_directories(first);
  std::filesystem::create_directories(second);
  const std::vector candidates {first, second};
  std::ofstream(first / "status") << "connected";
  EXPECT_TRUE(policy::unique_connected_connector(candidates).empty());
  std::ofstream(second / "status") << "disconnected";
  EXPECT_EQ(policy::unique_connected_connector(candidates), first);
  std::ofstream(second / "status") << "connected";
  EXPECT_TRUE(policy::unique_connected_connector(candidates).empty());
  std::ofstream(first / "status") << "disconnected";
  EXPECT_EQ(policy::unique_connected_connector(candidates), second);
  std::ofstream(first / "status") << "unknown";
  EXPECT_TRUE(policy::unique_connected_connector(candidates).empty());
  std::filesystem::remove_all(root);
}

TEST(PrivateDisplayCapturePolicy, PortalShortcutOrderKeepsOnlyProvableDrmTargets) {
  const std::vector<std::string> active {"card1:Virtual-1", "card0:DP-1", "card2:DP-1", "card0:HDMI-A-1"};
  const auto targets = policy::qualify_portal_switch_targets(
    {"HDMI-A-1", "position-0x0-resolution-1920x1080", "DP-1", "Virtual-1", "init"}, active);
  const std::vector<std::string> expected {"card0:HDMI-A-1", "", "", "card1:Virtual-1", ""};
  EXPECT_EQ(targets, expected);
}
