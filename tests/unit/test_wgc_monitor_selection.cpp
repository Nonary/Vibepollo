/**
 * @file tests/unit/test_wgc_monitor_selection.cpp
 * @brief Capture target selection across monitor topology changes.
 */
#include "../tests_common.h"

#include <src/platform/windows/wgc_capture_policy.h>

#include <algorithm>
#include <chrono>
#include <vector>

using namespace std::chrono_literals;

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

TEST(WgcActivityFrameLimiter, PreservesJitteredCadenceBelowTheLimit) {
  using limiter_t = platf::dxgi::wgc_policy::activity_frame_limiter_t;
  limiter_t limiter;
  limiter.reset(120);

  const limiter_t::clock_t::time_point start {};
  for (const auto offset : {0ms, 7ms, 17ms, 24ms, 34ms, 41ms, 51ms}) {
    EXPECT_TRUE(limiter.admit(start + offset)) << "offset=" << offset.count() << "ms";
  }
}

TEST(WgcActivityFrameLimiter, PreservesBursty393FpsCallbacksUnderA480FpsLimit) {
  using limiter_t = platf::dxgi::wgc_policy::activity_frame_limiter_t;
  limiter_t limiter;
  limiter.reset(480);

  auto arrival = limiter_t::clock_t::time_point {};
  for (int frame = 0; frame < 393; ++frame) {
    EXPECT_TRUE(limiter.admit(arrival)) << "frame=" << frame;
    // This alternating pair averages 393 fps, but the old minimum-gap gate
    // discarded every 1 ms half despite the source staying below 480 fps.
    arrival += frame % 2 == 0 ? 1ms : 4089058ns;
  }
}

TEST(WgcActivityFrameLimiter, CapsPersistentOversupply) {
  using limiter_t = platf::dxgi::wgc_policy::activity_frame_limiter_t;
  limiter_t limiter;
  limiter.reset(120);

  const limiter_t::clock_t::time_point start {};
  int admitted = 0;
  for (int milliseconds = 0; milliseconds < 1000; milliseconds += 2) {
    admitted += limiter.admit(start + std::chrono::milliseconds(milliseconds)) ? 1 : 0;
  }

  // One saved phase frame may lead the long-term 120 fps ceiling.
  EXPECT_GE(admitted, 120);
  EXPECT_LE(admitted, 121);
}

TEST(WgcActivityFrameLimiter, DoesNotBankAStallAsCatchUpBurst) {
  using limiter_t = platf::dxgi::wgc_policy::activity_frame_limiter_t;
  limiter_t limiter;
  limiter.reset(120);

  const limiter_t::clock_t::time_point start {};
  EXPECT_TRUE(limiter.admit(start));
  EXPECT_TRUE(limiter.admit(start + 100ms));
  EXPECT_FALSE(limiter.admit(start + 101ms));
}

TEST(WgcActivityFrameLimiter, ResetAdmitsTheNewRateImmediately) {
  using limiter_t = platf::dxgi::wgc_policy::activity_frame_limiter_t;
  limiter_t limiter;
  limiter.reset(120);

  const limiter_t::clock_t::time_point start {};
  EXPECT_TRUE(limiter.admit(start));
  EXPECT_TRUE(limiter.admit(start + 1ms));
  EXPECT_FALSE(limiter.admit(start + 2ms));

  limiter.reset(240);
  EXPECT_TRUE(limiter.admit(start + 2ms));
}
