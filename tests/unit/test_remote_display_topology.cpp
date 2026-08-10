#include <gtest/gtest.h>

#include <algorithm>

#include "src/remote_display_topology.h"

namespace {
  using remote_display_topology::mode_t;

  nlohmann::json layout(nlohmann::json placements) { return {{"version", 1}, {"placements", std::move(placements)}}; }
  const std::vector<std::string> clients {"one", "two", "three", "four", "five"};
}

TEST(RemoteDisplayTopology, RejectsInvalidAtomicLayoutGraphs) {
  std::string error;
  EXPECT_FALSE(remote_display_topology::validate_layout(layout({{"one", {{"anchor_kind", "client"}, {"anchor_id", "one"}, {"edge", "left"}, {"alignment", "start"}, {"gap_px", 0}}}}), clients, {"path"}, error));
  EXPECT_FALSE(remote_display_topology::validate_layout(layout({{"unknown", {{"anchor_kind", "physical"}, {"anchor_id", "path"}, {"edge", "left"}, {"alignment", "start"}, {"gap_px", 0}}}}), clients, {"path"}, error));
  EXPECT_FALSE(remote_display_topology::validate_layout(layout({{"one", {{"anchor_kind", "client"}, {"anchor_id", "two"}, {"edge", "left"}, {"alignment", "start"}, {"gap_px", 0}}}, {"two", {{"anchor_kind", "client"}, {"anchor_id", "one"}, {"edge", "left"}, {"alignment", "start"}, {"gap_px", 0}}}}), clients, {"path"}, error));
  EXPECT_FALSE(remote_display_topology::validate_layout(layout({{"one", {{"anchor_kind", "physical"}, {"anchor_id", "path"}, {"edge", "left"}, {"alignment", "start"}, {"gap_px", -1}}}}), clients, {"path"}, error));
  EXPECT_FALSE(remote_display_topology::validate_layout(layout({{"one", {{"anchor_kind", "physical"}, {"anchor_id", "path"}, {"edge", "left"}, {"alignment", "start"}, {"gap_px", 0}, {"primary", true}}}, {"two", {{"anchor_kind", "physical"}, {"anchor_id", "path"}, {"edge", "right"}, {"alignment", "start"}, {"gap_px", 0}, {"primary", true}}}}), clients, {"path"}, error));
}

TEST(RemoteDisplayTopology, SupportsEveryEdgeAndAlignment) {
  for (const auto &edge : {"left", "right", "above", "below"}) for (const auto &alignment : {"start", "center", "end"}) {
    std::string error;
    EXPECT_TRUE(remote_display_topology::validate_layout(layout({{"one", {{"anchor_kind", "physical"}, {"anchor_id", "stable-device-path"}, {"edge", edge}, {"alignment", alignment}, {"gap_px", 8}}}}), clients, {"stable-device-path"}, error)) << edge << '/' << alignment;
  }
}

TEST(RemoteDisplayTopology, OneIdentityCoversNormalGameAndRemoteMonitorAndCapacityIsFour) {
  remote_display_topology::coordinator_t coordinator;
  coordinator.set_runtime_callbacks({.create_or_reclaim = [](const auto &, const auto &) { return true; }, .apply_composed_topology = [](const auto &) { return true; }, .exact_target_has_current_mode_and_dxgi = [](const auto &uuid, const auto &) { return std::optional<std::string> {"\\\\.\\DISPLAY" + uuid}; }});
  coordinator.note_normal_game_identity("one", "One", {});
  EXPECT_TRUE(coordinator.activate_or_resume("one", "One", {}, 1).accepted);
  EXPECT_TRUE(coordinator.activate_or_resume("two", "Two", {}, 1).accepted);
  EXPECT_TRUE(coordinator.activate_or_resume("three", "Three", {}, 1).accepted);
  EXPECT_TRUE(coordinator.activate_or_resume("four", "Four", {}, 1).accepted);
  EXPECT_FALSE(coordinator.activate_or_resume("five", "Five", {}, 1).accepted);
}

TEST(RemoteDisplayTopology, FailedApplyAndLeaseLossRetainOwnershipWithoutFallback) {
  remote_display_topology::coordinator_t coordinator;
  coordinator.set_runtime_callbacks({.create_or_reclaim = [](const auto &, const auto &) { return true; }, .apply_composed_topology = [](const auto &) { return false; }, .exact_target_has_current_mode_and_dxgi = [](const auto &, const auto &) { return std::optional<std::string> {}; }});
  const auto failed = coordinator.activate_or_resume("one", "One", mode_t {2560, 1440, 60}, 9);
  EXPECT_TRUE(failed.accepted);
  EXPECT_TRUE(failed.retryable);
  coordinator.transport_lost("one", 9);
  const auto retained = coordinator.snapshot("one", 9);
  EXPECT_TRUE(retained.accepted);
  EXPECT_TRUE(retained.retryable);
  EXPECT_TRUE(retained.output.empty());
}

TEST(RemoteDisplayTopology, ExactOutputIsBoundToTheRequestedModeAndOldGenerationCannotReleaseNewOwner) {
  remote_display_topology::coordinator_t coordinator;
  mode_t observed {};
  int removals = 0;
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [](const auto &, const auto &) { return true; },
    .apply_composed_topology = [](const auto &) { return true; },
    .exact_target_has_current_mode_and_dxgi = [&observed](const auto &, const auto &mode) {
      observed = mode;
      return mode.width == 2560 && mode.height == 1440 ? std::optional<std::string> {"\\\\.\\DISPLAY7"} : std::nullopt;
    },
    .remove_owned_display = [&removals](const auto &) { ++removals; },
  });
  EXPECT_TRUE(coordinator.activate_or_resume("one", "One", {2560, 1440, 120}, 8).ready);
  EXPECT_EQ(observed.width, 2560);
  EXPECT_EQ(observed.height, 1440);
  EXPECT_EQ(observed.refresh_hz, 120);
  coordinator.activate_or_resume("one", "One", {2560, 1440, 120}, 9);
  coordinator.explicit_release("one", 8, "stale disconnect");
  EXPECT_EQ(removals, 0);
  EXPECT_TRUE(coordinator.snapshot("one", 9).ready);
}

TEST(RemoteDisplayTopology, FailedPeerApplyKeepsExistingRemoteOwnerComposed) {
  remote_display_topology::coordinator_t coordinator;
  std::vector<std::vector<std::string>> applied;
  bool reject_two = false;
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [](const auto &, const auto &) { return true; },
    .apply_composed_topology = [&applied, &reject_two](const auto &nodes) {
      std::vector<std::string> ids;
      for (const auto &node : nodes) if (!node.physical) ids.push_back(node.id);
      applied.push_back(ids);
      return !reject_two;
    },
    .exact_target_has_current_mode_and_dxgi = [](const auto &uuid, const auto &) { return std::optional<std::string> {std::string {"target-"} + uuid}; },
  });
  EXPECT_TRUE(coordinator.activate_or_resume("one", "One", {}, 1).ready);
  reject_two = true;
  const auto second = coordinator.activate_or_resume("two", "Two", {}, 1);
  EXPECT_TRUE(second.accepted);
  EXPECT_TRUE(second.retryable);
  const auto state = coordinator.snapshot({{{"uuid", "one"}, {"name", "One"}}, {{"uuid", "two"}, {"name", "Two"}}});
  EXPECT_EQ(state["capacity"]["used"], 2);
  EXPECT_NE(std::find_if(state["nodes"].begin(), state["nodes"].end(), [](const auto &node) { return node["id"] == "one"; }), state["nodes"].end());
  ASSERT_FALSE(applied.empty());
  EXPECT_EQ(applied.back(), std::vector<std::string>({"one", "two"}));
}

TEST(RemoteDisplayTopology, MissingAnchorAppendsAndReleasingOnePeerPreservesTheOther) {
  remote_display_topology::coordinator_t coordinator;
  coordinator.set_layout(layout({{"one", {{"anchor_kind", "physical"}, {"anchor_id", "removed-monitor"}, {"edge", "right"}, {"alignment", "center"}, {"gap_px", 0}}}}));
  std::vector<std::vector<std::string>> applied;
  coordinator.set_runtime_callbacks({.create_or_reclaim = [](const auto &, const auto &) { return true; }, .apply_composed_topology = [&applied](const auto &nodes) { std::vector<std::string> ids; for (const auto &node : nodes) if (!node.physical) ids.push_back(node.id); applied.push_back(std::move(ids)); return true; }, .exact_target_has_current_mode_and_dxgi = [](const auto &uuid, const auto &) { return std::optional<std::string> {std::string {"target-"} + uuid}; }});
  EXPECT_TRUE(coordinator.activate_or_resume("one", "One", {}, 3).ready);
  EXPECT_TRUE(coordinator.activate_or_resume("two", "Two", {}, 3).ready);
  coordinator.explicit_release("one", 3, "Disconnect Monitor");
  const auto state = coordinator.snapshot({{{"uuid", "one"}, {"name", "One"}}, {{"uuid", "two"}, {"name", "Two"}}});
  EXPECT_EQ(state["capacity"]["used"], 1);
  EXPECT_NE(std::find_if(state["nodes"].begin(), state["nodes"].end(), [](const auto &node) { return node["id"] == "two"; }), state["nodes"].end());
  ASSERT_FALSE(state["warnings"].empty());
  ASSERT_FALSE(applied.empty());
  EXPECT_EQ(applied.back(), std::vector<std::string>({"two"}));
}
