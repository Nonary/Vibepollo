/**
 * @file tests/unit/test_display_helper_v2_async_dispatcher.cpp
 * @brief Unit tests for display helper v2 async dispatcher.
 */
#include "../tests_common.h"

#include "src/platform/windows/display_helper_v2/async_dispatcher.h"
#include "src/platform/windows/display_helper_v2/operations.h"
#include "src/platform/windows/display_helper_v2/snapshot.h"

#include <future>
#include <mutex>

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
    display_helper::v2::ApplyStatus apply(const display_device::SingleDisplayConfiguration &) override {
      apply_calls += 1;
      return apply_status;
    }

    display_helper::v2::ApplyStatus apply_topology(const display_device::ActiveTopology &) override {
      return display_helper::v2::ApplyStatus::Ok;
    }

    display_device::EnumeratedDeviceList enumerate(display_device::DeviceEnumerationDetail) override {
      return {};
    }

    display_device::ActiveTopology capture_topology() override {
      return {};
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
      return std::nullopt;
    }

    bool is_topology_same(const display_device::ActiveTopology &, const display_device::ActiveTopology &) override {
      return true;
    }

    display_helper::v2::ApplyStatus apply_status = display_helper::v2::ApplyStatus::Ok;
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
  display_helper::v2::SnapshotPersistence persistence(storage);
  display_helper::v2::ApplyPolicy policy(clock);
  display_helper::v2::ApplyOperation apply_op(display);
  display_helper::v2::VerificationOperation verify_op(display, clock);
  display_helper::v2::RecoveryOperation recovery_op(display, snapshot_service, persistence, policy, clock);
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
  ASSERT_EQ(clock.sleeps.size(), 3u);
  EXPECT_EQ(clock.sleeps[0], std::chrono::milliseconds(100));
  EXPECT_EQ(clock.sleeps[1], std::chrono::milliseconds(500));
  EXPECT_EQ(clock.sleeps[2], std::chrono::milliseconds(1000));
}

TEST(DisplayHelperV2AsyncDispatcher, FailsWhenVirtualDisplayDisableFails) {
  FakeClock clock;
  FakeDisplaySettings display;
  display_helper::v2::SnapshotService snapshot_service(display);
  display_helper::v2::InMemorySnapshotStorage storage;
  display_helper::v2::SnapshotPersistence persistence(storage);
  display_helper::v2::ApplyPolicy policy(clock);
  display_helper::v2::ApplyOperation apply_op(display);
  display_helper::v2::VerificationOperation verify_op(display, clock);
  display_helper::v2::RecoveryOperation recovery_op(display, snapshot_service, persistence, policy, clock);
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
  ASSERT_EQ(clock.sleeps.size(), 1u);
  EXPECT_EQ(clock.sleeps[0], std::chrono::milliseconds(50));
}
