#include "src/platform/windows/display_restore_guard.h"
#include "src/platform/windows/display_snapshot_restore.h"

#include <display_device/types.h>
#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
  using Topology = std::vector<std::vector<std::string>>;
  struct Layout {
    Topology topology {{"MSI"}, {"AOC"}};
    std::optional<std::string> primary_device {"MSI"};
    std::map<std::string, display_device::Point> monitor_positions {{"MSI", {0, 0}}, {"AOC", {3840, 0}}};
    std::map<std::string, std::pair<unsigned, unsigned>> device_refresh_rate_overrides {{"MSI", {240, 4}}};
  };
  struct Mode { display_device::Resolution m_resolution; display_device::Rational m_refresh_rate; };
  using Modes = std::map<std::string, Mode>;
  struct Device {
    Topology topology {{"VDD"}};
    std::vector<std::string> calls;
    Modes applied;
    bool topology_ok = true;
    bool primary_ok = true;
    bool origin_ok = true;
    bool refresh_ok = true;
    bool refresh_throws = false;
    bool modes_readable = true;
    auto getCurrentTopology() { return topology; }
    bool isTopologyTheSame(const Topology &a, const Topology &b) { return a == b; }
    bool setTopology(const Topology &value) {
      calls.push_back("topology");
      if (topology_ok) topology = value;
      return topology_ok;
    }
    bool setAsPrimary(const std::string &) { calls.push_back("primary"); return primary_ok; }
    bool setDisplayOrigin(const std::string &id, const display_device::Point &) { calls.push_back("origin:" + id); return origin_ok; }
    Modes getCurrentDisplayModes(const std::set<std::string> &) {
      calls.push_back("read modes");
      if (!modes_readable) return {};
      return {{"MSI", {{3840, 2160}, {59997, 1000}}}};
    }
    bool setDisplayModesExactTemporary(const Modes &modes) {
      calls.push_back("refresh");
      if (refresh_throws) throw std::runtime_error("refresh failed");
      applied = modes;
      return refresh_ok;
    }
  };
  const auto ignore_failure = [](const auto &, const auto &) {};
  const auto ignore_skip = [](const char *) {};
  const auto connected = [] { return false; };
  const auto capture = [](const auto &) { return true; };
}

TEST(DisplayCoordinatorRestore, ReactivatesThenRestoresPrimaryOriginsAndVerbatimRefresh) {
  Device device;
  EXPECT_TRUE(display_helper::restore_composed_topology(device, Layout {}, ignore_failure));
  EXPECT_EQ(device.calls, (std::vector<std::string> {"topology", "primary", "origin:AOC", "origin:MSI", "read modes", "refresh"}));
  ASSERT_TRUE(device.applied.contains("MSI"));
  EXPECT_EQ(device.applied.at("MSI").m_refresh_rate.m_numerator, 240u);
  EXPECT_EQ(device.applied.at("MSI").m_refresh_rate.m_denominator, 4u);
}

TEST(DisplayCoordinatorRestore, RefreshFailureWarnsWithDeviceAndRequestedReadbackRationals) {
  Device device;
  device.refresh_ok = false;
  std::vector<std::pair<std::string, std::string>> warnings;
  EXPECT_TRUE(display_helper::restore_composed_topology(device, Layout {}, [&](const auto &reason, const auto &id) {
    warnings.emplace_back(reason, id);
  }));
  EXPECT_EQ(device.calls, (std::vector<std::string> {"topology", "primary", "origin:AOC", "origin:MSI", "read modes", "refresh", "read modes"}));
  ASSERT_EQ(warnings.size(), 1u);
  EXPECT_EQ(warnings.front().second, "MSI");
  EXPECT_NE(warnings.front().first.find("requested 240/4"), std::string::npos);
  EXPECT_NE(warnings.front().first.find("readback 59997/1000"), std::string::npos);
}

TEST(DisplayCoordinatorRestore, GeometrySuccessAndRefreshFailureAdmitsNextSnapshot) {
  Device device;
  device.refresh_ok = false;
  display_helper_integration::DisplayRestoreGuard guard;
  ASSERT_FALSE(guard.compose([] { return false; }));
  EXPECT_TRUE(guard.compose([&] { return display_helper::restore_composed_topology(device, Layout {}, ignore_failure); }));
  EXPECT_FALSE(guard.protects_baseline());
  EXPECT_TRUE(guard.snapshot(capture, connected, ignore_skip));
}

TEST(DisplayCoordinatorRestore, RefreshReadFailureOrExceptionDoesNotProtectRepairedGeometry) {
  for (const bool missing_modes : {false, true}) {
    Device device;
    device.modes_readable = !missing_modes;
    device.refresh_throws = !missing_modes;
    display_helper_integration::DisplayRestoreGuard guard;
    std::vector<std::string> warnings;
    EXPECT_TRUE(guard.compose([&] {
      return display_helper::restore_composed_topology(device, Layout {}, [&](const auto &reason, const auto &id) {
        EXPECT_EQ(id, "MSI");
        warnings.push_back(reason);
      });
    }));
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_NE(warnings.front().find("requested 240/4"), std::string::npos);
    EXPECT_NE(warnings.front().find(missing_modes ? "readback unavailable" : "readback 59997/1000"), std::string::npos);
    EXPECT_FALSE(guard.protects_baseline());
    EXPECT_TRUE(guard.snapshot(capture, connected, ignore_skip));
  }
}

TEST(DisplayCoordinatorRestore, AnyGeometryFailureProtectsBaselineAndRejectsSnapshot) {
  for (const auto stage : {"topology", "primary", "origin"}) {
    SCOPED_TRACE(stage);
    Device device;
    device.topology_ok = std::string(stage) != "topology";
    device.primary_ok = std::string(stage) != "primary";
    device.origin_ok = std::string(stage) != "origin";
    display_helper_integration::DisplayRestoreGuard guard;
    EXPECT_FALSE(guard.compose([&] { return display_helper::restore_composed_topology(device, Layout {}, ignore_failure); }));
    EXPECT_TRUE(guard.protects_baseline());
    EXPECT_FALSE(guard.snapshot(capture, connected, ignore_skip));
  }
}

TEST(DisplayCoordinatorRestore, TopologyFailureStopsSettingsOnInactiveDevices) {
  Device device;
  device.topology_ok = false;
  EXPECT_FALSE(display_helper::restore_composed_topology(device, Layout {}, ignore_failure));
  EXPECT_EQ(device.calls, (std::vector<std::string> {"topology"}));
}

TEST(DisplayCoordinatorRestore, ExclusiveApplySkipsInactivePhysicalRefreshTargets) {
  Device device;
  Layout layout;
  layout.topology = {{"VDD"}};
  layout.primary_device = "VDD";
  layout.monitor_positions = {{"VDD", {0, 0}}};
  EXPECT_TRUE(display_helper::restore_composed_topology(device, layout, ignore_failure));
  EXPECT_EQ(device.calls, (std::vector<std::string> {"primary", "origin:VDD"}));
}

TEST(DisplayRestoreGuard, NormalCaptureNeverQueriesConnectivity) {
  display_helper_integration::DisplayRestoreGuard guard;
  EXPECT_TRUE(guard.snapshot(capture, [] { ADD_FAILURE(); return false; }, ignore_skip));
}

TEST(DisplayRestoreGuard, StableButUnrestoredLayoutRemainsBlockedUntilSuccessfulCompose) {
  display_helper_integration::DisplayRestoreGuard guard;
  ASSERT_FALSE(guard.compose([] { return false; }));
  int warnings = 0;
  for (int i = 0; i < 2; ++i) {
    EXPECT_FALSE(guard.snapshot([](const auto &) { ADD_FAILURE(); return true; }, connected,
      [&](const char *reason) { ++warnings; EXPECT_NE(std::string(reason).find("failed compose"), std::string::npos); }));
  }
  EXPECT_EQ(warnings, 2);
  EXPECT_TRUE(guard.compose([] { return true; }));
  EXPECT_TRUE(guard.snapshot(capture, connected, ignore_skip));
}

TEST(DisplayRestoreGuard, DisconnectedBaselineAdmitsRecapture) {
  display_helper_integration::DisplayRestoreGuard guard;
  ASSERT_FALSE(guard.compose([] { return false; }));
  EXPECT_TRUE(guard.snapshot(capture, [] { return true; }, ignore_skip));
  EXPECT_TRUE(guard.snapshot(capture, [] { ADD_FAILURE(); return false; }, ignore_skip));
}

TEST(DisplayRestoreGuard, ConcurrentComposeInvalidatesConnectivityEvidence) {
  display_helper_integration::DisplayRestoreGuard guard;
  ASSERT_FALSE(guard.compose([] { return false; }));
  EXPECT_FALSE(guard.snapshot(capture, [&] {
    EXPECT_FALSE(guard.compose([] { return false; }));
    return true;
  }, ignore_skip));
  EXPECT_FALSE(guard.snapshot(capture, connected, ignore_skip));
}

TEST(DisplayRestoreGuard, HungSnapshotDoesNotBlockComposeAndCannotReportStaleSuccess) {
  display_helper_integration::DisplayRestoreGuard guard;
  std::promise<void> entered;
  std::promise<void> release;
  auto released = release.get_future();
  auto snapshot = std::async(std::launch::async, [&] {
    return guard.snapshot([&](const auto &) { entered.set_value(); released.wait(); return true; }, connected, ignore_skip);
  });
  entered.get_future().wait();
  auto compose = std::async(std::launch::async, [&] { return guard.compose([] { return true; }); });
  const auto status = compose.wait_for(std::chrono::seconds(2));
  release.set_value();  // Always release, including a regression that holds the lock.
  EXPECT_EQ(status, std::future_status::ready);
  EXPECT_TRUE(compose.get());
  EXPECT_FALSE(snapshot.get());
}
