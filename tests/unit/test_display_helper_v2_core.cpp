/**
 * @file tests/unit/test_display_helper_v2_core.cpp
 * @brief Unit tests for display helper v2 core components.
 */
#include "../tests_common.h"

#include "src/platform/windows/display_helper_v2/operations.h"
#include "src/platform/windows/display_helper_v2/runtime_support.h"
#include "src/platform/windows/display_helper_v2/snapshot.h"

#include <filesystem>
#include <future>

namespace {
  class FakeClock final : public display_helper::v2::IClock {
  public:
    std::chrono::steady_clock::time_point now() override {
      return now_;
    }

    void sleep_for(std::chrono::milliseconds duration) override {
      now_ += duration;
    }

    void advance(std::chrono::milliseconds duration) {
      now_ += duration;
    }

  private:
    std::chrono::steady_clock::time_point now_ {std::chrono::steady_clock::now()};
  };

  class FakeDisplaySettings final : public display_helper::v2::IDisplaySettings {
  public:
    display_helper::v2::ApplyStatus apply(const display_device::SingleDisplayConfiguration &) override {
      return apply_status;
    }

    display_helper::v2::ApplyStatus apply_topology(const display_device::ActiveTopology &) override {
      return apply_topology_status;
    }

    display_device::EnumeratedDeviceList enumerate(display_device::DeviceEnumerationDetail) override {
      return enumerated_devices;
    }

    display_device::ActiveTopology capture_topology() override {
      return topology;
    }

    bool validate_topology(const display_device::ActiveTopology &) override {
      return validate_topology_result;
    }

    display_device::DisplaySettingsSnapshot capture_snapshot() override {
      return snapshot;
    }

    bool apply_snapshot(const display_device::DisplaySettingsSnapshot &) override {
      return apply_snapshot_result;
    }

    bool snapshot_matches_current(const display_device::DisplaySettingsSnapshot &) override {
      return snapshot_matches_result;
    }

    bool configuration_matches(const display_device::SingleDisplayConfiguration &) override {
      return configuration_matches_result;
    }

    bool set_display_origin(const std::string &, const display_device::Point &) override {
      return set_display_origin_result;
    }

    std::optional<display_device::ActiveTopology> compute_expected_topology(
      const display_device::SingleDisplayConfiguration &,
      const std::optional<display_device::ActiveTopology> &) override {
      return expected_topology;
    }

    bool is_topology_same(const display_device::ActiveTopology &, const display_device::ActiveTopology &) override {
      return topology_same_result;
    }

    display_helper::v2::ApplyStatus apply_status = display_helper::v2::ApplyStatus::Ok;
    display_helper::v2::ApplyStatus apply_topology_status = display_helper::v2::ApplyStatus::Ok;
    display_device::EnumeratedDeviceList enumerated_devices;
    display_device::ActiveTopology topology;
    bool validate_topology_result = true;
    display_device::DisplaySettingsSnapshot snapshot;
    bool apply_snapshot_result = true;
    bool snapshot_matches_result = true;
    bool configuration_matches_result = true;
    bool set_display_origin_result = true;
    std::optional<display_device::ActiveTopology> expected_topology;
    bool topology_same_result = true;
  };

  display_device::DisplaySettingsSnapshot make_snapshot(const std::vector<std::string> &ids) {
    display_device::DisplaySettingsSnapshot snapshot;
    if (!ids.empty()) {
      snapshot.m_topology.push_back(ids);
    }
    for (const auto &id : ids) {
      snapshot.m_modes[id] = display_device::DisplayMode {};
      snapshot.m_hdr_states[id] = std::nullopt;
    }
    return snapshot;
  }

  struct TempDir {
    std::filesystem::path path;

    TempDir() {
      std::error_code ec;
      const auto base = std::filesystem::temp_directory_path(ec);
      const auto token = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
      path = (ec ? std::filesystem::path(".") : base) / ("sunshine_display_helper_v2_test_" + token);
      std::filesystem::create_directories(path, ec);
    }

    ~TempDir() {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  };
}  // namespace

TEST(DisplayHelperV2Queue, PushPopOrder) {
  display_helper::v2::MessageQueue<int> queue;
  queue.push(1);
  queue.push(2);
  queue.push(3);

  auto first = queue.try_pop();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(*first, 1);

  auto second = queue.try_pop();
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(*second, 2);

  auto third = queue.try_pop();
  ASSERT_TRUE(third.has_value());
  EXPECT_EQ(*third, 3);
}

TEST(DisplayHelperV2Queue, WaitPopBlocksUntilValue) {
  display_helper::v2::MessageQueue<int> queue;
  auto future = std::async(std::launch::async, [&queue]() {
    return queue.wait_pop();
  });

  EXPECT_EQ(future.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
  queue.push(42);
  EXPECT_EQ(future.wait_for(std::chrono::milliseconds(200)), std::future_status::ready);
  EXPECT_EQ(future.get(), 42);
}

TEST(DisplayHelperV2Queue, WaitForTimesOut) {
  display_helper::v2::MessageQueue<int> queue;
  auto value = queue.wait_for(std::chrono::milliseconds(10));
  EXPECT_FALSE(value.has_value());
}

TEST(DisplayHelperV2Cancellation, CancelInvalidatesToken) {
  display_helper::v2::CancellationSource source;
  auto token = source.token();
  EXPECT_FALSE(token.is_cancelled());

  source.cancel();
  EXPECT_TRUE(token.is_cancelled());

  auto token2 = source.token();
  EXPECT_FALSE(token2.is_cancelled());
}

TEST(DisplayHelperV2DisconnectGrace, TriggersAfterGrace) {
  FakeClock clock;
  display_helper::v2::DisconnectGrace grace(clock, std::chrono::seconds(30));

  grace.on_disconnect();
  EXPECT_FALSE(grace.should_trigger());

  clock.advance(std::chrono::seconds(29));
  EXPECT_FALSE(grace.should_trigger());

  clock.advance(std::chrono::seconds(1));
  EXPECT_TRUE(grace.should_trigger());
  EXPECT_FALSE(grace.should_trigger());
}

TEST(DisplayHelperV2DisconnectGrace, ReconnectCancelsPendingTrigger) {
  FakeClock clock;
  display_helper::v2::DisconnectGrace grace(clock, std::chrono::seconds(30));

  grace.on_disconnect();
  clock.advance(std::chrono::seconds(10));
  grace.on_reconnect();

  clock.advance(std::chrono::seconds(40));
  EXPECT_FALSE(grace.should_trigger());
}

TEST(DisplayHelperV2DisconnectGrace, SubsequentDisconnectResetsTimer) {
  FakeClock clock;
  display_helper::v2::DisconnectGrace grace(clock, std::chrono::seconds(30));

  grace.on_disconnect();
  clock.advance(std::chrono::seconds(20));
  grace.on_reconnect();

  grace.on_disconnect();
  clock.advance(std::chrono::seconds(29));
  EXPECT_FALSE(grace.should_trigger());

  clock.advance(std::chrono::seconds(1));
  EXPECT_TRUE(grace.should_trigger());
}

TEST(DisplayHelperV2ReconnectController, TriggersRevertAfterGrace) {
  FakeClock clock;
  display_helper::v2::ReconnectController controller(clock, std::chrono::seconds(30));

  controller.update_connection(true);
  controller.update_connection(false);

  clock.advance(std::chrono::seconds(29));
  EXPECT_FALSE(controller.update_connection(false));

  clock.advance(std::chrono::seconds(1));
  EXPECT_TRUE(controller.update_connection(false));
}

TEST(DisplayHelperV2ReconnectController, NoRevertBeforeGraceWindow) {
  FakeClock clock;
  display_helper::v2::ReconnectController controller(clock, std::chrono::seconds(30));

  controller.update_connection(true);
  controller.update_connection(false);

  clock.advance(std::chrono::seconds(15));
  EXPECT_FALSE(controller.update_connection(false));
  EXPECT_FALSE(controller.should_restart_pipe());
}

TEST(DisplayHelperV2ReconnectController, ReconnectWithinGraceDefersRevert) {
  FakeClock clock;
  display_helper::v2::ReconnectController controller(clock, std::chrono::seconds(30));

  controller.update_connection(true);
  controller.update_connection(false);

  clock.advance(std::chrono::seconds(10));
  controller.update_connection(true);

  clock.advance(std::chrono::seconds(40));
  EXPECT_FALSE(controller.update_connection(false));

  clock.advance(std::chrono::seconds(30));
  EXPECT_TRUE(controller.update_connection(false));
}

TEST(DisplayHelperV2ReconnectController, ReconnectDoesNotRestartHelper) {
  FakeClock clock;
  display_helper::v2::ReconnectController controller(clock, std::chrono::seconds(30));

  controller.update_connection(true);
  controller.update_connection(false);

  clock.advance(std::chrono::seconds(5));
  controller.update_connection(true);

  EXPECT_FALSE(controller.should_restart_pipe());
}

TEST(DisplayHelperV2ReconnectController, BrokenPipeRequestsRestart) {
  FakeClock clock;
  display_helper::v2::ReconnectController controller(clock, std::chrono::seconds(30));

  controller.on_broken();
  EXPECT_TRUE(controller.should_restart_pipe());
  EXPECT_FALSE(controller.update_connection(false));
}

TEST(DisplayHelperV2ApplyPolicy, RespectsVirtualDisplayCooldown) {
  FakeClock clock;
  display_helper::v2::ApplyPolicy policy(clock);

  EXPECT_EQ(
    policy.maybe_reset_virtual_display(display_helper::v2::ApplyStatus::NeedsVirtualDisplayReset, true),
    display_helper::v2::PolicyDecision::ResetVirtualDisplay);

  EXPECT_EQ(
    policy.maybe_reset_virtual_display(display_helper::v2::ApplyStatus::NeedsVirtualDisplayReset, true),
    display_helper::v2::PolicyDecision::Proceed);

  clock.advance(std::chrono::seconds(31));
  EXPECT_EQ(
    policy.maybe_reset_virtual_display(display_helper::v2::ApplyStatus::NeedsVirtualDisplayReset, true),
    display_helper::v2::PolicyDecision::ResetVirtualDisplay);
}

TEST(DisplayHelperV2ApplyPolicy, RetryDelayIsConstant) {
  FakeClock clock;
  display_helper::v2::ApplyPolicy policy(clock);
  EXPECT_EQ(policy.retry_delay(1), std::chrono::milliseconds(300));
  EXPECT_EQ(policy.retry_delay(2), std::chrono::milliseconds(300));
}

TEST(DisplayHelperV2ApplyPolicy, SkipTierOnFatal) {
  FakeClock clock;
  display_helper::v2::ApplyPolicy policy(clock);
  EXPECT_TRUE(policy.should_skip_tier(display_helper::v2::ApplyStatus::InvalidRequest));
  EXPECT_TRUE(policy.should_skip_tier(display_helper::v2::ApplyStatus::Fatal));
  EXPECT_FALSE(policy.should_skip_tier(display_helper::v2::ApplyStatus::Retryable));
}

TEST(DisplayHelperV2ApplyOperation, HonorsExplicitTopologyForExpected) {
  FakeDisplaySettings display;
  display.expected_topology = display_device::ActiveTopology {{"A"}};

  display_helper::v2::ApplyOperation operation(display);
  display_helper::v2::ApplyRequest request;
  display_device::SingleDisplayConfiguration config;
  config.m_device_id = "A";
  config.m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::EnsureOnlyDisplay;
  request.configuration = config;
  request.topology = display_device::ActiveTopology {{"A"}, {"B"}};

  display_helper::v2::CancellationSource source;
  auto outcome = operation.run(request, source.token());

  ASSERT_TRUE(outcome.expected_topology.has_value());
  EXPECT_EQ(outcome.expected_topology.value(), *request.topology);
}

TEST(DisplayHelperV2SnapshotPersistence, SaveFiltersBlacklistedDevices) {
  display_helper::v2::InMemorySnapshotStorage storage;
  display_helper::v2::SnapshotPersistence persistence(storage);

  auto snapshot = make_snapshot({"A", "B"});
  std::set<std::string> blacklist {"B"};

  EXPECT_TRUE(persistence.save(display_helper::v2::SnapshotTier::Current, snapshot, blacklist));

  auto loaded = storage.load(display_helper::v2::SnapshotTier::Current);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->m_topology.size(), 1u);
  EXPECT_EQ(loaded->m_topology.front().size(), 1u);
  EXPECT_EQ(loaded->m_topology.front().front(), "A");
  EXPECT_EQ(loaded->m_modes.count("B"), 0u);
}

TEST(DisplayHelperV2SnapshotPersistence, SaveRejectsAllBlacklisted) {
  display_helper::v2::InMemorySnapshotStorage storage;
  display_helper::v2::SnapshotPersistence persistence(storage);

  auto snapshot = make_snapshot({"B"});
  std::set<std::string> blacklist {"B"};

  EXPECT_FALSE(persistence.save(display_helper::v2::SnapshotTier::Current, snapshot, blacklist));
}

TEST(DisplayHelperV2SnapshotPersistence, LoadRejectsMissingDevices) {
  display_helper::v2::InMemorySnapshotStorage storage;
  display_helper::v2::SnapshotPersistence persistence(storage);

  auto snapshot = make_snapshot({"A"});
  EXPECT_TRUE(storage.save(display_helper::v2::SnapshotTier::Current, snapshot));

  std::set<std::string> available {"B"};
  auto loaded = persistence.load(display_helper::v2::SnapshotTier::Current, available);
  EXPECT_FALSE(loaded.has_value());
}

TEST(DisplayHelperV2SnapshotPersistence, RecoveryOrderRespectsGoldenPreference) {
  display_helper::v2::InMemorySnapshotStorage storage;
  display_helper::v2::SnapshotPersistence persistence(storage);

  auto order = persistence.recovery_order();
  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], display_helper::v2::SnapshotTier::Current);
  EXPECT_EQ(order[1], display_helper::v2::SnapshotTier::Previous);
  EXPECT_EQ(order[2], display_helper::v2::SnapshotTier::Golden);

  persistence.set_prefer_golden_first(true);
  order = persistence.recovery_order();
  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], display_helper::v2::SnapshotTier::Golden);
  EXPECT_EQ(order[1], display_helper::v2::SnapshotTier::Current);
  EXPECT_EQ(order[2], display_helper::v2::SnapshotTier::Previous);
}

TEST(DisplayHelperV2SnapshotPersistence, RotateCopiesCurrentToPrevious) {
  display_helper::v2::InMemorySnapshotStorage storage;
  display_helper::v2::SnapshotPersistence persistence(storage);

  auto snapshot = make_snapshot({"A"});
  EXPECT_TRUE(storage.save(display_helper::v2::SnapshotTier::Current, snapshot));
  EXPECT_TRUE(persistence.rotate_current_to_previous());

  auto previous = storage.load(display_helper::v2::SnapshotTier::Previous);
  ASSERT_TRUE(previous.has_value());
  EXPECT_EQ(previous->m_topology.front().front(), "A");
}

TEST(DisplayHelperV2SnapshotService, CaptureReturnsSnapshot) {
  FakeDisplaySettings display;
  display.snapshot = make_snapshot({"A"});

  display_helper::v2::SnapshotService service(display);
  auto captured = service.capture();
  EXPECT_EQ(captured.m_topology, display.snapshot.m_topology);
}

TEST(DisplayHelperV2SnapshotService, ApplyRejectsInvalidTopology) {
  FakeDisplaySettings display;
  display.validate_topology_result = false;

  display_helper::v2::SnapshotService service(display);
  display_helper::v2::CancellationSource source;
  auto status = service.apply(display.snapshot, source.token());
  EXPECT_EQ(status, display_helper::v2::ApplyStatus::InvalidRequest);
}

TEST(DisplayHelperV2SnapshotService, ApplyReturnsRetryableOnFailure) {
  FakeDisplaySettings display;
  display.apply_snapshot_result = false;

  display_helper::v2::SnapshotService service(display);
  display_helper::v2::CancellationSource source;
  auto status = service.apply(display.snapshot, source.token());
  EXPECT_EQ(status, display_helper::v2::ApplyStatus::Retryable);
}

TEST(DisplayHelperV2SnapshotService, ApplyReturnsOkOnSuccess) {
  FakeDisplaySettings display;

  display_helper::v2::SnapshotService service(display);
  display_helper::v2::CancellationSource source;
  auto status = service.apply(display.snapshot, source.token());
  EXPECT_EQ(status, display_helper::v2::ApplyStatus::Ok);
}

TEST(DisplayHelperV2SnapshotService, ApplyReturnsFatalWhenCancelled) {
  FakeDisplaySettings display;
  display_helper::v2::SnapshotService service(display);
  display_helper::v2::CancellationSource source;
  auto token = source.token();
  source.cancel();

  auto status = service.apply(display.snapshot, token);
  EXPECT_EQ(status, display_helper::v2::ApplyStatus::Fatal);
}

TEST(DisplayHelperV2SnapshotService, MatchesCurrentUsesDisplayBackend) {
  FakeDisplaySettings display;
  display.snapshot_matches_result = false;

  display_helper::v2::SnapshotService service(display);
  EXPECT_FALSE(service.matches_current(display.snapshot));
}

TEST(DisplayHelperV2FileSnapshotStorage, SaveLoadRoundTrip) {
  TempDir temp;
  display_helper::v2::SnapshotPaths paths {
    temp.path / "current.json",
    temp.path / "previous.json",
    temp.path / "golden.json"
  };
  display_helper::v2::FileSnapshotStorage storage(paths);

  display_device::DisplaySettingsSnapshot snapshot;
  snapshot.m_topology = {{"A", "B"}};
  snapshot.m_modes["A"] = display_device::DisplayMode {};
  snapshot.m_modes["B"] = display_device::DisplayMode {};
  snapshot.m_hdr_states["A"] = display_device::HdrState::Enabled;
  snapshot.m_hdr_states["B"] = std::nullopt;
  snapshot.m_primary_device = "A";

  EXPECT_TRUE(storage.save(display_helper::v2::SnapshotTier::Current, snapshot));

  auto loaded = storage.load(display_helper::v2::SnapshotTier::Current);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(*loaded, snapshot);
}

TEST(DisplayHelperV2FileSnapshotStorage, ReportsMissingDevices) {
  TempDir temp;
  display_helper::v2::SnapshotPaths paths {
    temp.path / "current.json",
    temp.path / "previous.json",
    temp.path / "golden.json"
  };
  display_helper::v2::FileSnapshotStorage storage(paths);

  display_device::DisplaySettingsSnapshot snapshot;
  snapshot.m_topology = {{"A", "B"}};
  snapshot.m_modes["A"] = display_device::DisplayMode {};
  snapshot.m_modes["B"] = display_device::DisplayMode {};
  snapshot.m_hdr_states["A"] = std::nullopt;
  snapshot.m_hdr_states["B"] = std::nullopt;

  std::set<std::string> available {"A"};
  auto missing = storage.missing_devices(snapshot, available);

  ASSERT_EQ(missing.size(), 1u);
  EXPECT_EQ(missing.front(), "B");
}
