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

TEST(RemoteDisplayTopology, AppExitWaitsForEveryCaptureBeforeMutatingTopology) {
  remote_display_topology::coordinator_t coordinator;
  std::vector<std::string> operations;
  coordinator.set_runtime_callbacks({
    .apply_composed_topology = [&](const auto &) {
      operations.emplace_back("apply");
      return true;
    },
    .remove_owned_display = [&](const auto &uuid) {
      operations.push_back("remove:" + uuid);
      return true;
    },
  });
  const auto app = coordinator.reserve_normal_game_identity("game", "Game", {});
  auto rtsp = coordinator.retain_normal_game_capture("game", app.token);
  auto webrtc = coordinator.retain_normal_game_capture("game", app.token);
  ASSERT_TRUE(rtsp);
  ASSERT_TRUE(webrtc);
  EXPECT_FALSE(coordinator.normal_game_release_pending());

  coordinator.release_normal_game_identity("game", app.token);
  EXPECT_TRUE(coordinator.normal_game_release_pending());
  EXPECT_FALSE(coordinator.retain_normal_game_capture("game", app.token));
  coordinator.release_drained_normal_game_identities();
  EXPECT_TRUE(operations.empty());
  EXPECT_EQ(coordinator.managed_client_identity_count(), 1u);

  rtsp.reset();
  coordinator.release_drained_normal_game_identities();
  EXPECT_TRUE(operations.empty());
  webrtc.reset();
  // A reference destructor may run on the video thread or a failed startup
  // path. Only the caller holding the lifecycle gate may change the display.
  EXPECT_TRUE(operations.empty());
  coordinator.release_drained_normal_game_identities();
  EXPECT_EQ(operations, (std::vector<std::string> {"apply", "remove:game"}));
  EXPECT_EQ(coordinator.managed_client_identity_count(), 0u);
  EXPECT_FALSE(coordinator.normal_game_release_pending());
  coordinator.release_drained_normal_game_identities();
  EXPECT_EQ(operations.size(), 2u);
}

TEST(RemoteDisplayTopology, RemoteMonitorWaitsForRetiringNormalCaptureBeforeTouchingDisplay) {
  remote_display_topology::coordinator_t coordinator;
  std::vector<std::string> operations;
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [&](const auto &, const auto &, const auto &) {
      operations.emplace_back("create");
      return true;
    },
    .apply_composed_topology = [&](const auto &) {
      operations.emplace_back("apply");
      return true;
    },
    .exact_target_has_current_mode_and_dxgi = [](const auto &, const auto &) {
      return std::optional<std::string> {"monitor-output"};
    },
    .remove_owned_display = [&](const auto &) {
      operations.emplace_back("remove");
      return true;
    },
  });

  const auto game = coordinator.reserve_normal_game_identity("same", "Game", {2560, 1440, 120, true});
  auto capture = coordinator.retain_normal_game_capture("same", game.token);
  ASSERT_TRUE(capture);
  coordinator.release_normal_game_identity("same", game.token);

  const auto blocked = coordinator.activate_or_resume("same", "Monitor", {1920, 1080, 60, false}, 7);
  EXPECT_FALSE(blocked.accepted);
  EXPECT_FALSE(blocked.ready);
  EXPECT_TRUE(blocked.retryable);
  EXPECT_TRUE(operations.empty());

  capture.reset();
  coordinator.release_drained_normal_game_identities();
  EXPECT_EQ(operations, (std::vector<std::string> {"apply", "remove"}));

  const auto retry = coordinator.activate_or_resume("same", "Monitor", {1920, 1080, 60, false}, 7);
  EXPECT_TRUE(retry.accepted);
  EXPECT_TRUE(retry.ready);
  EXPECT_EQ(retry.output, "monitor-output");
  EXPECT_EQ(operations, (std::vector<std::string> {"apply", "remove", "create", "apply"}));
}

TEST(RemoteDisplayTopology, ExistingMonitorResumeDoesNotReconfigureWhileNormalCaptureDrains) {
  remote_display_topology::coordinator_t coordinator;
  std::vector<remote_display_topology::mode_t> applied;
  int creates = 0;
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [&](const auto &, const auto &, const auto &) {
      ++creates;
      return true;
    },
    .apply_composed_topology = [&](const auto &nodes) {
      if (!nodes.empty()) applied.push_back(nodes.front().configured_mode);
      return true;
    },
    .exact_target_has_current_mode_and_dxgi = [](const auto &, const auto &) {
      return std::optional<std::string> {"monitor-output"};
    },
  });

  ASSERT_TRUE(coordinator.activate_or_resume("same", "Monitor", {1920, 1080, 60, false}, 1).ready);
  const auto game = coordinator.reserve_normal_game_identity("same", "Game", {2560, 1440, 120, true});
  auto capture = coordinator.retain_normal_game_capture("same", game.token);
  ASSERT_TRUE(capture);
  coordinator.release_normal_game_identity("same", game.token);
  const auto apply_count = applied.size();

  const auto unchanged = coordinator.activate_or_resume("same", "Monitor", {1920, 1080, 60, false}, 2);
  EXPECT_FALSE(unchanged.accepted);
  EXPECT_FALSE(unchanged.ready);
  EXPECT_TRUE(unchanged.retryable);
  EXPECT_FALSE(coordinator.snapshot("same", 2).accepted);
  EXPECT_TRUE(coordinator.snapshot("same", 1).ready);
  EXPECT_EQ(applied.size(), apply_count);
  EXPECT_EQ(creates, 1);

  const auto changed = coordinator.activate_or_resume("same", "Monitor", {3840, 2160, 120, true}, 3);
  EXPECT_FALSE(changed.accepted);
  EXPECT_FALSE(changed.ready);
  EXPECT_TRUE(changed.retryable);
  EXPECT_FALSE(coordinator.snapshot("same", 3).accepted);
  EXPECT_TRUE(coordinator.snapshot("same", 1).ready);
  EXPECT_EQ(applied.size(), apply_count);
  EXPECT_EQ(creates, 1);

  capture.reset();
  coordinator.release_drained_normal_game_identities();
  const auto retried = coordinator.activate_or_resume("same", "Monitor", {3840, 2160, 120, true}, 3);
  EXPECT_TRUE(retried.ready);
  ASSERT_GT(applied.size(), apply_count);
  EXPECT_EQ(applied.back().width, 3840);
  EXPECT_EQ(applied.back().height, 2160);
  EXPECT_EQ(applied.back().refresh_hz, 120);
  EXPECT_TRUE(applied.back().hdr);
  EXPECT_EQ(creates, 1);
}

TEST(RemoteDisplayTopology, MonitorReleaseDefersTopologyUntilNormalCaptureDrains) {
  remote_display_topology::coordinator_t coordinator;
  std::vector<std::string> operations;
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [&](const auto &, const auto &, const auto &) {
      operations.emplace_back("create");
      return true;
    },
    .apply_composed_topology = [&](const auto &) {
      operations.emplace_back("apply");
      return true;
    },
    .exact_target_has_current_mode_and_dxgi = [](const auto &, const auto &) {
      return std::optional<std::string> {"monitor-output"};
    },
    .remove_owned_display = [&](const auto &) {
      operations.emplace_back("remove");
      return true;
    },
  });

  ASSERT_TRUE(coordinator.activate_or_resume("same", "Monitor", {1920, 1080, 60}, 1).ready);
  const auto game = coordinator.reserve_normal_game_identity("same", "Game", {2560, 1440, 120});
  auto capture = coordinator.retain_normal_game_capture("same", game.token);
  ASSERT_TRUE(capture);
  coordinator.release_normal_game_identity("same", game.token);
  operations.clear();

  coordinator.explicit_release("same", 1, "monitor disconnected");
  EXPECT_TRUE(operations.empty());
  EXPECT_EQ(coordinator.managed_client_identity_count(), 1u);

  capture.reset();
  coordinator.release_drained_normal_game_identities();
  EXPECT_EQ(operations, (std::vector<std::string> {"apply", "remove"}));
  EXPECT_EQ(coordinator.managed_client_identity_count(), 0u);
}

TEST(RemoteDisplayTopology, NewMonitorGenerationCancelsOlderDeferredRelease) {
  remote_display_topology::coordinator_t coordinator;
  std::vector<std::string> operations;
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [](const auto &, const auto &, const auto &) {
      return true;
    },
    .apply_composed_topology = [&](const auto &) {
      operations.emplace_back("apply");
      return true;
    },
    .exact_target_has_current_mode_and_dxgi = [](const auto &uuid, const auto &) {
      return std::optional<std::string> {uuid};
    },
    .remove_owned_display = [&](const auto &) {
      operations.emplace_back("remove");
      return true;
    },
  });

  ASSERT_TRUE(coordinator.activate_or_resume("same", "Monitor", {1920, 1080, 60}, 1).ready);
  const auto game = coordinator.reserve_normal_game_identity("same", "Game", {2560, 1440, 120});
  auto capture = coordinator.retain_normal_game_capture("same", game.token);
  coordinator.release_normal_game_identity("same", game.token);
  coordinator.explicit_release("same", 1, "old monitor disconnected");
  capture.reset();
  operations.clear();

  EXPECT_FALSE(coordinator.resume_remote_monitor("same").accepted);
  const auto stale = coordinator.activate_or_resume("same", "Monitor", {1920, 1080, 60}, 1);
  EXPECT_FALSE(stale.accepted);
  EXPECT_TRUE(stale.retryable);
  const auto successor = coordinator.activate_or_resume("same", "Monitor", {2560, 1440, 120}, 2);
  EXPECT_TRUE(successor.ready);
  coordinator.release_drained_normal_game_identities();

  EXPECT_TRUE(coordinator.snapshot("same", 2).ready);
  EXPECT_EQ(std::count(operations.begin(), operations.end(), "remove"), 0);
  EXPECT_EQ(coordinator.managed_client_identity_count(), 1u);
}


TEST(RemoteDisplayTopology, CommittedMonitorReleaseRejectsReplayedGeneration) {
  remote_display_topology::coordinator_t coordinator;
  std::vector<std::string> removed;
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
    .remove_owned_display = [&](const auto &uuid) {
      removed.push_back(uuid);
      return true;
    },
  });

  ASSERT_TRUE(coordinator.activate_or_resume("same", "Monitor", {1920, 1080, 60}, 7).ready);
  const auto blocker = coordinator.reserve_normal_game_identity("blocker", "Blocker", {});
  auto capture = coordinator.retain_normal_game_capture("blocker", blocker.token);
  coordinator.release_normal_game_identity("blocker", blocker.token);
  coordinator.explicit_release("same", 7, "monitor disconnected");
  capture.reset();
  coordinator.release_drained_normal_game_identities();

  EXPECT_NE(std::find(removed.begin(), removed.end(), "same"), removed.end());
  EXPECT_FALSE(coordinator.activate_or_resume("same", "Monitor", {1920, 1080, 60}, 7).accepted);
  const auto successor = coordinator.activate_or_resume("same", "Monitor", {2560, 1440, 120}, 8);
  EXPECT_TRUE(successor.ready);
  EXPECT_TRUE(coordinator.snapshot("same", 8).ready);
}

TEST(RemoteDisplayTopology, CapacityRejectionPreservesReleasedGenerationHighWater) {
  remote_display_topology::coordinator_t coordinator;
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
    .remove_owned_display = [](const auto &) {
      return true;
    },
  });

  ASSERT_TRUE(coordinator.activate_or_resume("same", "Monitor", {1920, 1080, 60}, 7).ready);
  coordinator.explicit_release("same", 7, "monitor disconnected");
  EXPECT_EQ(coordinator.managed_client_identity_count(), 0u);

  const auto one = coordinator.reserve_normal_game_identity("one", "One", {});
  ASSERT_TRUE(one.accepted);
  ASSERT_TRUE(coordinator.reserve_normal_game_identity("two", "Two", {}).accepted);
  ASSERT_TRUE(coordinator.reserve_normal_game_identity("three", "Three", {}).accepted);
  ASSERT_TRUE(coordinator.reserve_normal_game_identity("four", "Four", {}).accepted);

  const auto rejected_successor = coordinator.activate_or_resume("same", "Monitor", {2560, 1440, 120}, 8);
  EXPECT_FALSE(rejected_successor.accepted);
  EXPECT_TRUE(rejected_successor.retryable);

  ASSERT_TRUE(coordinator.rollback_normal_game_identity("one", one.token));
  const auto stale = coordinator.activate_or_resume("same", "Monitor", {1920, 1080, 60}, 7);
  EXPECT_FALSE(stale.accepted);
  EXPECT_TRUE(stale.retryable);
  EXPECT_TRUE(coordinator.activate_or_resume("same", "Monitor", {2560, 1440, 120}, 8).ready);
}

TEST(RemoteDisplayTopology, ActiveSuccessorRejectsOlderGenerationActivation) {
  remote_display_topology::coordinator_t coordinator;
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [](const auto &, const auto &, const auto &) { return true; },
    .apply_composed_topology = [](const auto &) { return true; },
    .exact_target_has_current_mode_and_dxgi = [](const auto &uuid, const auto &) {
      return std::optional<std::string> {uuid};
    },
  });

  ASSERT_TRUE(coordinator.activate_or_resume("same", "Successor", {2560, 1440, 120}, 8).ready);
  const auto stale = coordinator.activate_or_resume("same", "Stale", {1920, 1080, 60}, 7);
  EXPECT_FALSE(stale.accepted);
  EXPECT_TRUE(stale.retryable);
  EXPECT_TRUE(coordinator.snapshot("same", 8).ready);
  EXPECT_FALSE(coordinator.snapshot("same", 7).accepted);
}

TEST(RemoteDisplayTopology, PreAdmissionRejectionFencesOlderGeneration) {
  const auto configure = [](remote_display_topology::coordinator_t &coordinator) {
    coordinator.set_runtime_callbacks({
      .create_or_reclaim = [](const auto &, const auto &, const auto &) { return true; },
      .apply_composed_topology = [](const auto &) { return true; },
      .exact_target_has_current_mode_and_dxgi = [](const auto &uuid, const auto &) {
        return std::optional<std::string> {uuid};
      },
      .remove_owned_display = [](const auto &) { return true; },
    });
  };

  {
    remote_display_topology::coordinator_t coordinator;
    configure(coordinator);
    const auto one = coordinator.reserve_normal_game_identity("one", "One", {});
    ASSERT_TRUE(one.accepted);
    ASSERT_TRUE(coordinator.reserve_normal_game_identity("two", "Two", {}).accepted);
    ASSERT_TRUE(coordinator.reserve_normal_game_identity("three", "Three", {}).accepted);
    ASSERT_TRUE(coordinator.reserve_normal_game_identity("four", "Four", {}).accepted);

    EXPECT_FALSE(coordinator.activate_or_resume("same", "Monitor", {}, 8).accepted);
    ASSERT_TRUE(coordinator.rollback_normal_game_identity("one", one.token));
    EXPECT_FALSE(coordinator.activate_or_resume("same", "Monitor", {}, 7).accepted);
    EXPECT_TRUE(coordinator.activate_or_resume("same", "Monitor", {}, 8).ready);
  }

  {
    remote_display_topology::coordinator_t coordinator;
    configure(coordinator);
    const auto blocker = coordinator.reserve_normal_game_identity("blocker", "Blocker", {});
    auto capture = coordinator.retain_normal_game_capture("blocker", blocker.token);
    coordinator.release_normal_game_identity("blocker", blocker.token);

    EXPECT_FALSE(coordinator.activate_or_resume("same", "Monitor", {}, 8).accepted);
    capture.reset();
    coordinator.release_drained_normal_game_identities();
    EXPECT_FALSE(coordinator.activate_or_resume("same", "Monitor", {}, 7).accepted);
    EXPECT_TRUE(coordinator.activate_or_resume("same", "Monitor", {}, 8).ready);
  }
}

TEST(RemoteDisplayTopology, LegacyUnversionedMonitorCanReactivateAfterRelease) {
  remote_display_topology::coordinator_t coordinator;
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [](const auto &, const auto &, const auto &) { return true; },
    .apply_composed_topology = [](const auto &) { return true; },
    .exact_target_has_current_mode_and_dxgi = [](const auto &uuid, const auto &) {
      return std::optional<std::string> {uuid};
    },
    .remove_owned_display = [](const auto &) { return true; },
  });

  ASSERT_TRUE(coordinator.activate_remote_monitor("same", "Monitor", {}).capture_ready);
  coordinator.disconnect_monitor("same");
  EXPECT_TRUE(coordinator.activate_remote_monitor("same", "Monitor", {}).capture_ready);
}

TEST(RemoteDisplayTopology, FailedMonitorReleaseRemainsRetryable) {
  remote_display_topology::coordinator_t coordinator;
  bool allow_apply = true;
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [](const auto &, const auto &, const auto &) { return true; },
    .apply_composed_topology = [&](const auto &) { return allow_apply; },
    .exact_target_has_current_mode_and_dxgi = [](const auto &uuid, const auto &) {
      return std::optional<std::string> {uuid};
    },
    .remove_owned_display = [](const auto &) { return true; },
  });

  ASSERT_TRUE(coordinator.activate_or_resume("same", "Monitor", {}, 8).ready);
  allow_apply = false;
  EXPECT_FALSE(coordinator.explicit_release("same", 8, "first attempt"));
  EXPECT_TRUE(coordinator.snapshot("same", 8).accepted);

  allow_apply = true;
  EXPECT_TRUE(coordinator.explicit_release("same", 8, "retry"));
  EXPECT_FALSE(coordinator.snapshot("same", 8).accepted);

  ASSERT_TRUE(coordinator.activate_or_resume("automatic", "Monitor", {}, 9).ready);
  allow_apply = false;
  EXPECT_FALSE(coordinator.explicit_release("automatic", 9, "automatic retry"));
  allow_apply = true;
  coordinator.release_drained_normal_game_identities();
  EXPECT_FALSE(coordinator.snapshot("automatic", 9).accepted);
}

TEST(RemoteDisplayTopology, MonitorSnapshotRejectsNormalOnlySharedIdentity) {
  remote_display_topology::coordinator_t coordinator;
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [](const auto &, const auto &, const auto &) { return true; },
    .apply_composed_topology = [](const auto &) { return true; },
    .exact_target_has_current_mode_and_dxgi = [](const auto &uuid, const auto &) {
      return std::optional<std::string> {uuid};
    },
    .remove_owned_display = [](const auto &) { return true; },
  });

  ASSERT_TRUE(coordinator.activate_or_resume("same", "Monitor", {}, 8).ready);
  ASSERT_TRUE(coordinator.reserve_normal_game_identity("same", "Game", {}).accepted);
  EXPECT_TRUE(coordinator.explicit_release("same", 8, "monitor released"));
  EXPECT_FALSE(coordinator.snapshot("same", 8).accepted);
  EXPECT_EQ(coordinator.managed_client_identity_count(), 1u);
}

TEST(RemoteDisplayTopology, CaptureDrainFencesEveryCoordinatorDisplayMutation) {
  remote_display_topology::coordinator_t coordinator;
  std::vector<std::string> operations;
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [&](const auto &, const auto &, const auto &) {
      operations.emplace_back("create");
      return true;
    },
    .apply_composed_topology = [&](const auto &) {
      operations.emplace_back("apply");
      return true;
    },
    .exact_target_has_current_mode_and_dxgi = [](const auto &uuid, const auto &) {
      return std::optional<std::string> {uuid};
    },
    .remove_owned_display = [&](const auto &uuid) {
      operations.push_back("remove:" + uuid);
      return true;
    },
  });

  ASSERT_TRUE(coordinator.activate_or_resume("peer", "Peer", {1920, 1080, 60}, 4).ready);
  const auto rollback_peer = coordinator.reserve_normal_game_identity("rollback-peer", "Rollback", {});
  const auto game = coordinator.reserve_normal_game_identity("game", "Game", {2560, 1440, 120});
  auto capture = coordinator.retain_normal_game_capture("game", game.token);
  ASSERT_TRUE(capture);
  coordinator.release_normal_game_identity("game", game.token);
  operations.clear();

  EXPECT_FALSE(coordinator.reapply_composed_topology());
  EXPECT_FALSE(coordinator.resume_remote_monitor("peer").accepted);
  EXPECT_FALSE(coordinator.activate_or_resume("peer", "Peer", {1920, 1080, 60}, 3).accepted);
  EXPECT_FALSE(coordinator.reserve_normal_game_identity("new-game", "New", {}).accepted);
  EXPECT_FALSE(coordinator.rollback_normal_game_identity("rollback-peer", rollback_peer.token));
  coordinator.explicit_release("peer", 4, "peer disconnected");
  EXPECT_TRUE(operations.empty());
  EXPECT_TRUE(coordinator.snapshot("peer", 4).ready);

  capture.reset();
  coordinator.release_drained_normal_game_identities();
  EXPECT_EQ(std::count(operations.begin(), operations.end(), "apply"), 3);
  EXPECT_NE(std::find(operations.begin(), operations.end(), "remove:game"), operations.end());
  EXPECT_NE(std::find(operations.begin(), operations.end(), "remove:rollback-peer"), operations.end());
  EXPECT_NE(std::find(operations.begin(), operations.end(), "remove:peer"), operations.end());
  EXPECT_EQ(coordinator.managed_client_identity_count(), 0u);
}

TEST(RemoteDisplayTopology, ReleaseAllMarksEveryOwnerBeforeAnyDisplayMutation) {
  remote_display_topology::coordinator_t coordinator;
  std::vector<std::string> operations;
  coordinator.set_runtime_callbacks({
    .apply_composed_topology = [&](const auto &) {
      operations.emplace_back("apply");
      return true;
    },
    .remove_owned_display = [&](const auto &uuid) {
      operations.push_back("remove:" + uuid);
      return true;
    },
  });
  ASSERT_TRUE(coordinator.reserve_normal_game_identity("idle", "Idle", {}).accepted);
  const auto live = coordinator.reserve_normal_game_identity("live", "Live", {});
  auto capture = coordinator.retain_normal_game_capture("live", live.token);
  ASSERT_TRUE(capture);

  coordinator.release_all_normal_game_identities();
  EXPECT_TRUE(operations.empty());
  EXPECT_EQ(coordinator.managed_client_identity_count(), 2u);

  capture.reset();
  coordinator.release_drained_normal_game_identities();
  EXPECT_EQ(std::count(operations.begin(), operations.end(), "apply"), 2);
  EXPECT_NE(std::find(operations.begin(), operations.end(), "remove:idle"), operations.end());
  EXPECT_NE(std::find(operations.begin(), operations.end(), "remove:live"), operations.end());
  EXPECT_EQ(coordinator.managed_client_identity_count(), 0u);
}

TEST(RemoteDisplayTopology, ShutdownDefersEveryDisplayCallbackUntilCaptureDrains) {
  remote_display_topology::coordinator_t coordinator;
  std::vector<std::string> operations;
  coordinator.set_runtime_callbacks({
    .apply_composed_topology = [&](const auto &) {
      operations.emplace_back("apply");
      return true;
    },
    .remove_owned_display = [&](const auto &uuid) {
      operations.push_back("remove:" + uuid);
      return true;
    },
  });

  const auto game = coordinator.reserve_normal_game_identity("game", "Game", {2560, 1440, 120});
  auto capture = coordinator.retain_normal_game_capture("game", game.token);
  ASSERT_TRUE(capture);

  coordinator.shutdown(false);
  EXPECT_TRUE(operations.empty());
  EXPECT_EQ(coordinator.managed_client_identity_count(), 1u);
  const auto blocked = coordinator.activate_or_resume("monitor", "Monitor", {1920, 1080, 60}, 7);
  EXPECT_FALSE(blocked.accepted);
  EXPECT_TRUE(blocked.retryable);

  capture.reset();
  coordinator.release_drained_normal_game_identities();
  EXPECT_EQ(operations, (std::vector<std::string> {"apply", "remove:game"}));
  EXPECT_EQ(coordinator.managed_client_identity_count(), 0u);
}

TEST(RemoteDisplayTopology, ShutdownRetainsFailedDisplayOwnershipForRetry) {
  remote_display_topology::coordinator_t coordinator;
  bool allow_apply = false;
  bool allow_remove = false;
  coordinator.set_runtime_callbacks({
    .apply_composed_topology = [&](const auto &) {
      return allow_apply;
    },
    .remove_owned_display = [&](const auto &) {
      return allow_remove;
    },
  });
  ASSERT_TRUE(coordinator.reserve_normal_game_identity("game", "Game", {}).accepted);

  coordinator.shutdown(false);
  EXPECT_EQ(coordinator.managed_client_identity_count(), 1u);
  EXPECT_FALSE(coordinator.reserve_normal_game_identity("new-game", "New", {}).accepted);

  allow_apply = true;
  coordinator.release_drained_normal_game_identities();
  EXPECT_EQ(coordinator.managed_client_identity_count(), 1u);

  allow_remove = true;
  coordinator.release_drained_normal_game_identities();
  EXPECT_EQ(coordinator.managed_client_identity_count(), 0u);
}

TEST(RemoteDisplayTopology, FreshZeroTokenCaptureWaitsForRetiringNormalDisplay) {
  remote_display_topology::coordinator_t coordinator;
  std::vector<std::string> operations;
  coordinator.set_runtime_callbacks({
    .apply_composed_topology = [&](const auto &) {
      operations.emplace_back("apply");
      return true;
    },
    .remove_owned_display = [&](const auto &uuid) {
      operations.push_back("remove:" + uuid);
      return true;
    },
  });

  const auto app = coordinator.reserve_normal_game_identity("game", "Game", {});
  auto rtsp = coordinator.retain_normal_game_capture("game", app.token);
  ASSERT_TRUE(rtsp);

  coordinator.release_normal_game_identity("game", app.token);
  ASSERT_TRUE(coordinator.normal_game_release_pending());
  // A fresh WebRTC desktop capture has no surviving app token. It must wait
  // rather than inherit the RTSP output without retaining its generation.
  EXPECT_FALSE(coordinator.retain_normal_game_capture("game", 0));
  coordinator.release_drained_normal_game_identities();
  EXPECT_TRUE(operations.empty());

  rtsp.reset();
  EXPECT_TRUE(operations.empty());
  coordinator.release_drained_normal_game_identities();
  EXPECT_EQ(operations, (std::vector<std::string> {"apply", "remove:game"}));
  EXPECT_FALSE(coordinator.normal_game_release_pending());
}

TEST(RemoteDisplayTopology, CaptureIdleRetiresOnlyNormalGameRecoveryAndResumeCanReacquire) {
  remote_display_topology::coordinator_t coordinator;
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
  });

  const auto paused_game = coordinator.reserve_normal_game_identity("paused-game", "Paused Game", {});
  const auto shared = coordinator.reserve_normal_game_identity("shared", "Shared", {});
  ASSERT_TRUE(paused_game.accepted);
  ASSERT_TRUE(shared.accepted);
  ASSERT_TRUE(coordinator.activate_or_resume("shared", "Shared", {}, 1).ready);
  ASSERT_TRUE(coordinator.activate_or_resume("monitor", "Monitor", {}, 2).ready);
  coordinator.transport_lost("monitor", 2);

  // A live or starting peer keeps every recovery worker armed. Once capture is
  // globally idle, only the paused normal-game identity is eligible; retained
  // Remote Monitor identities keep their recovery authority.
  EXPECT_TRUE(coordinator.idle_normal_game_recovery_client_ids(true).empty());
  EXPECT_EQ(
    coordinator.idle_normal_game_recovery_client_ids(false),
    (std::vector<std::string> {"paused-game"})
  );

  // Selecting recovery retirement does not release the paused identity.
  // Resume can retain it again, and the live capture reference removes it
  // from the idle retirement set.
  auto resumed_capture = coordinator.retain_normal_game_capture("paused-game", paused_game.token);
  ASSERT_TRUE(resumed_capture);
  EXPECT_TRUE(coordinator.idle_normal_game_recovery_client_ids(false).empty());
  resumed_capture.reset();
  EXPECT_EQ(
    coordinator.idle_normal_game_recovery_client_ids(false),
    (std::vector<std::string> {"paused-game"})
  );
}

TEST(RemoteDisplayTopology, SuccessorAppWaitsForPredecessorCaptureDrain) {
  remote_display_topology::coordinator_t coordinator;
  std::vector<std::string> removed;
  coordinator.set_runtime_callbacks({
    .apply_composed_topology = [](const auto &) {
      return true;
    },
    .remove_owned_display = [&](const auto &uuid) {
      removed.push_back(uuid);
      return true;
    },
  });
  const auto old_app = coordinator.reserve_normal_game_identity("game", "Game", {});
  auto old_capture = coordinator.retain_normal_game_capture("game", old_app.token);
  coordinator.release_normal_game_identity("game", old_app.token);
  const auto blocked = coordinator.reserve_normal_game_identity("game", "Game", {2560, 1440, 120});
  EXPECT_FALSE(blocked.accepted);
  EXPECT_TRUE(coordinator.normal_game_release_pending());
  old_capture.reset();
  coordinator.release_drained_normal_game_identities();
  EXPECT_EQ(removed, (std::vector<std::string> {"game"}));

  const auto new_app = coordinator.reserve_normal_game_identity("game", "Game", {2560, 1440, 120});
  ASSERT_TRUE(new_app.accepted);
  ASSERT_NE(new_app.token, old_app.token);
  auto new_capture = coordinator.retain_normal_game_capture("game", new_app.token);
  ASSERT_TRUE(new_capture);
  coordinator.release_normal_game_identity("game", new_app.token);
  coordinator.release_drained_normal_game_identities();
  EXPECT_EQ(removed, (std::vector<std::string> {"game"}));
  new_capture.reset();
  coordinator.release_drained_normal_game_identities();
  EXPECT_EQ(removed, (std::vector<std::string> {"game", "game"}));
}

TEST(RemoteDisplayTopology, DrainedGameRetiresWhileIndependentMonitorRemainsOwned) {
  remote_display_topology::coordinator_t coordinator;
  std::vector<std::string> removed;
  std::vector<remote_display_topology::node_t> composed;
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [](const auto &, const auto &, const auto &) {
      return true;
    },
    .apply_composed_topology = [&](const auto &nodes) {
      composed = nodes;
      return true;
    },
    .exact_target_has_current_mode_and_dxgi = [](const auto &uuid, const auto &) {
      return std::optional<std::string> {uuid};
    },
    .remove_owned_display = [&](const auto &uuid) {
      removed.push_back(uuid);
      return true;
    },
  });
  const auto app = coordinator.reserve_normal_game_identity("game", "Game", {});
  auto capture = coordinator.retain_normal_game_capture("game", app.token);
  ASSERT_TRUE(coordinator.activate_or_resume("monitor", "Monitor", {}, 1).ready);
  coordinator.release_normal_game_identity("game", app.token);
  ASSERT_EQ(composed.size(), 2u);
  capture.reset();
  coordinator.release_drained_normal_game_identities();
  EXPECT_EQ(removed, (std::vector<std::string> {"game"}));
  ASSERT_EQ(composed.size(), 1u);
  EXPECT_EQ(composed.front().id, "monitor");
  EXPECT_TRUE(coordinator.is_ready("monitor", 1));
}

TEST(RemoteDisplayTopology, RejectedSuccessorCannotRemovePredecessorCapture) {
  remote_display_topology::coordinator_t coordinator;
  std::vector<std::string> removed;
  coordinator.set_runtime_callbacks({
    .apply_composed_topology = [](const auto &) {
      return true;
    },
    .remove_owned_display = [&](const auto &uuid) {
      removed.push_back(uuid);
      return true;
    },
  });
  const auto old_app = coordinator.reserve_normal_game_identity("game", "Game", {});
  auto old_capture = coordinator.retain_normal_game_capture("game", old_app.token);
  coordinator.release_normal_game_identity("game", old_app.token);
  const auto successor = coordinator.reserve_normal_game_identity("game", "Game", {});
  EXPECT_FALSE(successor.accepted);
  EXPECT_FALSE(coordinator.rollback_normal_game_identity("game", old_app.token));
  coordinator.release_drained_normal_game_identities();
  EXPECT_TRUE(removed.empty());
  EXPECT_EQ(coordinator.managed_client_identity_count(), 1u);
  old_capture.reset();
  coordinator.release_drained_normal_game_identities();
  EXPECT_EQ(removed, (std::vector<std::string> {"game"}));
  EXPECT_EQ(coordinator.managed_client_identity_count(), 0u);
}

TEST(RemoteDisplayTopology, SharedMonitorSurvivesDeferredNormalReleaseAndFailedRestoreRetries) {
  remote_display_topology::coordinator_t coordinator;
  bool allow_apply = true;
  std::vector<std::string> removed;
  coordinator.set_runtime_callbacks({
    .create_or_reclaim = [](const auto &, const auto &, const auto &) {
      return true;
    },
    .apply_composed_topology = [&](const auto &) {
      return allow_apply;
    },
    .exact_target_has_current_mode_and_dxgi = [](const auto &uuid, const auto &) {
      return std::optional<std::string> {uuid};
    },
    .remove_owned_display = [&](const auto &uuid) {
      removed.push_back(uuid);
      return true;
    },
  });
  const auto shared = coordinator.reserve_normal_game_identity("shared", "Shared", {});
  auto shared_capture = coordinator.retain_normal_game_capture("shared", shared.token);
  ASSERT_TRUE(coordinator.activate_or_resume("shared", "Shared", {}, 1).ready);
  coordinator.release_normal_game_identity("shared", shared.token);
  shared_capture.reset();
  coordinator.release_drained_normal_game_identities();
  EXPECT_TRUE(removed.empty());
  EXPECT_TRUE(coordinator.is_ready("shared", 1));

  const auto app = coordinator.reserve_normal_game_identity("game", "Game", {});
  auto capture = coordinator.retain_normal_game_capture("game", app.token);
  coordinator.release_normal_game_identity("game", app.token);
  capture.reset();
  allow_apply = false;
  coordinator.release_drained_normal_game_identities();
  EXPECT_TRUE(removed.empty());
  EXPECT_EQ(coordinator.managed_client_identity_count(), 2u);
  allow_apply = true;
  coordinator.release_drained_normal_game_identities();
  EXPECT_EQ(removed, (std::vector<std::string> {"game"}));
  EXPECT_EQ(coordinator.managed_client_identity_count(), 1u);
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

TEST(RemoteDisplayTopology, NormalStreamUsesSavedBelowCenterWithoutRecomposingPeers) {
  remote_display_topology::coordinator_t coordinator;
  coordinator.set_layout(layout({{"one", {{"anchor_kind", "physical"}, {"anchor_id", "primary"}, {"edge", "below"}, {"alignment", "center"}, {"gap_px", 0}}}}));
  const remote_display_topology::node_t primary {.id = "primary", .physical = true, .active = true, .primary = true, .configured_mode = {3840, 2160, 60}};
  const remote_display_topology::node_t target {.id = "one", .active = true, .x = 3840, .configured_mode = {2732, 2048, 60}};
  const remote_display_topology::node_t peer {.id = "two", .active = true, .x = -1920, .y = 100};
  const auto placed = coordinator.saved_stream_placement(target, {primary, peer});
  ASSERT_TRUE(placed);
  EXPECT_EQ(placed->x, 554);
  EXPECT_EQ(placed->y, 2160);
  EXPECT_EQ(peer.x, -1920);
  EXPECT_EQ(peer.y, 100);
  EXPECT_EQ(coordinator.managed_client_identity_count(), 0u);
}

TEST(RemoteDisplayTopology, NormalStreamManualAndUnavailableAnchorsPreserveWindowsPosition) {
  remote_display_topology::coordinator_t coordinator;
  const remote_display_topology::node_t target {.id = "one", .active = true, .x = 554, .y = 2160};
  EXPECT_FALSE(coordinator.saved_stream_placement(target, {}));
  coordinator.set_layout(layout({{"one", {{"anchor_kind", "physical"}, {"anchor_id", "absent"}, {"edge", "right"}, {"alignment", "start"}, {"gap_px", 0}}}}));
  EXPECT_FALSE(coordinator.saved_stream_placement(target, {}));
  const remote_display_topology::node_t inactive {.id = "absent", .physical = true};
  EXPECT_FALSE(coordinator.saved_stream_placement(target, {inactive}));
  EXPECT_EQ(target.x, 554);
  EXPECT_EQ(target.y, 2160);
}

TEST(RemoteDisplayTopology, NormalStreamUsesLiveClientAnchorAndRequestedTargetDimensions) {
  remote_display_topology::coordinator_t coordinator;
  const remote_display_topology::node_t anchor {.id = "two", .active = true, .x = -1920, .y = 100, .configured_mode = {1920, 1080, 60}};
  const remote_display_topology::node_t target {.id = "one", .active = true, .configured_mode = {1280, 720, 60}};
  for (const auto &edge : {"left", "right", "above", "below"}) {
    for (const auto &alignment : {"start", "center", "end"}) {
      coordinator.set_layout(layout({{"one", {{"anchor_kind", "client"}, {"anchor_id", "two"}, {"edge", edge}, {"alignment", alignment}, {"gap_px", 8}, {"primary", true}}}}));
      const auto placed = coordinator.saved_stream_placement(target, {anchor});
      ASSERT_TRUE(placed);
      EXPECT_TRUE(placed->primary);
      if (std::string(edge) == "left") {
        EXPECT_EQ(placed->x, -3208);
      }
      if (std::string(edge) == "right") {
        EXPECT_EQ(placed->x, 8);
      }
      if (std::string(edge) == "above") {
        EXPECT_EQ(placed->y, -628);
      }
      if (std::string(edge) == "below") {
        EXPECT_EQ(placed->y, 1188);
      }
      if (std::string(edge) == "left" || std::string(edge) == "right") {
        EXPECT_EQ(placed->y, std::string(alignment) == "start" ? 100 : std::string(alignment) == "end" ? 460 :
                                                                                                         280);
      } else {
        EXPECT_EQ(placed->x, std::string(alignment) == "start" ? -1920 : std::string(alignment) == "end" ? -1280 :
                                                                                                           -1600);
      }
    }
  }
}

TEST(RemoteDisplayTopology, ExplicitManualRulePreservesIsolatedTargetWithoutAnAnchor) {
  remote_display_topology::coordinator_t coordinator;
  const auto manual = layout({{"one", {{"anchor_kind", "physical"}, {"anchor_id", ""}, {"edge", "right"}, {"alignment", "center"}, {"gap_px", 0}, {"preserve", true}}}});
  std::string error;
  ASSERT_TRUE(remote_display_topology::validate_layout(manual, clients, {}, error)) << error;
  coordinator.set_layout(manual);
  const remote_display_topology::node_t target {.id = "one", .active = true, .x = 554, .y = 2160};
  const auto placed = coordinator.saved_stream_placement(target, {});
  ASSERT_TRUE(placed);
  EXPECT_EQ(placed->x, 554);
  EXPECT_EQ(placed->y, 2160);
  EXPECT_FALSE(placed->primary);
}
