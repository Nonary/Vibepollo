#include <gtest/gtest.h>

#include <algorithm>

#include "src/remote_display_topology.h"
#include "src/platform/linux/private_display_resume_policy.h"

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

TEST(RemoteDisplayTopology, RejectsWrongTypedPersistedPlacementFieldsWithoutThrowing) {
  const std::vector<std::string> clients {"one"};
  const std::vector<std::string> physical {"path"};
  for (const auto &placement : std::vector<nlohmann::json> {
         {{"anchor_kind", "physical"}, {"anchor_id", "path"}, {"edge", "right"}, {"alignment", "start"}, {"gap_px", 0}, {"primary", "yes"}},
         {{"anchor_kind", 1}, {"anchor_id", "path"}, {"edge", "right"}, {"alignment", "start"}, {"gap_px", 0}},
         {{"anchor_kind", "physical"}, {"anchor_id", "path"}, {"edge", "right"}, {"alignment", "start"}, {"gap_px", "0"}},
       }) {
    std::string error;
    EXPECT_NO_THROW(EXPECT_FALSE(remote_display_topology::validate_layout(layout({{"one", placement}}), clients, physical, error)));
  }
}

TEST(RemoteDisplayTopology, NormalizesMalformedPersistenceButKeepsUnavailableTypedAnchor) {
  const auto fallback = remote_display_topology::normalize_layout({{"version", 1}, {"placements", {{"one", {{"anchor_kind", "physical"}, {"anchor_id", "temporarily-missing"}, {"edge", "right"}, {"alignment", "start"}, {"gap_px", "0"}}}}}});
  EXPECT_EQ(fallback, (nlohmann::json {{"version", remote_display_topology::layout_version}, {"placements", nlohmann::json::object()}}));

  const nlohmann::json saved {{"version", 1}, {"placements", {{"one", {{"anchor_kind", "physical"}, {"anchor_id", "temporarily-missing"}, {"edge", "right"}, {"alignment", "start"}, {"gap_px", 0}}}}}};
  EXPECT_EQ(remote_display_topology::normalize_layout(saved), saved);
}

TEST(RemoteDisplayTopology, SupportsEveryEdgeAndAlignment) {
  for (const auto &edge : {"left", "right", "above", "below"}) for (const auto &alignment : {"start", "center", "end"}) {
    std::string error;
    EXPECT_TRUE(remote_display_topology::validate_layout(layout({{"one", {{"anchor_kind", "physical"}, {"anchor_id", "stable-device-path"}, {"edge", edge}, {"alignment", alignment}, {"gap_px", 8}}}}), clients, {"stable-device-path"}, error)) << edge << '/' << alignment;
  }
}

TEST(RemoteDisplayTopology, CreationReceivesPairedClientLabel) {
  remote_display_topology::coordinator_t coordinator;
  std::string observed_label;
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [&observed_label](const auto &, const auto &label, const auto &) {
      observed_label = label;
      return true;
    },
    .apply_composed_topology = [](const auto &) { return true; },
    .exact_target_has_current_mode_and_dxgi = [](const auto &, const auto &) {
      return std::optional<std::string> {"\\\\.\\DISPLAY7"};
    },
  });
  EXPECT_TRUE(coordinator.activate_or_resume("client-uuid", "Living Room Tablet", {}, 1).ready);
  EXPECT_EQ(observed_label, "Living Room Tablet");
}

TEST(RemoteDisplayTopology, HdrRequestReachesCreationCompositionAndReadiness) {
  remote_display_topology::coordinator_t coordinator;
  remote_display_topology::mode_t created;
  remote_display_topology::mode_t verified;
  std::vector<remote_display_topology::node_t> composed;
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [&created](const auto &, const auto &, const auto &mode) {
      created = mode;
      return true;
    },
    .apply_composed_topology = [&composed](const auto &nodes) {
      composed = nodes;
      return true;
    },
    .exact_target_has_current_mode_and_dxgi = [&verified](const auto &, const auto &mode) {
      verified = mode;
      return std::optional<std::string> {"Virtual-1"};
    },
  });

  const auto result = coordinator.activate_or_resume("client", "HDR Client", {3840, 2160, 120, true}, 1);
  ASSERT_TRUE(result.ready);
  EXPECT_TRUE(result.hdr_enabled);
  EXPECT_TRUE(created.hdr);
  EXPECT_TRUE(verified.hdr);
  ASSERT_EQ(composed.size(), 1u);
  EXPECT_TRUE(composed.front().configured_mode.hdr);
  EXPECT_TRUE(coordinator.snapshot({})["nodes"][0]["mode"]["hdr"]);
}

TEST(RemoteDisplayTopology, PlatformCanDowngradeHdrBeforeApplyAndReadiness) {
  remote_display_topology::coordinator_t coordinator;
  remote_display_topology::mode_t applied;
  remote_display_topology::mode_t verified;
  bool hdr_available = false;
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [](const auto &, const auto &, const auto &) { return true; },
    .resolve_mode = [&hdr_available](const auto &, auto &mode) {
      mode.hdr = mode.hdr && hdr_available;
    },
    .apply_composed_topology = [&applied](const auto &nodes) {
      applied = nodes.front().configured_mode;
      return true;
    },
    .exact_target_has_current_mode_and_dxgi = [&verified](const auto &, const auto &mode) {
      verified = mode;
      return std::optional<std::string> {"Virtual-1"};
    },
  });

  const auto result = coordinator.activate_or_resume("client", "SDR Display", {3840, 2160, 120, true}, 1);
  ASSERT_TRUE(result.ready);
  EXPECT_FALSE(result.hdr_enabled);
  EXPECT_FALSE(applied.hdr);
  EXPECT_FALSE(verified.hdr);

  // The desired HDR request survives a transient capability miss and is
  // reconsidered on the next composition.
  hdr_available = true;
  ASSERT_TRUE(coordinator.reapply_composed_topology());
  EXPECT_TRUE(applied.hdr);
  EXPECT_TRUE(coordinator.snapshot({})["nodes"][0]["mode"]["hdr"]);
}

TEST(RemoteDisplayTopology, PlatformModeFallbackDoesNotReplaceRetainedClientRequest) {
  remote_display_topology::coordinator_t coordinator;
  remote_display_topology::mode_t applied;
  remote_display_topology::mode_t verified;
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [](const auto &, const auto &, const auto &) { return true; },
    .resolve_mode = [](const auto &, auto &mode) {
      mode.width = 2560;
      mode.height = 1440;
      mode.refresh_hz = 120;
    },
    .apply_composed_topology = [&applied](const auto &nodes) {
      applied = nodes.front().configured_mode;
      return true;
    },
    .exact_target_has_current_mode_and_dxgi = [&verified](const auto &, const auto &mode) {
      verified = mode;
      return std::optional<std::string> {"DP-1"};
    },
  });

  ASSERT_TRUE(coordinator.activate_or_resume("mac", "Mac", {3024, 1890, 120, false}, 1).ready);
  EXPECT_EQ(applied.width, 2560);
  EXPECT_EQ(applied.height, 1440);
  EXPECT_EQ(applied.refresh_hz, 120);
  EXPECT_EQ(verified.width, 2560);
  EXPECT_EQ(verified.height, 1440);

  // A later reapply starts from the retained client request and resolves it
  // again, rather than permanently replacing it with the platform fallback.
  ASSERT_TRUE(coordinator.reapply_composed_topology());
  EXPECT_EQ(applied.width, 2560);
  EXPECT_EQ(applied.height, 1440);
}

TEST(RemoteDisplayTopology, RemoteMonitorExtendsExistingPhysicalDesktop) {
  remote_display_topology::coordinator_t coordinator;
  std::vector<remote_display_topology::node_t> composed;
  coordinator.set_physical_baseline({{
    .id = "physical-one",
    .label = "Host Display",
    .physical = true,
    .active = true,
    .x = 100,
    .y = 0,
    .configured_mode = {1920, 1080, 60},
  }});
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [](const auto &, const auto &, const auto &) { return true; },
    .apply_composed_topology = [&composed](const auto &nodes) {
      composed = nodes;
      return true;
    },
    .exact_target_has_current_mode_and_dxgi = [](const auto &, const auto &) {
      return std::optional<std::string> {"\\\\.\\DISPLAY7"};
    },
  });

  ASSERT_TRUE(coordinator.activate_or_resume("client", "Client", {1280, 720, 60}, 1).ready);
  ASSERT_EQ(composed.size(), 2);
  EXPECT_TRUE(composed[0].physical);
  EXPECT_EQ(composed[0].id, "physical-one");
  EXPECT_FALSE(composed[1].physical);
  EXPECT_EQ(composed[1].id, "client");
  EXPECT_EQ(composed[1].x, 2020);
}

TEST(RemoteDisplayTopology, RemoteMonitorUsesLogicalFootprintOfScaledPhysicalDesktop) {
  remote_display_topology::coordinator_t coordinator;
  std::vector<remote_display_topology::node_t> composed;
  remote_display_topology::node_t physical {
    .id = "scaled-physical",
    .label = "Scaled 4K Display",
    .physical = true,
    .active = true,
    .x = 0,
    .y = 0,
    .configured_mode = {3840, 2160, 120},
  };
  physical.layout_width = 1920;
  physical.layout_height = 1080;
  coordinator.set_physical_baseline({physical});
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [](const auto &, const auto &, const auto &) { return true; },
    .apply_composed_topology = [&composed](const auto &nodes) {
      composed = nodes;
      return true;
    },
    .exact_target_has_current_mode_and_dxgi = [](const auto &, const auto &) {
      return std::optional<std::string> {"Virtual-1"};
    },
  });

  ASSERT_TRUE(coordinator.activate_or_resume("client", "Client", {3024, 1890, 120}, 1).ready);
  ASSERT_EQ(composed.size(), 2);
  EXPECT_EQ(composed[0].configured_mode.width, 3840);
  EXPECT_EQ(composed[1].x, 1920);
}

TEST(RemoteDisplayTopology, RemoteMonitorExtendsPreexistingStreamedVirtualDisplay) {
  remote_display_topology::coordinator_t coordinator;
  std::vector<remote_display_topology::node_t> composed;
  coordinator.set_physical_baseline({{
    .id = "game-client",
    .device_id = "shared-game-vdd",
    .label = "Existing Stream",
    .preexisting = true,
    .physical = false,
    .active = true,
    .configured_mode = {2560, 1440, 60},
  }});
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [](const auto &, const auto &, const auto &) { return true; },
    .apply_composed_topology = [&composed](const auto &nodes) {
      composed = nodes;
      return true;
    },
    .exact_target_has_current_mode_and_dxgi = [](const auto &, const auto &) {
      return std::optional<std::string> {"\\\\.\\DISPLAY8"};
    },
  });

  ASSERT_TRUE(coordinator.activate_or_resume("monitor-client", "Monitor", {1920, 1080, 60}, 1).ready);
  ASSERT_EQ(composed.size(), 2);
  EXPECT_TRUE(composed[0].preexisting);
  EXPECT_FALSE(composed[0].physical);
  EXPECT_EQ(composed[0].id, "game-client");
  EXPECT_EQ(composed[0].device_id, "shared-game-vdd");
  EXPECT_EQ(composed[1].x, 2560);
}

TEST(RemoteDisplayTopology, OneIdentityCoversNormalGameAndRemoteMonitorAndCapacityIsFour) {
  remote_display_topology::coordinator_t coordinator;
  coordinator.set_runtime_callbacks({.create_or_reclaim = [](const auto &, const auto &, const auto &) { return true; }, .apply_composed_topology = [](const auto &) { return true; }, .exact_target_has_current_mode_and_dxgi = [](const auto &uuid, const auto &) { return std::optional<std::string> {"\\\\.\\DISPLAY" + uuid}; }});
  EXPECT_TRUE(coordinator.reserve_normal_game_identity("one", "One", {}).accepted);
  EXPECT_TRUE(coordinator.activate_or_resume("one", "One", {}, 1).accepted);
  EXPECT_TRUE(coordinator.activate_or_resume("two", "Two", {}, 1).accepted);
  EXPECT_TRUE(coordinator.activate_or_resume("three", "Three", {}, 1).accepted);
  EXPECT_TRUE(coordinator.activate_or_resume("four", "Four", {}, 1).accepted);
  EXPECT_FALSE(coordinator.activate_or_resume("five", "Five", {}, 1).accepted);
}

TEST(RemoteDisplayTopology, NormalReservationRejectsBeforeCreateAndRollsBackOnlyItsIdentity) {
  remote_display_topology::coordinator_t coordinator;
  int creates = 0;
  coordinator.set_runtime_callbacks({.create_or_reclaim = [&creates](const auto &, const auto &, const auto &) { ++creates; return true; }, .apply_composed_topology = [](const auto &) { return true; }, .exact_target_has_current_mode_and_dxgi = [](const auto &uuid, const auto &) { return std::optional<std::string> {uuid}; }});
  const auto normal = coordinator.reserve_normal_game_identity("normal", "Normal", {});
  ASSERT_TRUE(normal.accepted);
  EXPECT_TRUE(coordinator.activate_or_resume("rm-1", "RM 1", {}, 1).accepted);
  EXPECT_TRUE(coordinator.activate_or_resume("rm-2", "RM 2", {}, 1).accepted);
  EXPECT_TRUE(coordinator.activate_or_resume("rm-3", "RM 3", {}, 1).accepted);
  EXPECT_FALSE(coordinator.reserve_normal_game_identity("fifth", "Fifth", {}).accepted);
  EXPECT_EQ(creates, 3);

  coordinator.rollback_normal_game_identity("normal", normal.token);
  const auto replacement = coordinator.reserve_normal_game_identity("fifth", "Fifth", {});
  EXPECT_TRUE(replacement.accepted);
  coordinator.rollback_normal_game_identity("fifth", normal.token);
  EXPECT_EQ(coordinator.snapshot({})["capacity"]["used"], 4);
}

TEST(RemoteDisplayTopology, NormalReservationCanRecomposeThroughPlatformRuntime) {
  remote_display_topology::coordinator_t coordinator;
  std::vector<remote_display_topology::node_t> composed;
  coordinator.set_runtime_callbacks({
    .apply_composed_topology = [&composed](const auto &nodes) {
      composed = nodes;
      return true;
    },
  });

  ASSERT_TRUE(coordinator.reserve_normal_game_identity("normal", "Normal Game", {2560, 1440, 120}).accepted);
  ASSERT_TRUE(coordinator.reapply_composed_topology());
  ASSERT_EQ(composed.size(), 1);
  EXPECT_EQ(composed.front().id, "normal");
  EXPECT_EQ(composed.front().configured_mode.width, 2560);
  EXPECT_EQ(composed.front().configured_mode.height, 1440);
  EXPECT_EQ(composed.front().configured_mode.refresh_hz, 120);
}

TEST(RemoteDisplayTopology, LinuxCrossClientResumeRetainsOneAppOwnedDisplay) {
  remote_display_topology::coordinator_t coordinator;
  std::vector<remote_display_topology::node_t> composed;
  std::vector<std::string> removed;
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [](const auto &, const auto &, const auto &) { return true; },
    .apply_composed_topology = [&](const auto &nodes) { composed = nodes; return true; },
    .exact_target_has_current_mode_and_dxgi = [](const auto &uuid, const auto &) { return std::optional<std::string> {uuid}; },
    .remove_owned_display = [&](const auto &uuid) { removed.push_back(uuid); return true; },
  });
  const auto app = coordinator.reserve_normal_game_identity("deck", "Deck", {2560, 1440, 120});
  ASSERT_TRUE(app.accepted);
  // Disconnecting the transport leaves the app and its display lease alive.
  const std::string owner {platf::linux_private_display::resume_policy::reservation_owner("mac", "deck", app.token)};
  const auto resumed = coordinator.reserve_normal_game_identity(owner, "Mac", {3024, 1890, 120});
  ASSERT_TRUE(resumed.accepted);
  EXPECT_FALSE(resumed.newly_reserved);
  EXPECT_EQ(resumed.token, app.token);
  ASSERT_TRUE(coordinator.reapply_composed_topology());
  ASSERT_EQ(composed.size(), 1);
  EXPECT_EQ(composed.front().id, "deck");

  // Only an explicit Remote Monitor request adds the Mac's separate output.
  ASSERT_TRUE(coordinator.activate_or_resume("mac", "Mac", {3024, 1890, 120}, 1).ready);
  ASSERT_EQ(composed.size(), 2);
  coordinator.release_normal_game_identity("deck", app.token);
  ASSERT_EQ(composed.size(), 1);
  EXPECT_EQ(composed.front().id, "mac");
  EXPECT_EQ(removed, std::vector<std::string> {"deck"});
}

TEST(RemoteDisplayTopology, NormalReservationResolvesHdrCapabilityBeforeRecompose) {
  remote_display_topology::coordinator_t coordinator;
  remote_display_topology::mode_t applied;
  coordinator.set_runtime_callbacks({
    .resolve_mode = [](const auto &, auto &mode) { mode.hdr = false; },
    .apply_composed_topology = [&applied](const auto &nodes) {
      applied = nodes.front().configured_mode;
      return true;
    },
  });

  ASSERT_TRUE(coordinator.reserve_normal_game_identity("normal", "Normal Game", {3840, 2160, 120, true}).accepted);
  ASSERT_TRUE(coordinator.reapply_composed_topology());
  EXPECT_FALSE(applied.hdr);
  EXPECT_FALSE(coordinator.snapshot({})["nodes"][0]["mode"]["hdr"]);
}

TEST(RemoteDisplayTopology, SharedNormalAndMonitorIdentityCountsOnceAndReleasesIndependently) {
  remote_display_topology::coordinator_t coordinator;
  std::vector<remote_display_topology::mode_t> applied;
  coordinator.set_runtime_callbacks({.create_or_reclaim = [](const auto &, const auto &, const auto &) { return true; }, .apply_composed_topology = [&applied](const auto &nodes) { if (!nodes.empty()) applied.push_back(nodes.front().configured_mode); return true; }, .exact_target_has_current_mode_and_dxgi = [](const auto &uuid, const auto &) { return std::optional<std::string> {uuid}; }});
  const auto normal = coordinator.reserve_normal_game_identity("same", "Same", {2560, 1440, 120, true});
  ASSERT_TRUE(normal.accepted);
  ASSERT_TRUE(coordinator.reapply_composed_topology());
  EXPECT_TRUE(coordinator.activate_or_resume("same", "Same", {1920, 1080, 60, false}, 7).accepted);
  EXPECT_EQ(coordinator.snapshot({})["capacity"]["used"], 1);
  ASSERT_GE(applied.size(), 2u);
  EXPECT_EQ(applied.back().width, 1920);
  EXPECT_FALSE(applied.back().hdr);

  coordinator.explicit_release("same", 7, "monitor done");
  EXPECT_EQ(coordinator.snapshot({})["capacity"]["used"], 1);
  EXPECT_EQ(applied.back().width, 2560);
  EXPECT_EQ(applied.back().height, 1440);
  EXPECT_EQ(applied.back().refresh_hz, 120);
  EXPECT_TRUE(applied.back().hdr);

  coordinator.release_normal_game_identity("same", normal.token);
  EXPECT_EQ(coordinator.snapshot({})["capacity"]["used"], 0);
}

TEST(RemoteDisplayTopology, TransportLossDefersGlobalCleanupAndExplicitReleasePreservesPeers) {
  remote_display_topology::coordinator_t coordinator;
  std::vector<std::string> removals;
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [](const auto &, const auto &, const auto &) {
      return true;
    },
    .apply_composed_topology = [](const auto &) {
      return true;
    },
    .exact_target_has_current_mode_and_dxgi = [](const auto &uuid, const auto &) {
      return std::optional<std::string> {uuid};
    },
    .remove_owned_display = [&removals](const auto &uuid) {
      removals.push_back(uuid);
      return true;
    },
  });

  ASSERT_TRUE(coordinator.activate_or_resume("one", "One", {}, 1).ready);
  ASSERT_TRUE(coordinator.activate_or_resume("two", "Two", {}, 1).ready);
  coordinator.transport_lost("one", 1);
  EXPECT_FALSE(coordinator.generic_virtual_display_cleanup_allowed());
  EXPECT_TRUE(coordinator.snapshot("one", 1).retryable);

  coordinator.explicit_release("one", 1, "owner released");
  EXPECT_EQ(removals, std::vector<std::string>({"one"}));
  EXPECT_FALSE(coordinator.generic_virtual_display_cleanup_allowed());
  EXPECT_TRUE(coordinator.snapshot("two", 1).accepted);

  coordinator.explicit_release("two", 1, "final owner released");
  EXPECT_EQ(removals, std::vector<std::string>({"one", "two"}));
  EXPECT_TRUE(coordinator.generic_virtual_display_cleanup_allowed());
}

TEST(RemoteDisplayTopology, ReleaseAppliesReplacementBeforeDisconnectingDepartingOutput) {
  remote_display_topology::coordinator_t coordinator;
  std::vector<std::string> events;
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [](const auto &, const auto &, const auto &) {
      return true;
    },
    .apply_composed_topology = [&events](const auto &nodes) {
      events.push_back("apply:" + std::to_string(nodes.size()));
      return true;
    },
    .exact_target_has_current_mode_and_dxgi = [](const auto &uuid, const auto &) {
      return std::optional<std::string> {uuid};
    },
    .remove_owned_display = [&events](const auto &uuid) {
      events.push_back("remove:" + uuid);
      return true;
    },
  });

  ASSERT_TRUE(coordinator.activate_or_resume("one", "One", {}, 1).ready);
  events.clear();
  coordinator.explicit_release("one", 1, "done");

  EXPECT_EQ(events, (std::vector<std::string> {"apply:0", "remove:one"}));
  EXPECT_TRUE(coordinator.generic_virtual_display_cleanup_allowed());
}

TEST(RemoteDisplayTopology, FailedReplacementOrDisconnectRetainsOwnership) {
  remote_display_topology::coordinator_t coordinator;
  bool reject_empty = false;
  bool reject_remove = false;
  int removals = 0;
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [](const auto &, const auto &, const auto &) {
      return true;
    },
    .apply_composed_topology = [&reject_empty](const auto &nodes) {
      return !(reject_empty && nodes.empty());
    },
    .exact_target_has_current_mode_and_dxgi = [](const auto &uuid, const auto &) {
      return std::optional<std::string> {uuid};
    },
    .remove_owned_display = [&reject_remove, &removals](const auto &) {
      ++removals;
      return !reject_remove;
    },
  });

  ASSERT_TRUE(coordinator.activate_or_resume("one", "One", {}, 1).ready);
  reject_empty = true;
  coordinator.explicit_release("one", 1, "failed topology handoff");
  EXPECT_EQ(removals, 0);
  EXPECT_TRUE(coordinator.snapshot("one", 1).accepted);

  reject_empty = false;
  reject_remove = true;
  coordinator.explicit_release("one", 1, "failed connector release");
  EXPECT_EQ(removals, 1);
  EXPECT_TRUE(coordinator.snapshot("one", 1).accepted);
  EXPECT_FALSE(coordinator.generic_virtual_display_cleanup_allowed());
}

TEST(RemoteDisplayTopology, SupervisedShutdownPreservesPlatformDisplaysUntouched) {
  remote_display_topology::coordinator_t coordinator;
  int applies = 0;
  int removals = 0;
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [](const auto &, const auto &, const auto &) {
      return true;
    },
    .apply_composed_topology = [&applies](const auto &) {
      ++applies;
      return true;
    },
    .exact_target_has_current_mode_and_dxgi = [](const auto &uuid, const auto &) {
      return std::optional<std::string> {uuid};
    },
    .remove_owned_display = [&removals](const auto &) {
      ++removals;
      return true;
    },
  });

  ASSERT_TRUE(coordinator.activate_or_resume("one", "One", {}, 1).ready);
  applies = 0;
  coordinator.shutdown(true);

  EXPECT_EQ(applies, 0);
  EXPECT_EQ(removals, 0);
  EXPECT_TRUE(coordinator.generic_virtual_display_cleanup_allowed());
}

TEST(RemoteDisplayTopology, ResumeReusesRetainedLeaseWithoutReplacingDisplay) {
  remote_display_topology::coordinator_t coordinator;
  int creates = 0;
  int exact_checks = 0;
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [&creates](const auto &, const auto &, const auto &) {
      ++creates;
      return true;
    },
    .apply_composed_topology = [](const auto &) { return true; },
    .exact_target_has_current_mode_and_dxgi = [&exact_checks](const auto &, const auto &) {
      ++exact_checks;
      return std::optional<std::string> {"\\\\.\\DISPLAY54"};
    },
  });

  ASSERT_TRUE(coordinator.activate_or_resume("one", "One", {1920, 1080, 60}, 1).ready);
  EXPECT_EQ(creates, 1);
  coordinator.transport_lost("one", 1);
  EXPECT_TRUE(coordinator.snapshot("one", 1).retryable);
  EXPECT_TRUE(coordinator.snapshot({})["runtime"]["one"]["lease_held"]);

  const auto resumed = coordinator.activate_or_resume("one", "One", {1920, 1080, 60}, 2);
  EXPECT_TRUE(resumed.ready);
  EXPECT_EQ(resumed.output, "\\\\.\\DISPLAY54");
  EXPECT_EQ(creates, 1);
  EXPECT_EQ(exact_checks, 2);
}

TEST(RemoteDisplayTopology, FailedCreationRollbackDoesNotRemoveRetainedMonitor) {
  remote_display_topology::coordinator_t coordinator;
  coordinator.set_runtime_callbacks({.create_or_reclaim = [](const auto &, const auto &, const auto &) { return true; }, .apply_composed_topology = [](const auto &) { return true; }, .exact_target_has_current_mode_and_dxgi = [](const auto &uuid, const auto &) { return std::optional<std::string> {uuid}; }});
  EXPECT_TRUE(coordinator.activate_or_resume("same", "Same", {}, 4).accepted);
  const auto normal = coordinator.reserve_normal_game_identity("same", "Same", {});
  ASSERT_TRUE(normal.newly_reserved);
  coordinator.rollback_normal_game_identity("same", normal.token);
  EXPECT_TRUE(coordinator.snapshot("same", 4).accepted);
  EXPECT_EQ(coordinator.snapshot({})["capacity"]["used"], 1);
}

TEST(RemoteDisplayTopology, FailedApplyAndLeaseLossRetainOwnershipWithoutFallback) {
  remote_display_topology::coordinator_t coordinator;
  coordinator.set_runtime_callbacks({.create_or_reclaim = [](const auto &, const auto &, const auto &) { return true; }, .apply_composed_topology = [](const auto &) { return false; }, .exact_target_has_current_mode_and_dxgi = [](const auto &, const auto &) { return std::optional<std::string> {}; }});
  const auto failed = coordinator.activate_or_resume("one", "One", remote_display_topology::mode_t {2560, 1440, 60}, 9);
  EXPECT_TRUE(failed.accepted);
  EXPECT_TRUE(failed.retryable);
  coordinator.transport_lost("one", 9);
  const auto retained = coordinator.snapshot("one", 9);
  EXPECT_TRUE(retained.accepted);
  EXPECT_TRUE(retained.retryable);
  EXPECT_TRUE(retained.output.empty());
}

TEST(RemoteDisplayTopology, NewGenerationRetriesRetainedMonitorActivation) {
  remote_display_topology::coordinator_t coordinator;
  bool create_ready = false;
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [&create_ready](const auto &, const auto &, const auto &) {
      return create_ready;
    },
    .apply_composed_topology = [](const auto &) { return true; },
    .exact_target_has_current_mode_and_dxgi = [](const auto &, const auto &) {
      return std::optional<std::string> {"\\\\.\\DISPLAY9"};
    },
  });
  const auto first = coordinator.activate_or_resume("one", "One", {}, 1);
  EXPECT_TRUE(first.accepted);
  EXPECT_TRUE(first.retryable);
  EXPECT_FALSE(first.ready);

  create_ready = true;
  const auto retried = coordinator.activate_or_resume("one", "One", {}, 2);
  EXPECT_TRUE(retried.accepted);
  EXPECT_TRUE(retried.ready);
  EXPECT_EQ(retried.output, "\\\\.\\DISPLAY9");
}

TEST(RemoteDisplayTopology, ExactOutputIsBoundToTheRequestedModeAndOldGenerationCannotReleaseNewOwner) {
  remote_display_topology::coordinator_t coordinator;
  remote_display_topology::mode_t observed {};
  int removals = 0;
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [](const auto &, const auto &, const auto &) {
      return true;
    },
    .apply_composed_topology = [](const auto &) {
      return true;
    },
    .exact_target_has_current_mode_and_dxgi = [&observed](const auto &, const auto &mode) {
      observed = mode;
      return mode.width == 2560 && mode.height == 1440 ? std::optional<std::string> {"\\\\.\\DISPLAY7"} : std::nullopt;
    },
    .remove_owned_display = [&removals](const auto &) {
      ++removals;
      return true;
    },
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
    .create_or_reclaim = [](const auto &, const auto &, const auto &) { return true; },
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

TEST(RemoteDisplayTopology, ClientAnchorCompositionIsIndependentOfMapIterationOrder) {
  remote_display_topology::coordinator_t coordinator;
  coordinator.set_physical_baseline({{
    .id = "physical",
    .label = "Physical",
    .physical = true,
    .active = true,
    .configured_mode = {1920, 1080, 60},
  }});
  coordinator.set_layout(layout({
    {"one", {{"anchor_kind", "client"}, {"anchor_id", "two"}, {"edge", "right"}, {"alignment", "start"}, {"gap_px", 0}}},
    {"two", {{"anchor_kind", "physical"}, {"anchor_id", "physical"}, {"edge", "right"}, {"alignment", "start"}, {"gap_px", 0}}},
  }));
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [](const auto &, const auto &, const auto &) { return true; },
    .apply_composed_topology = [](const auto &) { return true; },
    .exact_target_has_current_mode_and_dxgi = [](const auto &uuid, const auto &) {
      return std::optional<std::string> {uuid};
    },
  });
  EXPECT_TRUE(coordinator.activate_or_resume("one", "One", {1920, 1080, 60}, 1).ready);
  EXPECT_TRUE(coordinator.activate_or_resume("two", "Two", {1920, 1080, 60}, 1).ready);

  const auto state = coordinator.snapshot({});
  const auto find_x = [&](const std::string &id) {
    const auto it = std::find_if(state["nodes"].begin(), state["nodes"].end(), [&](const auto &node) {
      return node["id"] == id;
    });
    return it == state["nodes"].end() ? -1 : (*it)["desired_position"]["x"].get<int>();
  };
  EXPECT_EQ(find_x("two"), 1920);
  EXPECT_EQ(find_x("one"), 3840);
  EXPECT_TRUE(state["warnings"].empty());
}

TEST(RemoteDisplayTopology, RemoteMonitorExtendsActiveNormalGameVirtualDisplayByDefault) {
  remote_display_topology::coordinator_t coordinator;
  std::vector<remote_display_topology::node_t> applied;
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [](const auto &, const auto &, const auto &) { return true; },
    .apply_composed_topology = [&applied](const auto &nodes) {
      applied = nodes;
      return true;
    },
    .exact_target_has_current_mode_and_dxgi = [](const auto &uuid, const auto &) {
      return std::optional<std::string> {uuid};
    },
  });

  ASSERT_TRUE(coordinator.reserve_normal_game_identity("z-existing", "Existing", {1920, 1080, 60}).accepted);
  ASSERT_TRUE(coordinator.activate_or_resume("a-new", "New", {1920, 1080, 60}, 1).ready);

  const auto find_node = [&](const std::string &id) {
    return std::find_if(applied.begin(), applied.end(), [&](const auto &node) {
      return node.id == id;
    });
  };
  const auto existing = find_node("z-existing");
  const auto added = find_node("a-new");
  ASSERT_NE(existing, applied.end());
  ASSERT_NE(added, applied.end());
  EXPECT_FALSE(existing->physical);
  EXPECT_FALSE(added->physical);
  EXPECT_EQ(existing->x, 0);
  EXPECT_EQ(added->x, 1920);
}

TEST(RemoteDisplayTopology, MissingAnchorAppendsAndReleasingOnePeerPreservesTheOther) {
  remote_display_topology::coordinator_t coordinator;
  coordinator.set_layout(layout({{"one", {{"anchor_kind", "physical"}, {"anchor_id", "removed-monitor"}, {"edge", "right"}, {"alignment", "center"}, {"gap_px", 0}}}}));
  std::vector<std::vector<std::string>> applied;
  coordinator.set_runtime_callbacks({.create_or_reclaim = [](const auto &, const auto &, const auto &) { return true; }, .apply_composed_topology = [&applied](const auto &nodes) { std::vector<std::string> ids; for (const auto &node : nodes) if (!node.physical) ids.push_back(node.id); applied.push_back(std::move(ids)); return true; }, .exact_target_has_current_mode_and_dxgi = [](const auto &uuid, const auto &) { return std::optional<std::string> {std::string {"target-"} + uuid}; }});
  EXPECT_TRUE(coordinator.activate_or_resume("one", "One", {}, 3).ready);
  EXPECT_TRUE(coordinator.activate_or_resume("two", "Two", {}, 3).ready);
  EXPECT_FALSE(coordinator.snapshot({{{"uuid", "one"}, {"name", "One"}}, {{"uuid", "two"}, {"name", "Two"}}})["warnings"].empty());
  coordinator.explicit_release("one", 3, "Disconnect Monitor");
  const auto state = coordinator.snapshot({{{"uuid", "one"}, {"name", "One"}}, {{"uuid", "two"}, {"name", "Two"}}});
  EXPECT_EQ(state["capacity"]["used"], 1);
  EXPECT_NE(std::find_if(state["nodes"].begin(), state["nodes"].end(), [](const auto &node) { return node["id"] == "two"; }), state["nodes"].end());
  EXPECT_TRUE(state["warnings"].empty());
  ASSERT_FALSE(applied.empty());
  EXPECT_EQ(applied.back(), std::vector<std::string>({"two"}));
}
