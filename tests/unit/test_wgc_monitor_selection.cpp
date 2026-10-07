/**
 * @file tests/unit/test_wgc_monitor_selection.cpp
 * @brief Capture target selection across monitor topology changes.
 */
#include "../tests_common.h"

#include <src/platform/windows/wgc_capture_policy.h>

#include <algorithm>
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
