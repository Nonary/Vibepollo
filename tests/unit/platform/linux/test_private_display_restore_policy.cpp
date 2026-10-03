/**
 * @file tests/unit/platform/linux/test_private_display_restore_policy.cpp
 * @brief Tests for Linux private-display restore guards.
 */
#include <array>
#include <gtest/gtest.h>
#include <map>
#include <nlohmann/json.hpp>
#include <src/platform/linux/private_display_restore_policy.h>
#include <src/platform/linux/private_display_snapshot_policy.h>
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

TEST(LinuxPrivateDisplayRestorePolicy, CandidateOwnsNameAfterOriginalChanges) {
  std::string output_name {"HDMI-A-1"};
  const std::array candidates {policy::candidate_t {output_name, true, true, false}};
  output_name.assign("changed");

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

namespace snapshot_policy = platf::linux_private_display::snapshot_policy;

TEST(LinuxPrivateDisplaySnapshot, RestartRestoresSavedIntentFromSerializedBaseline) {
  const auto original = external_desktop();
  std::optional<json> snapshot;
  std::string disk;
  ASSERT_TRUE(snapshot_policy::capture(snapshot, original, {"Virtual-1"}, false, [&](const json &saved) {
    disk = json {{"version", 1}, {"owner", "1000:desktop"}, {"topology", saved}}.dump();
    return true;
  }));
  auto streamed = original;
  streamed["outputs"][1]["enabled"] = false;
  streamed["outputs"].push_back({{"name", "Virtual-1"}, {"connected", true}, {"enabled", true}});
  const auto restored = snapshot_policy::decode<json>(disk, "1000:desktop");
  ASSERT_TRUE(restored);
  EXPECT_EQ(*restored, original);
  EXPECT_FALSE((*restored)["outputs"][0]["enabled"]);
  EXPECT_TRUE((*restored)["outputs"][1]["enabled"]);
  EXPECT_EQ(snapshot_policy::retiring_outputs(*restored, streamed, {"Virtual-1"}), (std::set<std::string> {"Virtual-1"}));
}

TEST(LinuxPrivateDisplaySnapshot, IdleRefreshRunsEvenWhenOldSnapshotAlreadyExists) {
  auto old = external_desktop();
  old["outputs"][0]["enabled"] = true;
  std::optional<json> snapshot = old;
  const auto current = external_desktop();
  int saves = 0;
  ASSERT_TRUE(snapshot_policy::capture(snapshot, current, {"Virtual-1"}, false, [&](const json &saved) {
    ++saves;
    EXPECT_EQ(saved, current);
    return true;
  }));
  EXPECT_EQ(saves, 1);
  EXPECT_EQ(*snapshot, current);
}

TEST(LinuxPrivateDisplaySnapshot, ConnectedPrivateOrRetainedOwnerPreservesCrashBaseline) {
  std::optional<json> snapshot = external_desktop();
  const auto original = *snapshot;
  auto current = original;
  current["outputs"][1]["enabled"] = false;
  current["outputs"].push_back({{"name", "Virtual-1"}, {"connected", true}, {"enabled", true}});
  int writes = 0;
  ASSERT_TRUE(snapshot_policy::capture(snapshot, current, {"Virtual-1"}, false, [&](const json &) {
    ++writes;
    return true;
  }));
  current["outputs"].erase(2);
  ASSERT_TRUE(snapshot_policy::capture(snapshot, current, {"Virtual-1"}, true, [&](const json &) {
    ++writes;
    return true;
  }));
  EXPECT_EQ(writes, 0);
  EXPECT_EQ(*snapshot, original);
}

TEST(LinuxPrivateDisplaySnapshot, FailedPersistencePreventsSnapshotReplacementAndHotplug) {
  std::optional<json> snapshot = external_desktop();
  const auto original = *snapshot;
  auto current = original;
  current["outputs"][1]["rotation"] = 8;
  bool hotplug = false;
  if (snapshot_policy::capture(snapshot, current, {"Virtual-1"}, false, [](const json &) {
        return false;
      })) {
    hotplug = true;
  }
  EXPECT_FALSE(hotplug);
  EXPECT_EQ(*snapshot, original);
}

TEST(LinuxPrivateDisplaySnapshot, MissingOrInvalidBaselineUsesOnlyLiveEnabledPhysicalOutputs) {
  auto current = external_desktop();
  current["outputs"].push_back({{"name", "Virtual-1"}, {"connected", true}, {"enabled", true}});
  const auto fallback = snapshot_policy::live_fallback(current, {"Virtual-1"});
  ASSERT_TRUE(fallback);
  EXPECT_FALSE((*fallback)["outputs"][0]["enabled"]);
  EXPECT_TRUE((*fallback)["outputs"][1]["enabled"]);
  EXPECT_FALSE((*fallback)["outputs"][2]["enabled"]);
  current["outputs"][1]["enabled"] = false;
  EXPECT_FALSE(snapshot_policy::live_fallback(current, {"Virtual-1"}));
}

TEST(LinuxPrivateDisplaySnapshot, RejectsCorruptSchemaAndOtherSessionIntent) {
  const auto valid = json {{"version", 1}, {"owner", "1000:desktop"}, {"topology", external_desktop()}};
  EXPECT_FALSE(snapshot_policy::decode<json>(valid.dump(), "1001:desktop"));
  EXPECT_FALSE(snapshot_policy::decode<json>(valid.dump(), "1000:greeter"));
  EXPECT_FALSE(snapshot_policy::decode<json>("{", "1000:desktop"));
  for (const auto &change : {json {{"enabled", "true"}}, json {{"name", "DP-1;exec"}}, json {{"scale", -1}}, json {{"rotation", 90}}, json {{"pos", {{"x", "wrong"}}}}, json {{"modes", "wrong"}}}) {
    auto invalid = valid;
    invalid["topology"]["outputs"][1].update(change);
    EXPECT_FALSE(snapshot_policy::decode<json>(invalid.dump(), "1000:desktop"));
  }
}

TEST(LinuxPrivateDisplaySnapshot, StartupRetiresOnlyUnsavedOrDisabledManagedConnectors) {
  auto saved = external_desktop();
  saved["outputs"].push_back({{"name", "Virtual-2"}, {"connected", true}, {"enabled", true}});
  auto current = saved;
  current["outputs"].push_back({{"name", "Virtual-1"}, {"connected", true}, {"enabled", true}});
  current["outputs"].push_back({{"name", "Virtual-3"}, {"connected", false}, {"enabled", false}});
  EXPECT_EQ(snapshot_policy::retiring_outputs(saved, current, {"Virtual-1", "Virtual-2", "Virtual-3"}), (std::set<std::string> {"Virtual-1"}));
}

TEST(LinuxPrivateDisplaySnapshot, LiveFallbackRetiresPreviouslyRetainedPrivateBaseline) {
  auto saved = external_desktop();
  saved["outputs"][1]["enabled"] = false;
  saved["outputs"].push_back({{"name", "Virtual-1"}, {"connected", true}, {"enabled", true}});
  auto current = saved;
  current["outputs"][1]["enabled"] = true;
  EXPECT_TRUE(snapshot_policy::retiring_outputs(saved, current, {"Virtual-1"}).empty());
  const auto fallback = snapshot_policy::live_fallback(current, {"Virtual-1"});
  ASSERT_TRUE(fallback);
  EXPECT_EQ(snapshot_policy::retiring_outputs(*fallback, current, {"Virtual-1"}), (std::set<std::string> {"Virtual-1"}));
}

TEST(LinuxPrivateDisplaySnapshot, IdleStartupBaselineDoesNotRestoreOverLaterPhysicalLayout) {
  const auto startup = external_desktop();
  std::optional<json> baseline;
  std::string disk;
  ASSERT_TRUE(snapshot_policy::capture(baseline, startup, {"Virtual-1"}, false, [&](const json &saved) {
    disk = json {{"version", 1}, {"owner", "1000:desktop"}, {"restore_pending", false}, {"topology", saved}}.dump();
    return true;
  }));
  // Restart loads an idle baseline without arming an output transaction.
  bool pending = true;
  const auto saved = snapshot_policy::decode<json>(disk, "1000:desktop", &pending);
  ASSERT_TRUE(saved);
  EXPECT_FALSE(pending);
  const auto snapshot = pending ? saved : std::nullopt;
  auto current = startup;
  current["outputs"][0]["enabled"] = true;
  current["outputs"][1]["rotation"] = 8;
  current["outputs"][1]["pos"]["x"] = 1920;
  EXPECT_NE(current, *saved);
  EXPECT_FALSE(snapshot_policy::restore_needed(snapshot, current, {"Virtual-1"}, false));
}

TEST(LinuxPrivateDisplaySnapshot, ExplicitConnectedOutputCapturesCurrentLayoutWithoutIdleRestore) {
  auto current = external_desktop();
  current["outputs"].push_back({{"name", "Virtual-1"}, {"connected", true}, {"enabled", true}});
  std::optional<json> snapshot;
  // An explicitly reserved, always-connected connector is not an orphan
  // managed hotplug. A physical-only stream has no restore work here.
  EXPECT_FALSE(snapshot_policy::restore_needed(snapshot, current, {}, false));
  current["outputs"][1]["rotation"] = 8;
  ASSERT_TRUE(snapshot_policy::capture(snapshot, current, {"Virtual-1"}, true, [](const json &) { return true; }));
  ASSERT_TRUE(snapshot);
  EXPECT_EQ((*snapshot)["outputs"][1]["rotation"], 8);
  EXPECT_TRUE(snapshot_policy::restore_needed(snapshot, current, {}, true));
}

TEST(LinuxPrivateDisplaySnapshot, OwnedBaselineSurvivesConnectorReleaseUntilRestore) {
  const auto current = external_desktop();
  std::optional<json> snapshot;
  std::string disk;
  ASSERT_TRUE(snapshot_policy::capture(snapshot, current, {"Virtual-1"}, false, [&](const json &saved) {
    disk = json {{"version", 1}, {"owner", "1000:desktop"}, {"restore_pending", true}, {"topology", saved}}.dump();
    return true;
  }));
  bool pending = false;
  const auto recovered = snapshot_policy::decode<json>(disk, "1000:desktop", &pending);
  ASSERT_TRUE(recovered);
  EXPECT_TRUE(pending);
  // Remote Monitor release can unplug its output before final topology
  // restoration. Its armed snapshot still needs restoration with no owner.
  EXPECT_TRUE(snapshot_policy::restore_needed(recovered, current, {"Virtual-1"}, false));
}

TEST(LinuxPrivateDisplaySnapshot, ManagedOrphanWithoutSavedIntentStillRequiresGuardedRecovery) {
  auto current = external_desktop();
  current["outputs"].push_back({{"name", "Virtual-1"}, {"connected", true}, {"enabled", true}});
  const std::optional<json> snapshot;
  EXPECT_TRUE(snapshot_policy::restore_needed(snapshot, current, {"Virtual-1"}, false));
  EXPECT_TRUE(snapshot_policy::restore_needed(snapshot, external_desktop(), {}, true));
}

TEST(LinuxPrivateDisplaySnapshot, LegacySavedIntentRemainsRecoverableAndInvalidFlagIsRejected) {
  auto saved = json {{"version", 1}, {"owner", "1000:desktop"}, {"topology", external_desktop()}};
  bool pending = false;
  EXPECT_TRUE(snapshot_policy::decode<json>(saved.dump(), "1000:desktop", &pending));
  EXPECT_TRUE(pending);
  saved["restore_pending"] = "true";
  EXPECT_FALSE(snapshot_policy::decode<json>(saved.dump(), "1000:desktop", &pending));
}
