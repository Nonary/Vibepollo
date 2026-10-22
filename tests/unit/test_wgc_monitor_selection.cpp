/**
 * @file tests/unit/test_wgc_monitor_selection.cpp
 * @brief Capture target selection across monitor topology changes.
 */
#include "../tests_common.h"

#include <src/platform/windows/wgc_capture_policy.h>

#include <algorithm>
#include <limits>
#include <vector>

namespace {
  struct monitor_topology {
    int primary = 1;
    std::vector<int> requested {16};
    std::size_t enumeration = 0;
    int primary_queries = 0;
    int waits = 0;
    int retry_budget = 3;

    int select(const bool explicit_target = true) {
      return platf::dxgi::wgc_policy::select_monitor(
        explicit_target,
        [&] {
          const auto index = std::min(enumeration++, requested.size() - 1);
          return requested[index];
        },
        [&] {
          ++primary_queries;
          return primary;
        },
        [&] {
          if (waits >= retry_budget) {
            return false;
          }
          ++waits;
          return true;
        }
      );
    }
  };
}  // namespace

TEST(WgcMonitorSelection, ExplicitTargetDoesNotQueryPrimary) {
  monitor_topology topology;
  EXPECT_EQ(topology.select(), 16);
  EXPECT_EQ(topology.primary_queries, 0);
  EXPECT_EQ(topology.waits, 0);
}

TEST(WgcMonitorSelection, MissingExplicitTargetDoesNotCaptureAvailablePrimary) {
  monitor_topology topology;
  topology.requested = {0};
  EXPECT_EQ(topology.select(), 0);
  EXPECT_EQ(topology.primary_queries, 0);
  EXPECT_EQ(topology.waits, topology.retry_budget);
}

TEST(WgcMonitorSelection, TargetCanReappearDuringTopologySettle) {
  monitor_topology topology;
  topology.requested = {0, 0, 16};
  EXPECT_EQ(topology.select(), 16);
  EXPECT_EQ(topology.primary_queries, 0);
  EXPECT_EQ(topology.waits, 2);
}

TEST(WgcMonitorSelection, ReselectionDoesNotKeepADisconnectedHandle) {
  monitor_topology topology;
  topology.requested = {16, 0};
  EXPECT_EQ(topology.select(), 16);
  EXPECT_EQ(topology.select(), 0);
  EXPECT_EQ(topology.primary_queries, 0);
}

TEST(WgcMonitorSelection, EmptyTargetUsesPrimaryWithoutWaitingOrEnumeration) {
  monitor_topology topology;
  EXPECT_EQ(topology.select(false), 1);
  EXPECT_EQ(topology.primary_queries, 1);
  EXPECT_EQ(topology.enumeration, 0u);
  EXPECT_EQ(topology.waits, 0);
}

TEST(WgcMonitorSelection, EmptyTargetReportsUnavailablePrimary) {
  monitor_topology topology;
  topology.primary = 0;
  EXPECT_EQ(topology.select(false), 0);
  EXPECT_EQ(topology.primary_queries, 1);
  EXPECT_EQ(topology.enumeration, 0u);
  EXPECT_EQ(topology.waits, 0);
}

TEST(WgcInputGeometry, RefreshOnlyChangeKeepsInputMapping) {
  using namespace platf::dxgi::wgc_policy;
  EXPECT_EQ(assess_input_geometry({1920, 0, 4480, 1600}, 1920, 0, {0, 0, 4480, 1600}), input_geometry_change_e::unchanged);
}

TEST(WgcInputGeometry, NegativeDesktopOriginNormalizesTargetExactlyOnce) {
  using namespace platf::dxgi::wgc_policy;
  EXPECT_EQ(assess_input_geometry({1920, 1080, 4480, 2680}, 0, 0, {-1920, -1080, 4480, 2680}), input_geometry_change_e::unchanged);
  EXPECT_EQ(assess_input_geometry({0, 0, 4480, 2680}, -1920, -1080, {-1920, -1080, 4480, 2680}), input_geometry_change_e::unchanged);
}

TEST(WgcInputGeometry, MovingAnotherMonitorChangesNormalization) {
  using namespace platf::dxgi::wgc_policy;
  // Target stays at (0, 0); another output moves farther right/below.
  EXPECT_EQ(assess_input_geometry({0, 0, 4480, 1600}, 0, 0, {0, 0, 6400, 1600}), input_geometry_change_e::changed);
  EXPECT_EQ(assess_input_geometry({0, 0, 2560, 2680}, 0, 0, {0, 0, 2560, 3200}), input_geometry_change_e::changed);
}

TEST(WgcInputGeometry, ReinitializationRebuildsOffsetsAfterNeighbourCrossesOrigin) {
  using namespace platf::dxgi::wgc_policy;
  const desktop_bounds_t moved {-1920, -1080, 4480, 2680};
  EXPECT_EQ(assess_input_geometry({0, 0, 4480, 2680}, 0, 0, moved), input_geometry_change_e::changed);
  EXPECT_EQ(assess_input_geometry({1920, 1080, 4480, 2680}, 0, 0, moved), input_geometry_change_e::unchanged);
}

TEST(WgcInputGeometry, UnavailableMetricsDoNotForceCaptureReinitialization) {
  using namespace platf::dxgi::wgc_policy;
  const input_geometry_t captured {1920, 0, 4480, 1600};
  for (const auto bounds : {desktop_bounds_t {0, 0, 0, 1600}, {0, 0, 4480, 0}, {0, 0, -1, 1600}, {0, 0, 4480, -1}}) {
    EXPECT_EQ(assess_input_geometry(captured, 1920, 0, bounds), input_geometry_change_e::unavailable);
  }
  EXPECT_EQ(assess_input_geometry(captured, 1920, 0, {0, 0, 4480, 1600}), input_geometry_change_e::unchanged);
}
TEST(WgcGeometryUpdatePolicy, VerifiedDesktopOnlyChangePublishesMapping) {
  using namespace platf::dxgi::wgc_policy;
  const input_geometry_t captured {0, 0, 3840, 2160};
  const auto decision = assess_verified_geometry_update(
    active_capture_state_e::valid, verified_target_state_e::same, true,
    captured, 0, 0, {0, 0, 7680, 2160});
  EXPECT_EQ(decision.action, geometry_update_action_e::publish);
  EXPECT_EQ(decision.mapping.offset_x, 0);
  EXPECT_EQ(decision.mapping.offset_y, 0);
  EXPECT_EQ(decision.mapping.desktop_width, 7680);
  EXPECT_EQ(decision.mapping.desktop_height, 2160);
}

TEST(WgcGeometryUpdatePolicy, StableTargetDesktopEnvelopeChangePreservesCapture) {
  using namespace platf::dxgi::wgc_policy;
  const input_geometry_t captured {0, 0, 3840, 2160};
  const desktop_bounds_t expanded {0, 0, 7680, 2160};
  const auto change = assess_input_geometry(captured, 0, 0, expanded);
  ASSERT_EQ(change, input_geometry_change_e::changed);

  // Frame-push uses the same verified geometry policy as WGC. The unchanged
  // target can publish the updated envelope without restarting capture.
  EXPECT_FALSE(input_geometry_change_requires_capture_restart(true, change));
  const auto verified = assess_verified_geometry_update(
    active_capture_state_e::valid, verified_target_state_e::same, true,
    captured, 0, 0, expanded);
  EXPECT_EQ(verified.action, geometry_update_action_e::publish);
  EXPECT_EQ(verified.mapping.desktop_width, 7680);

  // Negative controls retain reinitialization for legacy capture and for any
  // change that affects the selected DXGI target itself.
  EXPECT_TRUE(input_geometry_change_requires_capture_restart(false, change));
  const auto target_changed = assess_verified_geometry_update(
    active_capture_state_e::valid, verified_target_state_e::changed, true,
    captured, 0, 0, expanded);
  EXPECT_EQ(target_changed.action, geometry_update_action_e::reinitialize);
  EXPECT_FALSE(input_geometry_change_requires_capture_restart(false, input_geometry_change_e::unavailable));
}

TEST(WgcGeometryUpdatePolicy, NegativeOriginUpdatesTargetOffsetExactlyOnce) {
  using namespace platf::dxgi::wgc_policy;
  const input_geometry_t captured {0, 0, 4480, 2680};
  const auto decision = assess_verified_geometry_update(
    active_capture_state_e::valid, verified_target_state_e::same, true,
    captured, 0, 0, {-1920, -1080, 4480, 2680});
  EXPECT_EQ(decision.action, geometry_update_action_e::publish);
  EXPECT_EQ(decision.mapping.offset_x, 1920);
  EXPECT_EQ(decision.mapping.offset_y, 1080);
}

TEST(WgcGeometryUpdatePolicy, HmonitorTargetChangeWinsOverDesktopGeometryRefresh) {
  using namespace platf::dxgi::wgc_policy;
  // The Windows caller maps a fresh DXGI HMONITOR mismatch to `changed`.
  // Even while the virtual desktop is growing, that identity change stays on
  // the existing capture-reinitialization path.
  const auto decision = assess_verified_geometry_update(
    active_capture_state_e::valid, verified_target_state_e::changed, true,
    {0, 0, 3840, 2160}, 0, 0, {0, 0, 7680, 2160});
  EXPECT_EQ(decision.action, geometry_update_action_e::reinitialize);
}

TEST(WgcGeometryUpdatePolicy, ClosedOrInvalidCaptureStillReinitializes) {
  using namespace platf::dxgi::wgc_policy;
  const input_geometry_t captured {0, 0, 3840, 2160};
  for (const auto state : {active_capture_state_e::invalid, active_capture_state_e::item_closed}) {
    const auto decision = assess_verified_geometry_update(
      state, verified_target_state_e::same, true, captured, 0, 0, {0, 0, 7680, 2160});
    EXPECT_EQ(decision.action, geometry_update_action_e::reinitialize);
    EXPECT_EQ(decision.mapping, captured);
  }
}

TEST(WgcGeometryUpdatePolicy, UnknownTargetOrMetricsInvalidateAndRetry) {
  using namespace platf::dxgi::wgc_policy;
  const input_geometry_t captured {0, 0, 3840, 2160};
  const auto unknown_target = assess_verified_geometry_update(
    active_capture_state_e::valid, verified_target_state_e::unavailable, true,
    captured, 0, 0, {0, 0, 7680, 2160});
  EXPECT_EQ(unknown_target.action, geometry_update_action_e::invalidate_and_retry);
  EXPECT_EQ(unknown_target.mapping, captured);

  const auto unknown_metrics = assess_verified_geometry_update(
    active_capture_state_e::valid, verified_target_state_e::same, true,
    captured, 0, 0, {0, 0, 0, 2160});
  EXPECT_EQ(unknown_metrics.action, geometry_update_action_e::invalidate_and_retry);
  EXPECT_EQ(unknown_metrics.mapping, captured);
  const auto still_invalid = assess_verified_geometry_update(
    active_capture_state_e::valid, verified_target_state_e::same, false,
    captured, 0, 0, {0, 0, 0, 2160});
  EXPECT_EQ(still_invalid.action, geometry_update_action_e::retry);
}

TEST(WgcGeometryUpdatePolicy, ValidSampleRestoresPreviouslyInvalidMapping) {
  using namespace platf::dxgi::wgc_policy;
  const input_geometry_t captured {1920, 0, 4480, 1600};
  const auto decision = assess_verified_geometry_update(
    active_capture_state_e::valid, verified_target_state_e::same, false,
    captured, 1920, 0, {0, 0, 4480, 1600});
  EXPECT_EQ(decision.action, geometry_update_action_e::publish);
  EXPECT_EQ(decision.mapping, captured);
}

TEST(WgcGeometryUpdatePolicy, ChangedTargetInvalidatesMappingBeforeReinitialize) {
  using namespace platf::dxgi::wgc_policy;
  const input_geometry_t captured {1920, 0, 4480, 1600};
  bool mapping_valid = true;
  int invalidations = 0;
  const auto invalidate = [&] { ++invalidations; };

  EXPECT_TRUE(invalidate_changed_target_geometry_before_reinitialize(true, mapping_valid, invalidate));
  EXPECT_FALSE(mapping_valid);
  EXPECT_EQ(invalidations, 1);

  // Repeated observations do not republish or invalidate an already-invalid map.
  EXPECT_TRUE(invalidate_changed_target_geometry_before_reinitialize(true, mapping_valid, invalidate));
  EXPECT_FALSE(mapping_valid);
  EXPECT_EQ(invalidations, 1);

  // Desktop Duplication does not opt into capture-preserving geometry updates.
  bool duplication_mapping_valid = true;
  EXPECT_FALSE(invalidate_changed_target_geometry_before_reinitialize(false, duplication_mapping_valid, invalidate));
  EXPECT_TRUE(duplication_mapping_valid);
  EXPECT_EQ(invalidations, 1);

  const auto target_changed = assess_verified_geometry_update(
    active_capture_state_e::valid, verified_target_state_e::changed, false,
    captured, 0, 0, {0, 0, 4480, 1600});
  EXPECT_EQ(target_changed.action, geometry_update_action_e::reinitialize);
}

TEST(WgcGeometryUpdatePolicy, InitialWgcMappingWaitsForFirstVerifiedObservation) {
  using namespace platf::dxgi::wgc_policy;
  const input_geometry_t captured {1920, 0, 4480, 1600};

  // A WGC session starts without a verified input map; even unchanged system
  // metrics require one fresh target observation before absolute input opens.
  EXPECT_TRUE(input_geometry_observation_needed(true, false, input_geometry_change_e::unchanged));
  EXPECT_FALSE(input_geometry_observation_needed(true, true, input_geometry_change_e::unchanged));
  EXPECT_TRUE(input_geometry_observation_needed(true, true, input_geometry_change_e::changed));
  EXPECT_FALSE(input_geometry_observation_needed(false, false, input_geometry_change_e::changed));

  const auto verified = assess_verified_geometry_update(
    active_capture_state_e::valid, verified_target_state_e::same, false,
    captured, 1920, 0, {0, 0, 4480, 1600});
  EXPECT_EQ(verified.action, geometry_update_action_e::publish);
  EXPECT_EQ(verified.mapping, captured);
}

TEST(WgcGeometryUpdatePolicy, UnchangedValidMappingDoesNotRepublish) {
  using namespace platf::dxgi::wgc_policy;
  const input_geometry_t captured {1920, 0, 4480, 1600};
  const auto decision = assess_verified_geometry_update(
    active_capture_state_e::valid, verified_target_state_e::same, true,
    captured, 1920, 0, {0, 0, 4480, 1600});
  EXPECT_EQ(decision.action, geometry_update_action_e::unchanged);
  EXPECT_EQ(decision.mapping, captured);
}

TEST(WgcGeometryUpdatePolicy, UnrepresentableOffsetCannotPublish) {
  using namespace platf::dxgi::wgc_policy;
  const input_geometry_t captured {0, 0, 3840, 2160};
  const auto decision = assess_verified_geometry_update(
    active_capture_state_e::valid, verified_target_state_e::same, true,
    captured, std::numeric_limits<int>::max(), 0,
    {std::numeric_limits<int>::min(), 0, 7680, 2160});
  EXPECT_EQ(decision.action, geometry_update_action_e::invalidate_and_retry);
  EXPECT_EQ(decision.mapping, captured);
}

TEST(WgcGeometryUpdatePolicy, OnlyTheSameAttachedDxgiTargetPassesTheContinuationGate) {
  using namespace platf::dxgi::wgc_policy;
  const output_target_identity_t captured {
    17, 23, L"\\\\.\\DISPLAY1", 0x1234, true,
    -1920, -1080, 0, 0, 1, true, true,
  };
  EXPECT_TRUE(matches_captured_target(captured, captured));

  std::vector<output_target_identity_t> changed_targets;
  const auto add_change = [&](auto change) {
    auto observed = captured;
    change(observed);
    changed_targets.push_back(observed);
  };
  add_change([](auto &target) { ++target.adapter_high_part; });
  add_change([](auto &target) { ++target.adapter_low_part; });
  add_change([](auto &target) { target.output_name = L"\\\\.\\DISPLAY2"; });
  add_change([](auto &target) { target.attached_to_desktop = false; });
  add_change([](auto &target) { ++target.monitor; });
  add_change([](auto &target) { ++target.rect_left; });
  add_change([](auto &target) { ++target.rect_top; });
  add_change([](auto &target) { ++target.rect_right; });
  add_change([](auto &target) { ++target.rect_bottom; });
  add_change([](auto &target) { ++target.rotation; });
  add_change([](auto &target) { target.hdr_valid = false; });
  add_change([](auto &target) { target.hdr = false; });

  for (const auto &observed : changed_targets) {
    EXPECT_FALSE(matches_captured_target(captured, observed));
  }
}

TEST(WgcGeometryUpdatePolicy, WgcRechecksTargetIdentityOnPollEvenWithColorEventsSubscribed) {
  using namespace platf::dxgi::wgc_policy;
  // Color events can be subscribed while the selected output changes without
  // changing the virtual desktop bounds. WGC still periodically verifies the
  // full target identity; Desktop Duplication keeps its prior event policy.
  EXPECT_TRUE(target_observation_needed(false, false, false, true, false, true, true));
  EXPECT_FALSE(target_observation_needed(false, false, false, true, false, true, false));
  EXPECT_FALSE(target_observation_needed(false, false, false, true, false, false, true));
  EXPECT_TRUE(target_observation_needed(false, false, false, true, true, false, true));
  EXPECT_TRUE(target_observation_needed(true, false, false, true, false, false, false));
  EXPECT_TRUE(target_observation_needed(false, true, false, true, false, false, false));
  EXPECT_TRUE(target_observation_needed(false, false, true, true, false, false, false));
  EXPECT_TRUE(target_observation_needed(false, false, false, false, false, false, true));
}
