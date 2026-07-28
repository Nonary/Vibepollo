/**
 * @file tests/unit/test_display_helper_v2_async_dispatcher.cpp
 * @brief Unit tests for display helper v2 async dispatcher.
 */
#ifdef _WIN32

#include "../tests_common.h"

#include "src/platform/windows/display_helper_v2/async_dispatcher.h"
#include "src/platform/windows/display_helper_v2/golden_health.h"
#include "src/platform/windows/display_helper_v2/operations.h"
#include "src/platform/windows/display_helper_v2/snapshot.h"

#include <future>
#include <mutex>
#include <numeric>

namespace {
  class FakeClock final : public display_helper::v2::IClock {
  public:
    std::chrono::steady_clock::time_point now() override {
      std::lock_guard<std::mutex> lock(mutex_);
      return now_;
    }

    void sleep_for(std::chrono::milliseconds duration) override {
      std::lock_guard<std::mutex> lock(mutex_);
      sleeps.push_back(duration);
      now_ += duration;
    }

    std::vector<std::chrono::milliseconds> sleeps;

  private:
    std::mutex mutex_;
    std::chrono::steady_clock::time_point now_ {std::chrono::steady_clock::now()};
  };

  class FakeDisplaySettings final : public display_helper::v2::IDisplaySettings {
  public:
    FakeDisplaySettings() {
      display_device::EnumeratedDevice device;
      device.m_device_id = "A";
      device.m_display_name = "\\\\.\\DISPLAY1";
      device.m_info = display_device::EnumeratedDevice::Info {};
      devices.push_back(std::move(device));
    }

    display_helper::v2::ApplyStatus apply(const display_device::SingleDisplayConfiguration &) override {
      apply_calls += 1;
      return apply_status;
    }

    display_helper::v2::ApplyStatus apply_topology(const display_device::ActiveTopology &) override {
      return display_helper::v2::ApplyStatus::Ok;
    }

    display_device::EnumeratedDeviceList enumerate(display_device::DeviceEnumerationDetail) override {
      return devices;
    }

    display_device::ActiveTopology capture_topology() override {
      return topology;
    }

    bool validate_topology(const display_device::ActiveTopology &) override {
      return true;
    }

    display_device::DisplaySettingsSnapshot capture_snapshot() override {
      return {};
    }

    bool apply_snapshot(const display_device::DisplaySettingsSnapshot &) override {
      return true;
    }

    bool snapshot_matches_current(const display_device::DisplaySettingsSnapshot &) override {
      return true;
    }

    bool configuration_matches(const display_device::SingleDisplayConfiguration &) override {
      return true;
    }

    bool set_display_origin(const std::string &, const display_device::Point &) override {
      return true;
    }

    std::optional<display_device::ActiveTopology> compute_expected_topology(
      const display_device::SingleDisplayConfiguration &,
      const std::optional<display_device::ActiveTopology> &) override {
      return topology;
    }

    bool is_topology_same(const display_device::ActiveTopology &lhs, const display_device::ActiveTopology &rhs) override {
      return lhs == rhs;
    }

    display_helper::v2::ApplyStatus apply_status = display_helper::v2::ApplyStatus::Ok;
    display_device::ActiveTopology topology {{"A"}};
    display_device::EnumeratedDeviceList devices;
    int apply_calls = 0;
  };

  class FakeVirtualDisplayDriver final : public display_helper::v2::IVirtualDisplayDriver {
  public:
    bool disable() override {
      disable_calls += 1;
      return disable_result;
    }

    bool enable() override {
      enable_calls += 1;
      return enable_result;
    }

    bool is_available() override {
      return true;
    }

    std::string device_id() override {
      return {};
    }

    bool disable_result = true;
    bool enable_result = true;
    int disable_calls = 0;
    int enable_calls = 0;
  };
}  // namespace

TEST(DisplayHelperV2AsyncDispatcher, AppliesAfterVirtualDisplayResetSequence) {
  FakeClock clock;
  FakeDisplaySettings display;
  display_helper::v2::SnapshotService snapshot_service(display);
  display_helper::v2::InMemorySnapshotStorage storage;
  display_helper::v2::GoldenHealth golden_health({});
  display_helper::v2::RestoreState restore_state;
  int recovery_boundary_calls = 0;
  display_helper::v2::ApplyOperation apply_op(display, clock, [&] {
    ++recovery_boundary_calls;
    return true;
  });
  display_helper::v2::VerificationOperation verify_op(display, clock);
  display_helper::v2::RecoveryOperation recovery_op(display, storage, golden_health, restore_state, clock);
  display_helper::v2::RecoveryValidationOperation recovery_validate(snapshot_service, clock);
  FakeVirtualDisplayDriver virtual_display;

  display_helper::v2::AsyncDispatcher dispatcher(
    apply_op,
    verify_op,
    recovery_op,
    recovery_validate,
    virtual_display,
    clock
  );

  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  request.virtual_layout = "extended";
  display_helper::v2::CancellationSource cancel;

  std::promise<display_helper::v2::ApplyOutcome> promise;
  dispatcher.dispatch_apply(
    request,
    cancel.token(),
    std::chrono::milliseconds(100),
    true,
    [&](const display_helper::v2::ApplyOutcome &outcome) {
      promise.set_value(outcome);
    }
  );

  auto future = promise.get_future();
  ASSERT_EQ(future.wait_for(std::chrono::milliseconds(500)), std::future_status::ready);
  auto outcome = future.get();

  EXPECT_EQ(outcome.status, display_helper::v2::ApplyStatus::Ok);
  EXPECT_EQ(display.apply_calls, 1);
  EXPECT_EQ(virtual_display.disable_calls, 1);
  EXPECT_EQ(virtual_display.enable_calls, 1);
  EXPECT_TRUE(outcome.virtual_display_requested);
  EXPECT_TRUE(outcome.display_may_have_changed);
  EXPECT_TRUE(outcome.durable_recovery_armed);
  EXPECT_EQ(recovery_boundary_calls, 1);
  ASSERT_FALSE(clock.sleeps.empty());
  const auto total_sleep = std::accumulate(
    clock.sleeps.begin(),
    clock.sleeps.end(),
    std::chrono::milliseconds::zero());
  EXPECT_EQ(total_sleep, std::chrono::milliseconds(1600));
}

TEST(DisplayHelperV2AsyncDispatcher, FailsWhenVirtualDisplayDisableFails) {
  FakeClock clock;
  FakeDisplaySettings display;
  display_helper::v2::SnapshotService snapshot_service(display);
  display_helper::v2::InMemorySnapshotStorage storage;
  display_helper::v2::GoldenHealth golden_health({});
  display_helper::v2::RestoreState restore_state;
  int recovery_boundary_calls = 0;
  display_helper::v2::ApplyOperation apply_op(display, clock, [&] {
    ++recovery_boundary_calls;
    return true;
  });
  display_helper::v2::VerificationOperation verify_op(display, clock);
  display_helper::v2::RecoveryOperation recovery_op(display, storage, golden_health, restore_state, clock);
  display_helper::v2::RecoveryValidationOperation recovery_validate(snapshot_service, clock);
  FakeVirtualDisplayDriver virtual_display;
  virtual_display.disable_result = false;

  display_helper::v2::AsyncDispatcher dispatcher(
    apply_op,
    verify_op,
    recovery_op,
    recovery_validate,
    virtual_display,
    clock
  );

  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  request.virtual_layout = "extended";
  display_helper::v2::CancellationSource cancel;

  std::promise<display_helper::v2::ApplyOutcome> promise;
  dispatcher.dispatch_apply(
    request,
    cancel.token(),
    std::chrono::milliseconds(50),
    true,
    [&](const display_helper::v2::ApplyOutcome &outcome) {
      promise.set_value(outcome);
    }
  );

  auto future = promise.get_future();
  ASSERT_EQ(future.wait_for(std::chrono::milliseconds(500)), std::future_status::ready);
  auto outcome = future.get();

  EXPECT_EQ(outcome.status, display_helper::v2::ApplyStatus::Fatal);
  EXPECT_EQ(display.apply_calls, 0);
  EXPECT_EQ(virtual_display.disable_calls, 1);
  EXPECT_EQ(virtual_display.enable_calls, 0);
  EXPECT_TRUE(outcome.virtual_display_requested);
  EXPECT_TRUE(outcome.display_may_have_changed);
  EXPECT_TRUE(outcome.durable_recovery_armed);
  EXPECT_EQ(recovery_boundary_calls, 1);
  ASSERT_EQ(clock.sleeps.size(), 1u);
  EXPECT_EQ(clock.sleeps[0], std::chrono::milliseconds(50));
}

#endif  // _WIN32
