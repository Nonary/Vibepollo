/**
 * @file tests/unit/platform/linux/test_private_display_restore_policy.cpp
 * @brief Tests for Linux private-display restore guards.
 */
#include <array>
#include <gtest/gtest.h>
#include <map>
#include <nlohmann/json.hpp>
#include <src/platform/linux/private_display_restore_policy.h>
#include <vector>

namespace policy = platf::linux_private_display::restore_policy;

namespace {
  using json = nlohmann::json;

  json external_desktop() {
    return {{"outputs", {
      {{"name", "eDP-1"}, {"connected", true}, {"enabled", false}, {"rotation", 8}},
      {{"name", "DP-1"}, {"connected", true}, {"enabled", true}, {"currentModeId", "25"},
       {"priority", 1}, {"scale", 1.5}, {"rotation", 1}, {"hdr", true},
       {"pos", {{"x", 0}, {"y", 0}}}, {"size", {{"width", 2560}, {"height", 1440}}}}
    }}};
  }
}

TEST(LinuxPrivateDisplayRestorePolicy, SavesDesktopBeforeHotplugCanEnablePanelAndExtendMonitor) {
  auto desktop = external_desktop();
  const auto original = desktop;
  std::optional<json> snapshot;
  ASSERT_TRUE(policy::connect_with_snapshot(snapshot, [&] { return std::make_optional(desktop); }, [&] {
    desktop["outputs"][0]["enabled"] = true;
    desktop["outputs"][1]["pos"]["x"] = 1280;
    return true;
  }));
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(*snapshot, original);
  EXPECT_NE(*snapshot, desktop);
}

TEST(LinuxPrivateDisplayRestorePolicy, ReconnectAndSecondClientKeepFirstSnapshot) {
  std::optional<json> snapshot = external_desktop();
  const auto original = *snapshot;
  int queries = 0;
  int connections = 0;
  for (int attempt = 0; attempt < 2; ++attempt) {
    EXPECT_TRUE(policy::connect_with_snapshot(snapshot, [&]() -> std::optional<json> {
      ++queries;
      return json {{"outputs", json::array()}};
    }, [&] { ++connections; return true; }));
  }
  EXPECT_EQ(queries, 0);
  EXPECT_EQ(connections, 2);
  EXPECT_EQ(*snapshot, original);
}

TEST(LinuxPrivateDisplayRestorePolicy, FailedSnapshotPreventsConnection) {
  std::optional<json> snapshot;
  bool connected = false;
  EXPECT_FALSE(policy::connect_with_snapshot(snapshot, []() -> std::optional<json> {
    return std::nullopt;
  }, [&] { connected = true; return true; }));
  EXPECT_FALSE(connected);
  EXPECT_FALSE(snapshot);
}

TEST(LinuxPrivateDisplayRestorePolicy, FailedConnectionRetainsOriginalForRollback) {
  std::optional<json> snapshot;
  EXPECT_FALSE(policy::connect_with_snapshot(snapshot, [] { return std::make_optional(external_desktop()); }, [] { return false; }));
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(*snapshot, external_desktop());
}

TEST(LinuxPrivateDisplayRestorePolicy, FinalVerificationRejectsPanelReenabledByDisconnect) {
  const auto snapshot = external_desktop();
  auto current = snapshot;
  current["outputs"][0]["enabled"] = true;
  EXPECT_TRUE(policy::snapshot_matches(snapshot, current));
  EXPECT_FALSE(policy::snapshot_matches(snapshot, current, true));
  current["outputs"][0]["enabled"] = false;
  EXPECT_TRUE(policy::snapshot_matches(snapshot, current, true));
}

TEST(LinuxPrivateDisplayRestorePolicy, FinalVerificationRejectsShiftedDesktopAndChangedPrimary) {
  const auto snapshot = external_desktop();
  auto current = snapshot;
  current["outputs"][1]["pos"]["x"] = 1280;
  EXPECT_FALSE(policy::snapshot_matches(snapshot, current, true));
  current = snapshot;
  current["outputs"][1]["priority"] = 2;
  EXPECT_FALSE(policy::snapshot_matches(snapshot, current, true));
}

TEST(LinuxPrivateDisplayRestorePolicy, FinalVerificationRejectsChangedOrientationScaleAndHdr) {
  const auto snapshot = external_desktop();
  for (const auto &change : {json {{"rotation", 8}}, json {{"scale", 1.0}}, json {{"hdr", false}}}) {
    auto current = snapshot;
    current["outputs"][1].update(change);
    EXPECT_FALSE(policy::snapshot_matches(snapshot, current, true));
  }
}

TEST(LinuxPrivateDisplayRestorePolicy, DisabledSavedOutputMayDisappearButActiveOutputMustSurvive) {
  const auto snapshot = external_desktop();
  auto current = snapshot;
  current["outputs"].erase(0);
  EXPECT_TRUE(policy::snapshot_matches(snapshot, current, true));
  current["outputs"] = json::array();
  EXPECT_FALSE(policy::snapshot_matches(snapshot, current, true));
}

TEST(LinuxPrivateDisplayRestorePolicy, PrefersConnectedPhysicalGuard) {
  const std::array candidates {
    policy::candidate_t {"Virtual-1", true, true, true},
    policy::candidate_t {"HDMI-A-1", true, true, false},
  };

  EXPECT_EQ(policy::select_guard(candidates), "HDMI-A-1");
}

TEST(LinuxPrivateDisplayRestorePolicy, FallsBackToPrivateGuardForPrivateBaseline) {
  const std::array candidates {
    policy::candidate_t {"Virtual-2", true, true, true},
  };

  EXPECT_EQ(policy::select_guard(candidates), "Virtual-2");
}

TEST(LinuxPrivateDisplayRestorePolicy, RetiringPrivateOutputCannotGuardItsOwnDisconnect) {
  const std::array candidates {
    policy::candidate_t {"Virtual-1", true, true, true, true},
  };

  EXPECT_FALSE(policy::select_guard(candidates).has_value());
}

TEST(LinuxPrivateDisplayRestorePolicy, DistinctPrivateOutputCanGuardRetirement) {
  const std::array candidates {
    policy::candidate_t {"Virtual-1", true, true, true, true},
    policy::candidate_t {"Virtual-2", true, true, true, false},
  };

  EXPECT_EQ(policy::select_guard(candidates), "Virtual-2");
}

TEST(LinuxPrivateDisplayRestorePolicy, RejectsDisabledAndDisconnectedGuards) {
  const std::array candidates {
    policy::candidate_t {"HDMI-A-1", false, true, false},
    policy::candidate_t {"DP-1", true, false, false},
  };

  EXPECT_FALSE(policy::select_guard(candidates).has_value());
}

TEST(LinuxPrivateDisplayRestorePolicy, EnabledDisconnectedBaselineHasNoUsableGuard) {
  const std::array candidates {
    policy::candidate_t {"HDMI-A-1", true, false, false},
  };

  EXPECT_FALSE(policy::select_guard(candidates).has_value());
}

TEST(LinuxPrivateDisplayRestorePolicy, HeadlessBaselineHasNoUsableGuard) {
  const std::array candidates {
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

TEST(LinuxPrivateDisplayRestorePolicy, OwnsGuardNameWhenSnapshotNameStorageIsReused) {
  // Match restore_arguments: each JSON output supplies a copied local name.
  // Reusing that string must not turn the physical eDP-1 guard into "Virtu".
  std::string name = "eDP-1";
  std::vector<policy::candidate_t> candidates;
  candidates.push_back({name, true, true, false, false});
  name = "Virtual-1";
  candidates.push_back({name, true, true, true, true});
  const std::map<std::string, std::vector<std::string>> activations {
    {"eDP-1", {"output.eDP-1.enable"}},
    {"Virtual-1", {"output.Virtual-1.enable"}},
  };

  const auto guard = policy::select_guard(candidates);
  ASSERT_EQ(guard, "eDP-1");
  const auto activation = policy::guard_activation(guard, activations);
  ASSERT_TRUE(activation.has_value());
  EXPECT_EQ(*activation, activations.at("eDP-1"));
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
