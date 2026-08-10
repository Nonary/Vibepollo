/**
 * @file tests/unit/test_display_helper_v2_state_machine.cpp
 * @brief Unit tests for display helper v2 state machine and operations.
 */
#ifdef _WIN32

#include "../tests_common.h"

#include "src/platform/windows/display_helper_v2/async_dispatcher.h"
#include "src/platform/windows/display_helper_v2/operations.h"
#include "src/platform/windows/display_helper_v2/runtime_support.h"
#include "src/platform/windows/display_helper_v2/snapshot.h"
#include "src/platform/windows/display_helper_v2/state_machine.h"
#include "src/platform/windows/display_helper_v2/topology_policy.h"

#include <array>
#include <deque>
#include <map>
#include <set>

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

  class FakeDispatcher final : public display_helper::v2::IAsyncDispatcher {
  public:
    void dispatch_apply(
      const display_helper::v2::ApplyRequest &request,
      const display_helper::v2::CancellationToken &,
      std::chrono::milliseconds delay,
      bool reset_virtual_display,
      std::function<void(const display_helper::v2::ApplyOutcome &)> completion) override {
      apply_request = request;
      apply_delay = delay;
      apply_reset_virtual_display = reset_virtual_display;
      apply_completion = std::move(completion);
      apply_dispatch_count += 1;
    }

    void dispatch_verification(
      const display_helper::v2::ApplyRequest &request,
      const std::optional<display_helper::v2::ActiveTopology> &expected_topology,
      const std::optional<display_helper::v2::ResolvedConfigurationTarget> &resolved_target,
      const display_helper::v2::CancellationToken &,
      std::function<void(bool)> completion) override {
      verification_request = request;
      verification_topology = expected_topology;
      verification_target = resolved_target;
      verification_completion = std::move(completion);
      verification_dispatch_count += 1;
    }

    void dispatch_verification_after(
      const display_helper::v2::ApplyRequest &request,
      const std::optional<display_helper::v2::ActiveTopology> &expected_topology,
      const std::optional<display_helper::v2::ResolvedConfigurationTarget> &resolved_target,
      const display_helper::v2::CancellationToken &token,
      std::chrono::milliseconds delay,
      std::function<void(bool)> completion) override {
      verification_delay = delay;
      dispatch_verification(request, expected_topology, resolved_target, token, std::move(completion));
    }

    void dispatch_recovery(
      const display_helper::v2::CancellationToken &,
      std::chrono::milliseconds delay,
      std::function<void(const display_helper::v2::RecoveryOutcome &)> completion) override {
      recovery_delay = delay;
      recovery_completion = std::move(completion);
      recovery_dispatch_count += 1;
    }

    void dispatch_recovery_validation(
      const display_helper::v2::Snapshot &snapshot,
      const display_helper::v2::CancellationToken &,
      std::function<void(bool)> completion) override {
      recovery_validation_snapshot = snapshot;
      recovery_validation_completion = std::move(completion);
      recovery_validation_dispatch_count += 1;
    }

    void dispatch_reset_staged_apply_state(
      std::function<void(bool)> completion) override {
      reset_completion = std::move(completion);
      reset_dispatch_count += 1;
    }

    void dispatch_refresh_rate(
      std::string device_id,
      unsigned int numerator,
      unsigned int denominator,
      const display_helper::v2::CancellationToken &,
      std::function<void(bool)> completion) override {
      refresh_device_id = std::move(device_id);
      refresh_numerator = numerator;
      refresh_denominator = denominator;
      refresh_completion = std::move(completion);
      refresh_dispatch_count += 1;
    }

    display_helper::v2::ApplyRequest apply_request;
    std::chrono::milliseconds apply_delay {0};
    bool apply_reset_virtual_display = false;
    int apply_dispatch_count = 0;
    std::function<void(const display_helper::v2::ApplyOutcome &)> apply_completion;

    display_helper::v2::ApplyRequest verification_request;
    std::optional<display_helper::v2::ActiveTopology> verification_topology;
    std::optional<display_helper::v2::ResolvedConfigurationTarget> verification_target;
    std::chrono::milliseconds verification_delay {0};
    int verification_dispatch_count = 0;
    std::function<void(bool)> verification_completion;

    int recovery_dispatch_count = 0;
    std::chrono::milliseconds recovery_delay {0};
    std::function<void(const display_helper::v2::RecoveryOutcome &)> recovery_completion;

    display_helper::v2::Snapshot recovery_validation_snapshot;
    int recovery_validation_dispatch_count = 0;
    std::function<void(bool)> recovery_validation_completion;

    int reset_dispatch_count = 0;
    std::function<void(bool)> reset_completion;

    int refresh_dispatch_count = 0;
    std::string refresh_device_id;
    unsigned int refresh_numerator = 0;
    unsigned int refresh_denominator = 0;
    std::function<void(bool)> refresh_completion;
  };

  class FakeTaskManager final : public display_helper::v2::IScheduledTaskManager {
  public:
    bool create_restore_task(const std::wstring &) override {
      created += 1;
      return true;
    }

    bool delete_restore_task() override {
      deleted += 1;
      return true;
    }

    bool is_task_present() override {
      return created > deleted;
    }

    int created = 0;
    int deleted = 0;
  };

  class FakeWorkarounds final : public display_helper::v2::IPlatformWorkarounds {
  public:
    void blank_hdr_states(std::chrono::milliseconds delay) override {
      blank_calls += 1;
      last_delay = delay;
    }

    void refresh_shell() override {
      refresh_calls += 1;
    }

    int blank_calls = 0;
    int refresh_calls = 0;
    std::chrono::milliseconds last_delay {0};
  };

  class FakeVirtualDisplayDriver final : public display_helper::v2::IVirtualDisplayDriver {
  public:
    bool disable() override {
      disabled = true;
      return true;
    }

    bool enable() override {
      enabled = true;
      return true;
    }

    bool is_available() override {
      return available;
    }

    std::string device_id() override {
      return current_device_id;
    }

    bool available = true;
    bool disabled = false;
    bool enabled = false;
    std::string current_device_id {"virtual"};
  };

  class FakeDisplaySettings final : public display_helper::v2::IDisplaySettings {
  public:
    display_helper::v2::ApplyStatus apply(const display_device::SingleDisplayConfiguration &) override {
      return apply_status;
    }

    display_helper::v2::ApplyStatus apply_topology(const display_device::ActiveTopology &) override {
      return topology_status;
    }

    display_device::EnumeratedDeviceList enumerate(display_device::DeviceEnumerationDetail) override {
      return devices;
    }

    display_device::ActiveTopology capture_topology() override {
      return topology;
    }

    bool validate_topology(const display_device::ActiveTopology &topology_input) override {
      return valid_topology_ids.count(extract_id(topology_input)) > 0;
    }

    display_device::DisplaySettingsSnapshot capture_snapshot() override {
      return snapshot;
    }

    bool apply_snapshot(const display_device::DisplaySettingsSnapshot &snapshot_input) override {
      apply_snapshot_calls += 1;
      return apply_snapshot_ids.count(extract_id(snapshot_input.m_topology)) > 0;
    }

    bool snapshot_matches_current(const display_device::DisplaySettingsSnapshot &snapshot_input) override {
      const auto id = extract_id(snapshot_input.m_topology);
      auto &calls = match_calls[id];
      if (calls < static_cast<int>(match_sequence[id].size())) {
        return match_sequence[id][calls++];
      }
      return false;
    }

    bool configuration_matches(const display_device::SingleDisplayConfiguration &) override {
      return configuration_matches_result;
    }

    bool set_display_origin(const std::string &, const display_device::Point &) override {
      return true;
    }

    std::optional<display_device::ActiveTopology> compute_expected_topology(
      const display_device::SingleDisplayConfiguration &,
      const std::optional<display_device::ActiveTopology> &) override {
      return expected_topology;
    }

    bool is_topology_same(const display_device::ActiveTopology &, const display_device::ActiveTopology &) override {
      return topology_same_result;
    }

    static std::string extract_id(const display_device::ActiveTopology &topology_input) {
      if (!topology_input.empty() && !topology_input.front().empty()) {
        return topology_input.front().front();
      }
      return {};
    }

    display_helper::v2::ApplyStatus apply_status = display_helper::v2::ApplyStatus::Ok;
    display_helper::v2::ApplyStatus topology_status = display_helper::v2::ApplyStatus::Ok;
    display_device::EnumeratedDeviceList devices;
    display_device::ActiveTopology topology;
    std::set<std::string> valid_topology_ids;
    std::set<std::string> apply_snapshot_ids;
    std::map<std::string, std::vector<bool>> match_sequence;
    std::map<std::string, int> match_calls;
    display_device::DisplaySettingsSnapshot snapshot;
    bool configuration_matches_result = true;
    std::optional<display_device::ActiveTopology> expected_topology;
    bool topology_same_result = true;
    int apply_snapshot_calls = 0;
  };

  display_device::DisplaySettingsSnapshot make_snapshot(const std::string &id) {
    display_device::DisplaySettingsSnapshot snapshot;
    snapshot.m_topology.push_back({id});
    snapshot.m_modes[id] = display_device::DisplayMode {};
    snapshot.m_hdr_states[id] = std::nullopt;
    return snapshot;
  }

  struct StateMachineHarness {
    FakeClock clock;
    display_helper::v2::ApplyPolicy policy {clock};
    FakeDispatcher dispatcher;
    FakeVirtualDisplayDriver virtual_display;
    FakeDisplaySettings display_settings;
    display_helper::v2::SnapshotService snapshot_service {display_settings};
    display_helper::v2::InMemorySnapshotStorage storage;
    display_helper::v2::SnapshotPersistence snapshot_persistence {storage};
    FakeWorkarounds workarounds;
    FakeTaskManager task_manager;
    display_helper::v2::HeartbeatMonitor heartbeat {clock};
    display_helper::v2::CancellationSource cancellation;
    display_helper::v2::InMemoryTextStorage golden_status_storage;
    display_helper::v2::GoldenHealth golden_health {golden_status_storage, {}};
    display_helper::v2::RestoreState restore_state;
    std::deque<display_helper::v2::Message> messages;
    std::optional<display_helper::v2::ApplyStatus> apply_result;
    int apply_result_count = 0;
    std::optional<bool> verification_result;
    int verification_result_count = 0;
    std::optional<bool> snapshot_result;
    std::optional<bool> refresh_result;
    std::optional<int> exit_code;

    display_helper::v2::SystemPorts system_ports {workarounds, task_manager, heartbeat, clock, cancellation};
    display_helper::v2::ApplyPipeline apply_pipeline {
      dispatcher,
      policy,
      system_ports,
      [this](display_helper::v2::Message message) { messages.push_back(std::move(message)); }
    };
    display_helper::v2::RecoveryPipeline recovery_pipeline {
      dispatcher,
      system_ports,
      [this](display_helper::v2::Message message) { messages.push_back(std::move(message)); }
    };
    display_helper::v2::SnapshotLedger snapshot_ledger {snapshot_service, snapshot_persistence, clock};

    display_helper::v2::StateMachine state_machine {
      apply_pipeline,
      recovery_pipeline,
      snapshot_ledger,
      system_ports,
      virtual_display,
      golden_health,
      restore_state
    };

    void seed_current_snapshot(const std::string &id = "seed") {
      display_device::DisplaySettingsSnapshot snapshot;
      snapshot.m_topology.push_back({id});
      snapshot.m_modes[id] = display_device::DisplayMode {};
      snapshot.m_hdr_states[id] = std::nullopt;
      ASSERT_TRUE(storage.save(display_helper::v2::SnapshotTier::Current, snapshot));
    }

    void add_active_device(const std::string &id) {
      display_device::EnumeratedDevice device;
      device.m_device_id = id;
      device.m_display_name = "\\\\.\\DISPLAY_" + id;
      device.m_friendly_name = "Fake Monitor";
      device.m_info = display_device::EnumeratedDevice::Info {};
      display_settings.devices.push_back(std::move(device));
    }

    StateMachineHarness() {
      state_machine.set_apply_result_callback([this](display_helper::v2::ApplyStatus status, std::uint64_t, std::uint64_t) {
        apply_result = status;
        ++apply_result_count;
      });
      state_machine.set_verification_result_callback([this](bool success, std::uint64_t, std::uint64_t) {
        verification_result = success;
        ++verification_result_count;
      });
      state_machine.set_snapshot_result_callback([this](bool success, std::uint64_t, std::uint64_t) {
        snapshot_result = success;
      });
      state_machine.set_refresh_rate_result_callback([this](bool success, std::uint64_t, std::uint64_t) {
        refresh_result = success;
      });
      state_machine.set_exit_callback([this](int code) {
        exit_code = code;
      });
    }

    void drain_messages() {
      while (!messages.empty()) {
        auto message = std::move(messages.front());
        messages.pop_front();
        state_machine.handle_message(message);
      }
    }
  };
}  // namespace

TEST(DisplayHelperV2Debounce, CoalescesUntilDelay) {
  display_helper::v2::DebouncedTrigger debouncer(std::chrono::milliseconds(500));
  auto start = std::chrono::steady_clock::now();

  debouncer.notify(start);
  EXPECT_FALSE(debouncer.should_fire(start));
  EXPECT_FALSE(debouncer.should_fire(start + std::chrono::milliseconds(250)));
  EXPECT_TRUE(debouncer.should_fire(start + std::chrono::milliseconds(500)));
  EXPECT_FALSE(debouncer.pending());
}

TEST(DisplayHelperV2Debounce, RetainsNotificationGenerationUntilDue) {
  display_helper::v2::DebouncedTrigger debouncer(std::chrono::milliseconds(500));
  const auto start = std::chrono::steady_clock::now();

  debouncer.notify(start, 42);
  EXPECT_FALSE(debouncer.take_if_due(start + std::chrono::milliseconds(499)).has_value());

  const auto generation = debouncer.take_if_due(start + std::chrono::milliseconds(500));
  ASSERT_TRUE(generation.has_value());
  EXPECT_EQ(*generation, 42u);
}

TEST(DisplayHelperV2Heartbeat, TriggersTimeoutWhenArmed) {
  FakeClock clock;
  display_helper::v2::HeartbeatMonitor monitor(clock);

  monitor.arm();
  clock.advance(std::chrono::seconds(31));
  // v1 waits through an optional start window, a missed-ping window, and a
  // reconnection grace window before it restores a display.
  EXPECT_FALSE(monitor.check_timeout());
  clock.advance(std::chrono::seconds(119));
  EXPECT_FALSE(monitor.check_timeout());
  clock.advance(std::chrono::seconds(1));
  EXPECT_TRUE(monitor.check_timeout());
  EXPECT_FALSE(monitor.check_timeout());
}

TEST(DisplayHelperV2Heartbeat, PingCancelsPendingRecoveryGrace) {
  FakeClock clock;
  display_helper::v2::HeartbeatMonitor monitor(clock);

  monitor.arm();
  clock.advance(std::chrono::seconds(31));
  EXPECT_FALSE(monitor.check_timeout());
  monitor.record_ping();
  clock.advance(std::chrono::minutes(3));
  EXPECT_FALSE(monitor.check_timeout());
}

TEST(DisplayHelperV2Heartbeat, IgnoresTimeoutWhenDisarmed) {
  FakeClock clock;
  display_helper::v2::HeartbeatMonitor monitor(clock);

  clock.advance(std::chrono::seconds(40));
  EXPECT_FALSE(monitor.check_timeout());
}

TEST(DisplayHelperV2StateMachine, ApplyTransitionsAndVerifies) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};

  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);
  EXPECT_EQ(harness.task_manager.created, 0);
  ASSERT_TRUE(harness.dispatcher.apply_completion);

  display_helper::v2::ApplyOutcome outcome;
  outcome.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(outcome);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Verification);
  EXPECT_EQ(harness.task_manager.created, 1);
  ASSERT_TRUE(harness.dispatcher.verification_completion);

  harness.dispatcher.verification_completion(true);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  ASSERT_TRUE(harness.verification_result.has_value());
  EXPECT_TRUE(harness.verification_result.value());
  EXPECT_TRUE(harness.state_machine.recovery_armed());
  EXPECT_EQ(harness.dispatcher.reset_dispatch_count, 0);
  EXPECT_EQ(harness.workarounds.refresh_calls, 1);
  EXPECT_EQ(harness.workarounds.blank_calls, 0);
  ASSERT_TRUE(harness.apply_result.has_value());
  EXPECT_EQ(harness.apply_result, display_helper::v2::ApplyStatus::Ok);
}

TEST(DisplayHelperV2StateMachine, IgnoresCommandsFromRetiredConnectionEpoch) {
  StateMachineHarness harness;
  std::uint64_t active_connection_epoch = 2;
  harness.state_machine.set_connection_epoch_provider([&] {
    return active_connection_epoch;
  });

  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {
    request,
    harness.cancellation.current_generation(),
    1,
  });

  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 0);
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);

  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {
    request,
    harness.cancellation.current_generation(),
    active_connection_epoch,
  });

  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 1);
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);
}

TEST(DisplayHelperV2StateMachine, IgnoresStaleDisconnectAndHeartbeatRecoverySignals) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();
  std::uint64_t active_connection_epoch = 1;
  harness.state_machine.set_connection_epoch_provider([&] {
    return active_connection_epoch;
  });

  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {
    request,
    harness.cancellation.current_generation(),
    active_connection_epoch,
  });
  display_helper::v2::ApplyOutcome applied;
  applied.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(applied);
  harness.drain_messages();
  ASSERT_TRUE(harness.dispatcher.verification_completion);
  harness.dispatcher.verification_completion(true);
  harness.drain_messages();
  ASSERT_TRUE(harness.state_machine.recovery_armed());

  active_connection_epoch = 2;
  display_helper::v2::RevertCommand stale_disconnect {harness.cancellation.current_generation()};
  stale_disconnect.connection_epoch = 1;
  stale_disconnect.from_disconnect = true;
  harness.state_machine.handle_message(stale_disconnect);
  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, 0);

  harness.state_machine.handle_message(display_helper::v2::HelperEventMessage {
    .event = display_helper::v2::HelperEvent::HeartbeatTimeout,
    .generation = harness.cancellation.current_generation(),
    .connection_epoch = 1,
  });
  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, 0);
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
}

TEST(DisplayHelperV2StateMachine, RecentApplyDefersAutonomousDisconnectRecovery) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {
    request,
    harness.cancellation.current_generation(),
  });

  EXPECT_TRUE(harness.state_machine.should_defer_disconnect_recovery());
  harness.clock.advance(std::chrono::milliseconds(5001));
  EXPECT_FALSE(harness.state_machine.should_defer_disconnect_recovery());

  // A direct REVERT is never treated as transient startup churn, even if it
  // arrives while the APPLY worker is still fenced.
  harness.state_machine.handle_message(display_helper::v2::RevertCommand {
    harness.cancellation.current_generation(),
  });
  EXPECT_FALSE(harness.state_machine.should_defer_disconnect_recovery());
}

TEST(DisplayHelperV2StateMachine, RecentApplyWithRestoreDisabledUsesNormalDisconnectPolicy) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();

  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  request.restore_on_disconnect = false;
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {
    request,
    harness.cancellation.current_generation(),
  });

  // v1 only keeps a broken recent APPLY alive for settling when the session
  // opted into restore-on-disconnect.  Policy=false must not issue the extra
  // 250/750ms modesets after its pipe disappears.
  EXPECT_FALSE(harness.state_machine.should_defer_disconnect_recovery());
  EXPECT_FALSE(harness.state_machine.begin_transient_disconnect_settlement());
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 1);

  display_helper::v2::RevertCommand disconnect {harness.cancellation.current_generation()};
  disconnect.from_disconnect = true;
  harness.state_machine.handle_message(disconnect);

  display_helper::v2::ApplyOutcome cancelled;
  cancelled.status = display_helper::v2::ApplyStatus::Fatal;
  harness.dispatcher.apply_completion(cancelled);
  harness.drain_messages();

  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 1);
  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, 0);
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
}

TEST(DisplayHelperV2StateMachine, RecentDisconnectVerifiesBeforeEachBoundedSettlingRepair) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {
    request,
    harness.cancellation.current_generation(),
  });

  ASSERT_TRUE(harness.state_machine.begin_transient_disconnect_settlement());

  display_helper::v2::ApplyOutcome failed;
  failed.status = display_helper::v2::ApplyStatus::Fatal;
  harness.dispatcher.apply_completion(failed);
  harness.drain_messages();
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 1);
  EXPECT_EQ(harness.dispatcher.verification_dispatch_count, 1);
  EXPECT_EQ(harness.dispatcher.verification_delay, std::chrono::milliseconds(250));

  // v1 checks the live topology first. Only a failed check repairs it, and
  // that repair is immediate rather than another delayed blind modeset.
  harness.dispatcher.verification_completion(false);
  harness.drain_messages();
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 2);
  EXPECT_EQ(harness.dispatcher.apply_delay, std::chrono::milliseconds(0));

  harness.dispatcher.apply_completion(failed);
  harness.drain_messages();
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 2);
  EXPECT_EQ(harness.dispatcher.verification_dispatch_count, 2);
  EXPECT_EQ(harness.dispatcher.verification_delay, std::chrono::milliseconds(750));

  harness.dispatcher.verification_completion(false);
  harness.drain_messages();
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 3);

  harness.dispatcher.apply_completion(failed);
  harness.drain_messages();
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 3);
  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, 0);
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
}

TEST(DisplayHelperV2StateMachine, RecentDisconnectWithStickyStateAvoidsExtraRepair) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {
    request,
    harness.cancellation.current_generation(),
  });
  ASSERT_TRUE(harness.state_machine.begin_transient_disconnect_settlement());

  display_helper::v2::ApplyOutcome applied;
  applied.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(applied);
  harness.drain_messages();
  ASSERT_TRUE(harness.dispatcher.verification_completion);

  // The initial response gate still runs, but the normal 750ms post-apply
  // check is replaced by the disconnect-specific 250ms health check.
  harness.dispatcher.verification_completion(true);
  harness.drain_messages();
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 1);
  EXPECT_EQ(harness.dispatcher.verification_dispatch_count, 2);
  EXPECT_EQ(harness.dispatcher.verification_delay, std::chrono::milliseconds(250));
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Verification);

  harness.dispatcher.verification_completion(true);
  harness.drain_messages();
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 1);
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
}

TEST(DisplayHelperV2StateMachine, ApplyBlanksHdrOnlyWhenRequested) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  request.hdr_blank = true;

  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});
  display_helper::v2::ApplyOutcome outcome;
  outcome.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(outcome);
  harness.drain_messages();
  harness.dispatcher.verification_completion(true);
  harness.drain_messages();

  EXPECT_EQ(harness.workarounds.blank_calls, 1);
  EXPECT_EQ(harness.workarounds.last_delay, std::chrono::milliseconds(1000));
}

TEST(DisplayHelperV2StateMachine, ApplyRetriesOnRetryable) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};

  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  display_helper::v2::ApplyOutcome outcome;
  outcome.status = display_helper::v2::ApplyStatus::Retryable;
  harness.dispatcher.apply_completion(outcome);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);
  EXPECT_EQ(harness.dispatcher.apply_delay, std::chrono::milliseconds(500));
  EXPECT_FALSE(harness.apply_result.has_value());
}

TEST(DisplayHelperV2StateMachine, NewApplySupersedesDeferredRevert) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();

  display_helper::v2::ApplyRequest first_request;
  first_request.configuration = display_device::SingleDisplayConfiguration {};
  first_request.configuration->m_device_id = "first";
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {
    first_request,
    harness.cancellation.current_generation(),
  });
  ASSERT_TRUE(harness.dispatcher.apply_completion);

  // A disconnect/revert reaches the mutation fence first, then a fast new
  // session starts before the cancelled APPLY reports completion. The newest
  // session intent must win; running the queued REVERT first would restore the
  // desktop and impose its full grace window before this APPLY.
  harness.state_machine.handle_message(display_helper::v2::RevertCommand {
    harness.cancellation.current_generation(),
  });

  display_helper::v2::ApplyRequest replacement_request;
  replacement_request.configuration = display_device::SingleDisplayConfiguration {};
  replacement_request.configuration->m_device_id = "replacement";
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {
    replacement_request,
    harness.cancellation.current_generation(),
  });

  display_helper::v2::ApplyOutcome cancelled;
  cancelled.status = display_helper::v2::ApplyStatus::Fatal;
  harness.dispatcher.apply_completion(cancelled);
  harness.drain_messages();

  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, 0);
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 2);
  ASSERT_TRUE(harness.dispatcher.apply_request.configuration);
  EXPECT_EQ(harness.dispatcher.apply_request.configuration->m_device_id, "replacement");
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);
}

TEST(DisplayHelperV2StateMachine, DeferredApplySurvivesHeartbeatAfterSupersedingExplicitRevert) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();

  display_helper::v2::ApplyRequest established;
  established.configuration = display_device::SingleDisplayConfiguration {};
  established.configuration->m_device_id = "ESTABLISHED";
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {
    established,
    harness.cancellation.current_generation(),
  });
  display_helper::v2::ApplyOutcome applied;
  applied.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(applied);
  harness.drain_messages();
  ASSERT_TRUE(harness.dispatcher.verification_completion);
  harness.dispatcher.verification_completion(true);
  harness.drain_messages();
  ASSERT_TRUE(harness.state_machine.recovery_armed());

  display_helper::v2::ApplyRequest in_flight;
  in_flight.configuration = display_device::SingleDisplayConfiguration {};
  in_flight.configuration->m_device_id = "IN_FLIGHT";
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {
    in_flight,
    harness.cancellation.current_generation(),
  });

  // An explicit REVERT latches durable recovery intent, but a later APPLY is
  // the newest session target even though it must wait for the current worker.
  harness.state_machine.handle_message(display_helper::v2::RevertCommand {
    harness.cancellation.current_generation(),
  });
  display_helper::v2::ApplyRequest replacement;
  replacement.configuration = display_device::SingleDisplayConfiguration {};
  replacement.configuration->m_device_id = "REPLACEMENT";
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {
    replacement,
    harness.cancellation.current_generation(),
  });

  harness.state_machine.handle_message(display_helper::v2::HelperEventMessage {
    .event = display_helper::v2::HelperEvent::HeartbeatTimeout,
    .generation = harness.cancellation.current_generation(),
  });

  // The heartbeat is autonomous policy. It must leave the queued newest APPLY
  // intact instead of replacing it with a DISARM or recovery command.
  harness.dispatcher.apply_completion(applied);
  harness.drain_messages();

  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, 0);
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 3);
  ASSERT_TRUE(harness.dispatcher.apply_request.configuration);
  EXPECT_EQ(harness.dispatcher.apply_request.configuration->m_device_id, "REPLACEMENT");
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);
}

TEST(DisplayHelperV2StateMachine, ApplyStopsAfterMaxRetries) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};

  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  display_helper::v2::ApplyOutcome outcome;
  outcome.status = display_helper::v2::ApplyStatus::Retryable;

  for (int i = 0; i < 3; ++i) {
    harness.dispatcher.apply_completion(outcome);
    harness.drain_messages();
  }

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  ASSERT_TRUE(harness.apply_result.has_value());
  EXPECT_EQ(harness.apply_result, display_helper::v2::ApplyStatus::Retryable);
}

TEST(DisplayHelperV2StateMachine, VirtualHdrStateFailureRetriesThenFallsBackToSdrAndVerifies) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  request.configuration->m_device_id = "virtual";
  request.configuration->m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::EnsureOnlyDisplay;
  request.configuration->m_hdr_state = display_device::HdrState::Enabled;
  request.topology = display_device::ActiveTopology {{"virtual"}};
  request.monitor_positions.emplace_back("physical", display_device::Point {32, 64});
  request.refresh_rate_overrides.emplace_back("physical", std::pair<unsigned int, unsigned int> {120, 1});
  request.hdr_blank = true;
  request.prefer_golden_first = true;
  request.restore_on_disconnect = false;
  request.omit_final_initial_hdr_reapply = true;
  request.virtual_layout = "extended";
  request.snapshot_exclusions = std::vector<std::string> {"virtual"};
  request.request_id = 42;

  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {
    request,
    harness.cancellation.current_generation(),
  });
  ASSERT_EQ(harness.dispatcher.apply_dispatch_count, 1);
  const auto original_request = harness.dispatcher.apply_request;

  display_helper::v2::ApplyOutcome hdr_failed;
  hdr_failed.status = display_helper::v2::ApplyStatus::HdrStateFailed;
  hdr_failed.virtual_display_requested = true;
  hdr_failed.display_may_have_changed = true;

  harness.dispatcher.apply_completion(hdr_failed);
  harness.drain_messages();
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 2);
  EXPECT_EQ(harness.dispatcher.apply_delay, std::chrono::milliseconds(500));
  ASSERT_TRUE(harness.dispatcher.apply_request.configuration);
  EXPECT_EQ(harness.dispatcher.apply_request.configuration->m_hdr_state, display_device::HdrState::Enabled);
  EXPECT_EQ(harness.apply_result_count, 0);
  EXPECT_EQ(harness.verification_result_count, 0);

  harness.dispatcher.apply_completion(hdr_failed);
  harness.drain_messages();
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 3);
  EXPECT_EQ(harness.dispatcher.apply_delay, std::chrono::milliseconds(1000));
  ASSERT_TRUE(harness.dispatcher.apply_request.configuration);
  EXPECT_EQ(harness.dispatcher.apply_request.configuration->m_hdr_state, display_device::HdrState::Enabled);
  EXPECT_EQ(harness.apply_result_count, 0);
  EXPECT_EQ(harness.verification_result_count, 0);

  harness.dispatcher.apply_completion(hdr_failed);
  harness.drain_messages();
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 4);
  EXPECT_EQ(harness.dispatcher.apply_delay, std::chrono::milliseconds(0));
  ASSERT_TRUE(harness.dispatcher.apply_request.configuration);
  EXPECT_EQ(harness.dispatcher.apply_request.configuration->m_hdr_state, display_device::HdrState::Disabled);
  EXPECT_EQ(harness.dispatcher.apply_request.configuration->m_device_id, original_request.configuration->m_device_id);
  EXPECT_EQ(harness.dispatcher.apply_request.configuration->m_device_prep, original_request.configuration->m_device_prep);
  EXPECT_EQ(harness.dispatcher.apply_request.configuration->m_resolution.has_value(), original_request.configuration->m_resolution.has_value());
  EXPECT_EQ(harness.dispatcher.apply_request.configuration->m_refresh_rate.has_value(), original_request.configuration->m_refresh_rate.has_value());
  EXPECT_EQ(harness.dispatcher.apply_request.topology, original_request.topology);
  ASSERT_EQ(harness.dispatcher.apply_request.monitor_positions.size(), original_request.monitor_positions.size());
  EXPECT_EQ(harness.dispatcher.apply_request.monitor_positions.front().first, original_request.monitor_positions.front().first);
  EXPECT_EQ(harness.dispatcher.apply_request.monitor_positions.front().second.m_x, original_request.monitor_positions.front().second.m_x);
  EXPECT_EQ(harness.dispatcher.apply_request.monitor_positions.front().second.m_y, original_request.monitor_positions.front().second.m_y);
  EXPECT_EQ(harness.dispatcher.apply_request.refresh_rate_overrides, original_request.refresh_rate_overrides);
  EXPECT_EQ(harness.dispatcher.apply_request.hdr_blank, original_request.hdr_blank);
  EXPECT_EQ(harness.dispatcher.apply_request.prefer_golden_first, original_request.prefer_golden_first);
  EXPECT_EQ(harness.dispatcher.apply_request.restore_on_disconnect, original_request.restore_on_disconnect);
  EXPECT_EQ(harness.dispatcher.apply_request.omit_final_initial_hdr_reapply, original_request.omit_final_initial_hdr_reapply);
  EXPECT_EQ(harness.dispatcher.apply_request.virtual_layout, original_request.virtual_layout);
  EXPECT_EQ(harness.dispatcher.apply_request.snapshot_exclusions, original_request.snapshot_exclusions);
  EXPECT_EQ(harness.dispatcher.apply_request.request_id, original_request.request_id);
  EXPECT_EQ(harness.apply_result_count, 0);
  EXPECT_EQ(harness.verification_result_count, 0);

  display_helper::v2::ApplyOutcome sdr_applied;
  sdr_applied.status = display_helper::v2::ApplyStatus::Ok;
  sdr_applied.virtual_display_requested = true;
  sdr_applied.display_may_have_changed = true;
  harness.dispatcher.apply_completion(sdr_applied);
  harness.drain_messages();
  ASSERT_EQ(harness.state_machine.state(), display_helper::v2::State::Verification);
  ASSERT_TRUE(harness.dispatcher.verification_completion);
  EXPECT_EQ(harness.apply_result_count, 1);
  ASSERT_TRUE(harness.apply_result);
  EXPECT_EQ(*harness.apply_result, display_helper::v2::ApplyStatus::Ok);
  EXPECT_EQ(harness.verification_result_count, 0);

  harness.dispatcher.verification_completion(true);
  harness.drain_messages();
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::VirtualDisplayMonitoring);
  EXPECT_TRUE(harness.state_machine.recovery_armed());
  EXPECT_EQ(harness.apply_result_count, 1);
  EXPECT_EQ(harness.verification_result_count, 1);
  ASSERT_TRUE(harness.verification_result);
  EXPECT_TRUE(*harness.verification_result);
}

TEST(DisplayHelperV2StateMachine, PhysicalHdrStateFailureRetriesWithoutSdrFallback) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  request.configuration->m_device_id = "physical";
  request.configuration->m_hdr_state = display_device::HdrState::Enabled;

  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {
    request,
    harness.cancellation.current_generation(),
  });

  display_helper::v2::ApplyOutcome hdr_failed;
  hdr_failed.status = display_helper::v2::ApplyStatus::HdrStateFailed;
  harness.dispatcher.apply_completion(hdr_failed);
  harness.drain_messages();
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 2);
  EXPECT_EQ(harness.dispatcher.apply_delay, std::chrono::milliseconds(500));

  harness.dispatcher.apply_completion(hdr_failed);
  harness.drain_messages();
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 3);
  EXPECT_EQ(harness.dispatcher.apply_delay, std::chrono::milliseconds(1000));

  harness.dispatcher.apply_completion(hdr_failed);
  harness.drain_messages();

  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 3);
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  EXPECT_EQ(harness.apply_result_count, 1);
  ASSERT_TRUE(harness.apply_result);
  EXPECT_EQ(*harness.apply_result, display_helper::v2::ApplyStatus::HdrStateFailed);
  EXPECT_EQ(harness.verification_result_count, 1);
  ASSERT_TRUE(harness.verification_result);
  EXPECT_FALSE(*harness.verification_result);
}

TEST(DisplayHelperV2StateMachine, GenericVirtualHdrFailuresRetainSdrFallback) {
  for (const auto status : {display_helper::v2::ApplyStatus::Retryable, display_helper::v2::ApplyStatus::VerificationFailed}) {
    SCOPED_TRACE(static_cast<int>(status));
    StateMachineHarness harness;
    display_helper::v2::ApplyRequest request;
    request.configuration = display_device::SingleDisplayConfiguration {};
    request.configuration->m_device_id = "virtual";
    request.configuration->m_hdr_state = display_device::HdrState::Enabled;
    request.virtual_layout = "extended";
    harness.state_machine.handle_message(display_helper::v2::ApplyCommand {
      request,
      harness.cancellation.current_generation(),
    });

    display_helper::v2::ApplyOutcome failed;
    failed.status = status;
    failed.virtual_display_requested = true;
    for (int attempt = 0; attempt < 3; ++attempt) {
      harness.dispatcher.apply_completion(failed);
      harness.drain_messages();
    }

    EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 4);
    EXPECT_EQ(harness.dispatcher.apply_delay, std::chrono::milliseconds(0));
    ASSERT_TRUE(harness.dispatcher.apply_request.configuration);
    EXPECT_EQ(harness.dispatcher.apply_request.configuration->m_hdr_state, display_device::HdrState::Disabled);
    EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);
    EXPECT_EQ(harness.apply_result_count, 0);
    EXPECT_EQ(harness.verification_result_count, 0);
  }
}

TEST(DisplayHelperV2StateMachine, VerificationFailureReappliesBeforeReportingFailure) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};

  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  display_helper::v2::ApplyOutcome applied;
  applied.status = display_helper::v2::ApplyStatus::Ok;

  harness.dispatcher.apply_completion(applied);
  harness.drain_messages();
  ASSERT_TRUE(harness.dispatcher.verification_completion);
  harness.dispatcher.verification_completion(false);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 2);
  EXPECT_EQ(harness.dispatcher.apply_delay, std::chrono::milliseconds(0));
  EXPECT_FALSE(harness.verification_result.has_value());

  harness.dispatcher.apply_completion(applied);
  harness.drain_messages();
  harness.dispatcher.verification_completion(false);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 3);
  EXPECT_EQ(harness.dispatcher.apply_delay, std::chrono::milliseconds(750));
  EXPECT_FALSE(harness.verification_result.has_value());

  harness.dispatcher.apply_completion(applied);
  harness.drain_messages();
  harness.dispatcher.verification_completion(false);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  ASSERT_TRUE(harness.verification_result.has_value());
  EXPECT_FALSE(*harness.verification_result);
}

TEST(DisplayHelperV2StateMachine, HdrVerificationUsesLegacySettlingStairStep) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  request.configuration->m_hdr_state = display_device::HdrState::Enabled;
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {
    request,
    harness.cancellation.current_generation(),
  });

  display_helper::v2::ApplyOutcome applied;
  applied.status = display_helper::v2::ApplyStatus::Ok;
  const std::array expected_delays {
    std::chrono::milliseconds(0),
    std::chrono::milliseconds(750),
    std::chrono::milliseconds(2500),
    std::chrono::milliseconds(5500),
  };

  for (const auto delay : expected_delays) {
    harness.dispatcher.apply_completion(applied);
    harness.drain_messages();
    ASSERT_TRUE(harness.dispatcher.verification_completion);
    harness.dispatcher.verification_completion(false);
    harness.drain_messages();
    EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);
    EXPECT_EQ(harness.dispatcher.apply_delay, delay);
  }

  harness.dispatcher.apply_completion(applied);
  harness.drain_messages();
  ASSERT_TRUE(harness.dispatcher.verification_completion);
  harness.dispatcher.verification_completion(false);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  ASSERT_TRUE(harness.verification_result.has_value());
  EXPECT_FALSE(*harness.verification_result);
}

TEST(DisplayHelperV2StateMachine, HdrStabilizationRepairUsesTheNextLegacyStage) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  request.configuration->m_hdr_state = display_device::HdrState::Enabled;
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {
    request,
    harness.cancellation.current_generation(),
  });

  display_helper::v2::ApplyOutcome applied;
  applied.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(applied);
  harness.drain_messages();
  ASSERT_TRUE(harness.dispatcher.verification_completion);
  harness.dispatcher.verification_completion(true);
  harness.drain_messages();

  // The first delayed (750ms) settling check fails. v1 reapplied immediately,
  // then used the next 2.5s settling stage if the repair also failed.
  ASSERT_TRUE(harness.dispatcher.verification_completion);
  harness.dispatcher.verification_completion(false);
  harness.drain_messages();
  EXPECT_EQ(harness.dispatcher.apply_delay, std::chrono::milliseconds(0));

  harness.dispatcher.apply_completion(applied);
  harness.drain_messages();
  ASSERT_TRUE(harness.dispatcher.verification_completion);
  harness.dispatcher.verification_completion(false);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);
  EXPECT_EQ(harness.dispatcher.apply_delay, std::chrono::milliseconds(2500));
}

TEST(DisplayHelperV2StateMachine, FailedStabilizationRepairKeepsVirtualMonitoring) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  request.configuration->m_device_id = "virtual";
  request.virtual_layout = "extended";
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {
    request,
    harness.cancellation.current_generation(),
  });

  display_helper::v2::ApplyOutcome applied;
  applied.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(applied);
  harness.drain_messages();
  ASSERT_TRUE(harness.dispatcher.verification_completion);
  harness.dispatcher.verification_completion(true);
  harness.drain_messages();
  ASSERT_EQ(harness.state_machine.state(), display_helper::v2::State::VirtualDisplayMonitoring);
  ASSERT_TRUE(harness.verification_result.has_value());
  EXPECT_TRUE(*harness.verification_result);

  ASSERT_TRUE(harness.dispatcher.verification_completion);
  harness.dispatcher.verification_completion(false);
  harness.drain_messages();
  ASSERT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);

  display_helper::v2::ApplyOutcome failed;
  failed.status = display_helper::v2::ApplyStatus::Fatal;
  harness.dispatcher.apply_completion(failed);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::VirtualDisplayMonitoring);
  ASSERT_TRUE(harness.verification_result.has_value());
  EXPECT_TRUE(*harness.verification_result);
}

TEST(DisplayHelperV2StateMachine, InitialVirtualApplyFailureDoesNotEnterMonitoring) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  request.virtual_layout = "extended";
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {
    request,
    harness.cancellation.current_generation(),
  });

  display_helper::v2::ApplyOutcome failed;
  failed.status = display_helper::v2::ApplyStatus::Fatal;
  harness.dispatcher.apply_completion(failed);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  ASSERT_TRUE(harness.verification_result.has_value());
  EXPECT_FALSE(*harness.verification_result);
}

TEST(DisplayHelperV2StateMachine, SuccessfulRepairReturnsToSteadyState) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};

  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  display_helper::v2::ApplyOutcome applied;
  applied.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(applied);
  harness.drain_messages();

  ASSERT_TRUE(harness.dispatcher.verification_completion);
  harness.dispatcher.verification_completion(false);
  harness.drain_messages();
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);

  harness.dispatcher.apply_completion(applied);
  harness.drain_messages();
  ASSERT_TRUE(harness.dispatcher.verification_completion);
  harness.dispatcher.verification_completion(true);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  ASSERT_TRUE(harness.verification_result.has_value());
  EXPECT_TRUE(*harness.verification_result);
}

TEST(DisplayHelperV2StateMachine, VerificationPreservesDefaultDeviceAndCarriesResolvedGroup) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  EXPECT_TRUE(request.configuration->m_device_id.empty());

  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  display_helper::v2::ApplyOutcome applied;
  applied.status = display_helper::v2::ApplyStatus::Ok;
  applied.resolved_target = display_helper::v2::ResolvedConfigurationTarget {
    .kind = display_helper::v2::DeviceTargetKind::DefaultPrimaryGroup,
    .representative_device_id = "RESOLVED_PRIMARY",
    .duplicate_device_ids = {"RESOLVED_PRIMARY", "DUPLICATE_PRIMARY"},
  };
  harness.dispatcher.apply_completion(applied);
  harness.drain_messages();

  ASSERT_TRUE(harness.dispatcher.verification_request.configuration.has_value());
  EXPECT_TRUE(harness.dispatcher.verification_request.configuration->m_device_id.empty());
  ASSERT_TRUE(harness.dispatcher.verification_target.has_value());
  EXPECT_EQ(harness.dispatcher.verification_target->representative_device_id, "RESOLVED_PRIMARY");
  EXPECT_EQ(
    harness.dispatcher.verification_target->duplicate_device_ids,
    (std::set<std::string> {"RESOLVED_PRIMARY", "DUPLICATE_PRIMARY"}));
}

TEST(DisplayHelperV2StateMachine, FailedReplacementKeepsPriorLeaseButUsesNewDisconnectPolicy) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();

  display_helper::v2::ApplyRequest first_request;
  first_request.configuration = display_device::SingleDisplayConfiguration {};
  first_request.restore_on_disconnect = true;
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {
    first_request,
    harness.cancellation.current_generation()});

  display_helper::v2::ApplyOutcome first_applied;
  first_applied.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(first_applied);
  harness.drain_messages();
  ASSERT_TRUE(harness.dispatcher.verification_completion);
  harness.dispatcher.verification_completion(true);
  harness.drain_messages();
  ASSERT_TRUE(harness.state_machine.recovery_armed());

  display_helper::v2::ApplyRequest replacement_request;
  replacement_request.configuration = display_device::SingleDisplayConfiguration {};
  replacement_request.restore_on_disconnect = false;
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {
    replacement_request,
    harness.cancellation.current_generation()});

  display_helper::v2::ApplyOutcome replacement_failed;
  replacement_failed.status = display_helper::v2::ApplyStatus::Fatal;
  harness.dispatcher.apply_completion(replacement_failed);
  harness.drain_messages();

  EXPECT_TRUE(harness.state_machine.recovery_armed());
  EXPECT_EQ(harness.dispatcher.reset_dispatch_count, 0);
  harness.state_machine.handle_message(display_helper::v2::HelperEventMessage {
    display_helper::v2::HelperEvent::HeartbeatTimeout,
    harness.cancellation.current_generation()});

  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, 0);
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  EXPECT_FALSE(harness.state_machine.recovery_armed());
  EXPECT_EQ(harness.task_manager.deleted, 1);
}

TEST(DisplayHelperV2StateMachine, VirtualDisplayResetTriggersDispatch) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  request.virtual_layout = "extended";

  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  display_helper::v2::ApplyOutcome outcome;
  outcome.status = display_helper::v2::ApplyStatus::NeedsVirtualDisplayReset;
  outcome.virtual_display_requested = true;

  harness.dispatcher.apply_completion(outcome);
  harness.drain_messages();

  EXPECT_TRUE(harness.dispatcher.apply_reset_virtual_display);
}

// Test: Display event during APPLY with virtual display triggers re-apply.
// When a virtual display device crashes/reappears during an active apply operation,
// the state machine should restart the apply.
TEST(DisplayHelperV2StateMachine, DisplayEventDuringApplyWithVirtualDisplayTriggersReapply) {
  StateMachineHarness harness;
  harness.virtual_display.current_device_id = "virtual_new";
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  request.configuration->m_device_id = "virtual_old";
  request.topology = display_device::ActiveTopology {{request.configuration->m_device_id}};
  request.monitor_positions.emplace_back(request.configuration->m_device_id, display_device::Point {0, 0});
  request.virtual_layout = "extended";  // Virtual display requested

  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);

  const int apply_dispatches_before = harness.dispatcher.apply_dispatch_count;

  // Simulate display device removal/arrival (virtual display crash and recovery)
  harness.state_machine.handle_message(display_helper::v2::DisplayEventMessage {
    display_helper::v2::DisplayEvent::DeviceArrival,
    harness.cancellation.current_generation()});

  // The replacement is fenced behind the active worker so its task/recovery
  // state cannot be overwritten while SetDisplayConfig may still be running.
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, apply_dispatches_before);

  display_helper::v2::ApplyOutcome cancelled;
  cancelled.status = display_helper::v2::ApplyStatus::Fatal;
  harness.dispatcher.apply_completion(cancelled);
  harness.drain_messages();

  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, apply_dispatches_before + 1);
  ASSERT_TRUE(harness.dispatcher.apply_request.configuration.has_value());
  EXPECT_EQ(harness.dispatcher.apply_request.configuration->m_device_id, "virtual_new");
  ASSERT_TRUE(harness.dispatcher.apply_request.topology.has_value());
  ASSERT_FALSE(harness.dispatcher.apply_request.topology->empty());
  ASSERT_FALSE(harness.dispatcher.apply_request.topology->front().empty());
  EXPECT_EQ(harness.dispatcher.apply_request.topology->front().front(), "virtual_new");
  ASSERT_FALSE(harness.dispatcher.apply_request.monitor_positions.empty());
  EXPECT_EQ(harness.dispatcher.apply_request.monitor_positions.front().first, "virtual_new");
}

// Test: Successful apply with virtual display enters VirtualDisplayMonitoring state.
// This state monitors for device crashes and triggers re-apply when needed.
TEST(DisplayHelperV2StateMachine, VirtualDisplayEntersMonitoringStateAfterSuccessfulApply) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  request.virtual_layout = "extended";  // Virtual display requested

  // Complete a successful apply cycle
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  display_helper::v2::ApplyOutcome apply_ok;
  apply_ok.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(apply_ok);
  harness.drain_messages();

  harness.dispatcher.verification_completion(true);
  harness.drain_messages();

  // After successful verification with virtual display, we're in VirtualDisplayMonitoring
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::VirtualDisplayMonitoring);
  EXPECT_TRUE(harness.state_machine.recovery_armed());
}

// Test: Display event in VirtualDisplayMonitoring state triggers re-apply.
// When a virtual display device crashes/reappears during an active streaming session,
// the state machine should re-apply the streaming configuration.
TEST(DisplayHelperV2StateMachine, DisplayEventInMonitoringStateTriggersReapply) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  request.configuration->m_device_id = "virtual_old";
  request.topology = display_device::ActiveTopology {{request.configuration->m_device_id}};
  request.monitor_positions.emplace_back(request.configuration->m_device_id, display_device::Point {0, 0});
  request.virtual_layout = "extended";  // Virtual display requested

  // Complete a successful apply cycle to enter VirtualDisplayMonitoring
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  display_helper::v2::ApplyOutcome apply_ok;
  apply_ok.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(apply_ok);
  harness.drain_messages();

  harness.dispatcher.verification_completion(true);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::VirtualDisplayMonitoring);

  const int apply_dispatches_before = harness.dispatcher.apply_dispatch_count;

  harness.virtual_display.current_device_id = "virtual_new";
  // Simulate display device removal/arrival (virtual display crash and recovery)
  harness.state_machine.handle_message(display_helper::v2::DisplayEventMessage {
    display_helper::v2::DisplayEvent::DeviceArrival,
    harness.cancellation.current_generation()});

  // State machine should re-apply the virtual display configuration
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, apply_dispatches_before + 1);
  ASSERT_TRUE(harness.dispatcher.apply_request.configuration.has_value());
  EXPECT_EQ(harness.dispatcher.apply_request.configuration->m_device_id, "virtual_new");
  ASSERT_TRUE(harness.dispatcher.apply_request.topology.has_value());
  ASSERT_FALSE(harness.dispatcher.apply_request.topology->empty());
  ASSERT_FALSE(harness.dispatcher.apply_request.topology->front().empty());
  EXPECT_EQ(harness.dispatcher.apply_request.topology->front().front(), "virtual_new");
  ASSERT_FALSE(harness.dispatcher.apply_request.monitor_positions.empty());
  EXPECT_EQ(harness.dispatcher.apply_request.monitor_positions.front().first, "virtual_new");
}

TEST(DisplayHelperV2StateMachine, DisplayEventInMonitoringStateWithSameVirtualDeviceDoesNotReapply) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  request.configuration->m_device_id = "virtual_current";
  request.topology = display_device::ActiveTopology {{request.configuration->m_device_id}};
  request.virtual_layout = "extended";
  harness.virtual_display.current_device_id = "virtual_current";

  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  display_helper::v2::ApplyOutcome apply_ok;
  apply_ok.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(apply_ok);
  harness.drain_messages();

  harness.dispatcher.verification_completion(true);
  harness.drain_messages();

  ASSERT_EQ(harness.state_machine.state(), display_helper::v2::State::VirtualDisplayMonitoring);
  const int apply_dispatches_before = harness.dispatcher.apply_dispatch_count;

  harness.state_machine.handle_message(display_helper::v2::DisplayEventMessage {
    display_helper::v2::DisplayEvent::DisplayChange,
    harness.cancellation.current_generation()});

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::VirtualDisplayMonitoring);
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, apply_dispatches_before);
}

TEST(DisplayHelperV2StateMachine, RefreshRatePersistsIntoVirtualDisplayRepair) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  request.configuration->m_device_id = "virtual_old";
  request.virtual_layout = "extended";

  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});
  display_helper::v2::ApplyOutcome apply_ok;
  apply_ok.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(apply_ok);
  harness.drain_messages();
  harness.dispatcher.verification_completion(true);
  harness.drain_messages();

  harness.state_machine.handle_message(display_helper::v2::RefreshRateCommand {
    .device_id = "virtual_old",
    .numerator = 120,
    .denominator = 1,
    .generation = harness.cancellation.current_generation(),
    .connection_epoch = 7,
  });
  ASSERT_TRUE(harness.dispatcher.refresh_completion);
  harness.dispatcher.refresh_completion(true);
  harness.drain_messages();
  ASSERT_TRUE(harness.refresh_result.has_value());
  EXPECT_TRUE(*harness.refresh_result);

  harness.virtual_display.current_device_id = "virtual_new";
  const int before = harness.dispatcher.apply_dispatch_count;
  harness.state_machine.handle_message(display_helper::v2::DisplayEventMessage {
    display_helper::v2::DisplayEvent::DeviceArrival,
    harness.cancellation.current_generation(),
  });

  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, before + 1);
  ASSERT_TRUE(harness.dispatcher.apply_request.configuration.has_value());
  ASSERT_TRUE(harness.dispatcher.apply_request.configuration->m_refresh_rate.has_value());
  const auto *rate = std::get_if<display_device::Rational>(&*harness.dispatcher.apply_request.configuration->m_refresh_rate);
  ASSERT_NE(rate, nullptr);
  EXPECT_EQ(rate->m_numerator, 120u);
  EXPECT_EQ(rate->m_denominator, 1u);
}

TEST(DisplayHelperV2StateMachine, ApplyWaitsForAnInFlightRefreshRateMutation) {
  StateMachineHarness harness;
  harness.state_machine.handle_message(display_helper::v2::RefreshRateCommand {
    .device_id = "display",
    .numerator = 120,
    .denominator = 1,
    .generation = harness.cancellation.current_generation(),
  });
  ASSERT_EQ(harness.dispatcher.refresh_dispatch_count, 1);
  ASSERT_TRUE(harness.dispatcher.refresh_completion);

  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {
    request,
    harness.cancellation.current_generation(),
  });
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 0);

  // The queued APPLY cancels the predecessor. Its completion nevertheless
  // releases the refresh fence before the replacement starts.
  harness.dispatcher.refresh_completion(true);
  harness.drain_messages();

  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 1);
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);
}

TEST(DisplayHelperV2StateMachine, RefreshRateInvalidatesPendingStabilizationVerification) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  request.configuration->m_device_id = "virtual";
  request.virtual_layout = "extended";

  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});
  display_helper::v2::ApplyOutcome apply_ok;
  apply_ok.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(apply_ok);
  harness.drain_messages();
  ASSERT_TRUE(harness.dispatcher.verification_completion);
  harness.dispatcher.verification_completion(true);
  harness.drain_messages();

  // The successful initial verification schedules the delayed stabilization
  // check. Keep its completion so it can arrive after the refresh is accepted.
  ASSERT_TRUE(harness.dispatcher.verification_completion);
  const auto stale_verification = harness.dispatcher.verification_completion;
  const int apply_dispatches_before_refresh = harness.dispatcher.apply_dispatch_count;

  harness.state_machine.handle_message(display_helper::v2::RefreshRateCommand {
    .device_id = "virtual",
    .numerator = 464,
    .denominator = 1,
    .generation = harness.cancellation.current_generation(),
  });
  ASSERT_EQ(harness.dispatcher.refresh_dispatch_count, 1);

  // A failed old verification must not replace the refresh mutation with a
  // full APPLY using the previous session refresh rate.
  stale_verification(false);
  harness.drain_messages();
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, apply_dispatches_before_refresh);

  ASSERT_TRUE(harness.dispatcher.refresh_completion);
  harness.dispatcher.refresh_completion(true);
  harness.drain_messages();
  ASSERT_TRUE(harness.refresh_result.has_value());
  EXPECT_TRUE(*harness.refresh_result);

  const int verification_dispatches_before_failed_refresh = harness.dispatcher.verification_dispatch_count;
  harness.state_machine.handle_message(display_helper::v2::RefreshRateCommand {
    .device_id = "virtual",
    .numerator = 116,
    .denominator = 1,
    .generation = harness.cancellation.current_generation(),
  });
  ASSERT_EQ(harness.dispatcher.refresh_dispatch_count, 2);
  ASSERT_TRUE(harness.dispatcher.refresh_completion);
  harness.dispatcher.refresh_completion(false);
  harness.drain_messages();
  ASSERT_TRUE(harness.refresh_result.has_value());
  EXPECT_FALSE(*harness.refresh_result);
  EXPECT_EQ(harness.dispatcher.verification_dispatch_count, verification_dispatches_before_failed_refresh + 1);
}

// Test: Non-virtual display still goes to Waiting state after successful apply.
TEST(DisplayHelperV2StateMachine, NonVirtualDisplayGoesToWaitingState) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  // No virtual_layout - regular display

  // Complete a successful apply cycle
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  display_helper::v2::ApplyOutcome apply_ok;
  apply_ok.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(apply_ok);
  harness.drain_messages();

  harness.dispatcher.verification_completion(true);
  harness.drain_messages();

  // Non-virtual displays go to Waiting state (not VirtualDisplayMonitoring)
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  EXPECT_TRUE(harness.state_machine.recovery_armed());
}

TEST(DisplayHelperV2StateMachine, RevertRunsRecoveryAndValidation) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();

  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);
  // The boot-restore scheduled task must survive until the restore is confirmed:
  // a reboot mid-restore still needs it.
  EXPECT_EQ(harness.task_manager.deleted, 0);
  // Explicit REVERTs get a 5s grace window so a fast app switch can DISARM first.
  EXPECT_EQ(harness.dispatcher.recovery_delay, std::chrono::milliseconds(5000));

  display_helper::v2::RecoveryOutcome outcome;
  outcome.success = true;
  outcome.snapshot = make_snapshot("A");

  harness.dispatcher.recovery_completion(outcome);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::RecoveryValidation);
  ASSERT_TRUE(harness.dispatcher.recovery_validation_completion);

  harness.dispatcher.recovery_validation_completion(true);
  harness.drain_messages();

  ASSERT_TRUE(harness.exit_code.has_value());
  EXPECT_EQ(harness.exit_code.value(), 0);
  EXPECT_EQ(harness.task_manager.deleted, 1);
  EXPECT_EQ(harness.workarounds.refresh_calls, 1);
}

TEST(DisplayHelperV2StateMachine, RevertWithNoSnapshotsExitsImmediately) {
  StateMachineHarness harness;

  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});

  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, 0);
  ASSERT_TRUE(harness.exit_code.has_value());
  EXPECT_EQ(harness.exit_code.value(), 0);
}

TEST(DisplayHelperV2StateMachine, RecoveryFailureKeepsEventLoopArmed) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();

  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});

  display_helper::v2::RecoveryOutcome recovery_fail;
  recovery_fail.success = false;
  harness.dispatcher.recovery_completion(recovery_fail);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::EventLoop);
  EXPECT_TRUE(harness.state_machine.recovery_armed());
  EXPECT_FALSE(harness.exit_code.has_value());
}

TEST(DisplayHelperV2StateMachine, RecoveryValidationFailureAllowsReconnectCycle) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};

  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});

  display_helper::v2::RecoveryOutcome recovery_ok;
  recovery_ok.success = true;
  recovery_ok.snapshot = make_snapshot("A");
  harness.dispatcher.recovery_completion(recovery_ok);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::RecoveryValidation);
  ASSERT_TRUE(harness.dispatcher.recovery_validation_completion);

  harness.dispatcher.recovery_validation_completion(false);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::EventLoop);
  EXPECT_TRUE(harness.state_machine.recovery_armed());
  EXPECT_FALSE(harness.exit_code.has_value());

  harness.state_machine.handle_message(display_helper::v2::DisarmCommand {harness.cancellation.current_generation()});
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  EXPECT_FALSE(harness.state_machine.recovery_armed());

  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);
  ASSERT_TRUE(harness.dispatcher.apply_completion);

  display_helper::v2::ApplyOutcome apply_ok;
  apply_ok.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(apply_ok);
  harness.drain_messages();
  ASSERT_TRUE(harness.dispatcher.verification_completion);

  harness.dispatcher.verification_completion(true);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  EXPECT_TRUE(harness.state_machine.recovery_armed());
  EXPECT_FALSE(harness.exit_code.has_value());
}

TEST(DisplayHelperV2StateMachine, DisarmBeforeApplyWhileRecovering) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();
  std::vector<display_helper::v2::StateTransition> transitions;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};

  harness.state_machine.set_state_observer([&transitions](display_helper::v2::StateTransition transition) {
    transitions.push_back(std::move(transition));
  });

  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);

  display_helper::v2::RecoveryOutcome recovery_fail;
  recovery_fail.success = false;
  harness.dispatcher.recovery_completion(recovery_fail);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::EventLoop);
  EXPECT_TRUE(harness.state_machine.recovery_armed());

  const int recovery_dispatches_before = harness.dispatcher.recovery_dispatch_count;
  harness.state_machine.handle_message(display_helper::v2::DisarmCommand {harness.cancellation.current_generation()});
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);
  EXPECT_FALSE(harness.state_machine.recovery_armed());
  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, recovery_dispatches_before);
  ASSERT_GE(transitions.size(), 2u);
  EXPECT_EQ(transitions[transitions.size() - 2].to, display_helper::v2::State::Waiting);
  EXPECT_EQ(transitions.back().to, display_helper::v2::State::InProgress);

  display_helper::v2::ApplyOutcome applied;
  applied.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(applied);
  harness.drain_messages();
  ASSERT_TRUE(harness.dispatcher.verification_completion);
  harness.dispatcher.verification_completion(true);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  ASSERT_TRUE(harness.verification_result.has_value());
  EXPECT_TRUE(*harness.verification_result);
  EXPECT_TRUE(harness.state_machine.recovery_armed());
}

TEST(DisplayHelperV2StateMachine, EventLoopTriggersRecovery) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};

  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  display_helper::v2::ApplyOutcome apply_ok;
  apply_ok.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(apply_ok);
  harness.drain_messages();
  harness.dispatcher.verification_completion(true);
  harness.drain_messages();

  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});

  display_helper::v2::RecoveryOutcome recovery_fail;
  recovery_fail.success = false;
  harness.dispatcher.recovery_completion(recovery_fail);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::EventLoop);
  EXPECT_TRUE(harness.state_machine.recovery_armed());

  harness.state_machine.handle_message(display_helper::v2::DisplayEventMessage {
    display_helper::v2::DisplayEvent::DisplayChange,
    harness.cancellation.current_generation()});

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);
  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, 2);
}

TEST(DisplayHelperV2StateMachine, HeartbeatTimeoutTriggersRecovery) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();
  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});

  display_helper::v2::RecoveryOutcome recovery_fail;
  recovery_fail.success = false;
  harness.dispatcher.recovery_completion(recovery_fail);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::EventLoop);

  harness.state_machine.handle_message(display_helper::v2::HelperEventMessage {
    display_helper::v2::HelperEvent::HeartbeatTimeout,
    harness.cancellation.current_generation()});

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);
}

TEST(DisplayHelperV2StateMachine, DisarmFromEventLoopStopsRecoveryAttempts) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();

  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);

  display_helper::v2::RecoveryOutcome recovery_fail;
  recovery_fail.success = false;
  harness.dispatcher.recovery_completion(recovery_fail);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::EventLoop);
  EXPECT_TRUE(harness.state_machine.recovery_armed());
  const auto recovery_dispatches = harness.dispatcher.recovery_dispatch_count;

  harness.state_machine.handle_message(display_helper::v2::DisarmCommand {harness.cancellation.current_generation()});
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  EXPECT_FALSE(harness.state_machine.recovery_armed());
  EXPECT_EQ(harness.task_manager.deleted, 1);

  harness.state_machine.handle_message(display_helper::v2::DisplayEventMessage {
    display_helper::v2::DisplayEvent::DisplayChange,
    harness.cancellation.current_generation()});
  harness.state_machine.handle_message(display_helper::v2::HelperEventMessage {
    display_helper::v2::HelperEvent::HeartbeatTimeout,
    harness.cancellation.current_generation()});

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, recovery_dispatches);
}

TEST(DisplayHelperV2StateMachine, DisarmCancelsStaleOperations) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};

  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});
  ASSERT_TRUE(harness.dispatcher.apply_completion);

  harness.state_machine.handle_message(display_helper::v2::DisarmCommand {harness.cancellation.current_generation()});
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);
  EXPECT_EQ(harness.task_manager.deleted, 0);

  display_helper::v2::ApplyOutcome outcome;
  outcome.status = display_helper::v2::ApplyStatus::Fatal;
  outcome.durable_recovery_armed = true;
  harness.dispatcher.apply_completion(outcome);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  EXPECT_EQ(harness.task_manager.deleted, 1);
}

TEST(DisplayHelperV2StateMachine, DisarmAfterCancelledMutationRetainsRecoveryGuard) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};

  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});
  ASSERT_TRUE(harness.dispatcher.apply_completion);
  ASSERT_TRUE(harness.task_manager.create_restore_task(L""));  // worker boundary already crossed

  harness.state_machine.handle_message(display_helper::v2::DisarmCommand {harness.cancellation.current_generation()});
  EXPECT_EQ(harness.task_manager.deleted, 0);

  display_helper::v2::ApplyOutcome cancelled;
  cancelled.status = display_helper::v2::ApplyStatus::Fatal;
  cancelled.durable_recovery_armed = true;
  cancelled.display_may_have_changed = true;
  harness.dispatcher.apply_completion(cancelled);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  EXPECT_TRUE(harness.state_machine.recovery_armed());
  EXPECT_EQ(harness.task_manager.created, 1);
  EXPECT_EQ(harness.task_manager.deleted, 0);
}

TEST(DisplayHelperV2StateMachine, SupersedingApplyWaitsForCancelledWorkerCompletion) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest first;
  first.configuration = display_device::SingleDisplayConfiguration {};
  first.configuration->m_device_id = "FIRST";
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {first, harness.cancellation.current_generation()});
  ASSERT_TRUE(harness.dispatcher.apply_completion);
  ASSERT_TRUE(harness.task_manager.create_restore_task(L""));

  display_helper::v2::ApplyRequest second;
  second.configuration = display_device::SingleDisplayConfiguration {};
  second.configuration->m_device_id = "SECOND";
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {second, harness.cancellation.current_generation()});

  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 1);
  ASSERT_TRUE(harness.dispatcher.apply_request.configuration.has_value());
  EXPECT_EQ(harness.dispatcher.apply_request.configuration->m_device_id, "FIRST");

  display_helper::v2::ApplyOutcome cancelled;
  cancelled.status = display_helper::v2::ApplyStatus::Fatal;
  cancelled.durable_recovery_armed = true;
  cancelled.display_may_have_changed = true;
  harness.dispatcher.apply_completion(cancelled);
  harness.drain_messages();

  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 2);
  ASSERT_TRUE(harness.dispatcher.apply_request.configuration.has_value());
  EXPECT_EQ(harness.dispatcher.apply_request.configuration->m_device_id, "SECOND");
  EXPECT_TRUE(harness.state_machine.recovery_armed());
  EXPECT_EQ(harness.task_manager.deleted, 0);
}

TEST(DisplayHelperV2StateMachine, ApplyWaitsForCancelledRecoveryCompletion) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();
  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});
  ASSERT_TRUE(harness.dispatcher.recovery_completion);

  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  request.configuration->m_device_id = "NEXT";
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 0);

  display_helper::v2::RecoveryOutcome cancelled;
  cancelled.display_may_have_changed = true;
  harness.dispatcher.recovery_completion(cancelled);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 1);
  ASSERT_TRUE(harness.dispatcher.apply_request.configuration.has_value());
  EXPECT_EQ(harness.dispatcher.apply_request.configuration->m_device_id, "NEXT");
  EXPECT_TRUE(harness.state_machine.recovery_armed());
  EXPECT_EQ(harness.task_manager.deleted, 0);

  display_helper::v2::ApplyOutcome applied;
  applied.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(applied);
  harness.drain_messages();
  ASSERT_TRUE(harness.dispatcher.verification_completion);
  harness.dispatcher.verification_completion(true);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  ASSERT_TRUE(harness.apply_result.has_value());
  EXPECT_EQ(*harness.apply_result, display_helper::v2::ApplyStatus::Ok);
  ASSERT_TRUE(harness.verification_result.has_value());
  EXPECT_TRUE(*harness.verification_result);
}

TEST(DisplayHelperV2StateMachine, DisarmCancelsRecoveryBeforeMutation) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();
  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});
  ASSERT_TRUE(harness.dispatcher.recovery_completion);

  harness.state_machine.handle_message(display_helper::v2::DisarmCommand {harness.cancellation.current_generation()});
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);
  EXPECT_EQ(harness.task_manager.deleted, 0);

  display_helper::v2::RecoveryOutcome cancelled;
  harness.dispatcher.recovery_completion(cancelled);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  EXPECT_FALSE(harness.state_machine.recovery_armed());
  EXPECT_EQ(harness.task_manager.deleted, 1);
}

TEST(DisplayHelperV2StateMachine, DisarmDoesNotInterruptUnconfirmedRecovery) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();
  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});
  ASSERT_TRUE(harness.dispatcher.recovery_completion);
  harness.restore_state.restore_attempted_unconfirmed.store(true, std::memory_order_release);

  harness.state_machine.handle_message(display_helper::v2::DisarmCommand {harness.cancellation.current_generation()});
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);
  EXPECT_EQ(harness.task_manager.deleted, 0);

  display_helper::v2::RecoveryOutcome failed;
  failed.display_may_have_changed = true;
  harness.dispatcher.recovery_completion(failed);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::EventLoop);
  EXPECT_TRUE(harness.state_machine.recovery_armed());
  EXPECT_EQ(harness.task_manager.deleted, 0);
}

TEST(DisplayHelperV2StateMachine, ForcedDisarmSupersedesUnconfirmedRecoveryForStreamStart) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();
  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});
  ASSERT_TRUE(harness.dispatcher.recovery_completion);
  harness.restore_state.restore_attempted_unconfirmed.store(true, std::memory_order_release);

  harness.state_machine.handle_message(display_helper::v2::DisarmCommand {
    .generation = harness.cancellation.current_generation(),
    .force = true,
  });
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);

  display_helper::v2::RecoveryOutcome cancelled;
  harness.dispatcher.recovery_completion(cancelled);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  EXPECT_FALSE(harness.state_machine.recovery_armed());
  EXPECT_EQ(harness.task_manager.deleted, 1);

  // Preserve the live scenario's final contract: the replacement session's
  // APPLY is accepted and verified after forced DISARM, and the superseded
  // recovery cannot win afterward.
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {
    request,
    harness.cancellation.current_generation(),
  });
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);
  ASSERT_TRUE(harness.dispatcher.apply_completion);

  display_helper::v2::ApplyOutcome applied;
  applied.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(applied);
  harness.drain_messages();
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Verification);
  ASSERT_TRUE(harness.dispatcher.verification_completion);

  harness.dispatcher.verification_completion(true);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  ASSERT_TRUE(harness.apply_result.has_value());
  EXPECT_EQ(*harness.apply_result, display_helper::v2::ApplyStatus::Ok);
  ASSERT_TRUE(harness.verification_result.has_value());
  EXPECT_TRUE(*harness.verification_result);
  EXPECT_TRUE(harness.state_machine.recovery_armed());
}

TEST(DisplayHelperV2StateMachine, DuplicateDisconnectRevertJoinsActiveRecovery) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();
  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});
  ASSERT_TRUE(harness.dispatcher.recovery_completion);

  display_helper::v2::RevertCommand duplicate {harness.cancellation.current_generation()};
  duplicate.from_disconnect = true;
  harness.state_machine.handle_message(duplicate);
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);
  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, 1);

  display_helper::v2::RecoveryOutcome failed;
  failed.display_may_have_changed = true;
  harness.dispatcher.recovery_completion(failed);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::EventLoop);
  EXPECT_TRUE(harness.state_machine.recovery_armed());
}

TEST(DisplayHelperV2StateMachine, TerminalUncommittedApplyResetsStagedStateBeforeReplacement) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest first;
  first.configuration = display_device::SingleDisplayConfiguration {};
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {first, harness.cancellation.current_generation()});

  display_helper::v2::ApplyOutcome failed;
  failed.status = display_helper::v2::ApplyStatus::Fatal;
  failed.staged_state_prepared = true;
  harness.dispatcher.apply_completion(failed);
  harness.drain_messages();

  EXPECT_EQ(harness.dispatcher.reset_dispatch_count, 1);
  ASSERT_TRUE(harness.dispatcher.reset_completion);

  display_helper::v2::ApplyRequest replacement;
  replacement.configuration = display_device::SingleDisplayConfiguration {};
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {replacement, harness.cancellation.current_generation()});
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 1);

  harness.dispatcher.reset_completion(true);
  harness.drain_messages();
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 2);
}

TEST(DisplayHelperV2StateMachine, StagedStateFromRetryIsResetAfterTerminalFailure) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  display_helper::v2::ApplyOutcome retry;
  retry.status = display_helper::v2::ApplyStatus::Retryable;
  retry.staged_state_prepared = true;
  harness.dispatcher.apply_completion(retry);
  harness.drain_messages();
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 2);

  display_helper::v2::ApplyOutcome failed;
  failed.status = display_helper::v2::ApplyStatus::Fatal;
  harness.dispatcher.apply_completion(failed);
  harness.drain_messages();

  EXPECT_EQ(harness.dispatcher.reset_dispatch_count, 1);
  ASSERT_TRUE(harness.dispatcher.reset_completion);
  harness.dispatcher.reset_completion(true);
  harness.drain_messages();
}

TEST(DisplayHelperV2StateMachine, StaleResetCompletionReleasesApplyBarrier) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest first;
  first.configuration = display_device::SingleDisplayConfiguration {};
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {first, harness.cancellation.current_generation()});

  display_helper::v2::ApplyOutcome failed;
  failed.status = display_helper::v2::ApplyStatus::Fatal;
  failed.staged_state_prepared = true;
  harness.dispatcher.apply_completion(failed);
  harness.drain_messages();
  ASSERT_TRUE(harness.dispatcher.reset_completion);

  display_helper::v2::ApplyRequest replacement;
  replacement.configuration = display_device::SingleDisplayConfiguration {};
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {replacement, harness.cancellation.current_generation()});
  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 1);

  // A cancellation can arrive while the non-cancellable backend reset is
  // queued. Its completion must still retire the barrier and release APPLY.
  harness.cancellation.cancel();
  harness.dispatcher.reset_completion(true);
  harness.drain_messages();

  EXPECT_EQ(harness.dispatcher.apply_dispatch_count, 2);
}

TEST(DisplayHelperV2StateMachine, ExportGoldenCapturesSnapshotWithBlacklist) {
  StateMachineHarness harness;
  harness.add_active_device("A");
  harness.add_active_device("B");

  display_device::DisplaySettingsSnapshot snapshot;
  snapshot.m_topology = {{"A", "B"}};
  snapshot.m_modes["A"] = display_device::DisplayMode {};
  snapshot.m_modes["B"] = display_device::DisplayMode {};
  snapshot.m_hdr_states["A"] = std::nullopt;
  snapshot.m_hdr_states["B"] = std::nullopt;
  harness.display_settings.snapshot = snapshot;

  display_helper::v2::SnapshotCommandPayload payload;
  payload.exclude_devices = {"B"};
  payload.update_exclusions = true;
  harness.state_machine.handle_message(display_helper::v2::ExportGoldenCommand {payload, harness.cancellation.current_generation()});

  auto stored = harness.storage.load(display_helper::v2::SnapshotTier::Golden);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->m_topology.size(), 1u);
  ASSERT_EQ(stored->m_topology.front().size(), 1u);
  EXPECT_EQ(stored->m_topology.front().front(), "A");
  EXPECT_EQ(stored->m_modes.count("B"), 0u);
}

TEST(DisplayHelperV2StateMachine, SnapshotCurrentRefreshesPreservingPrevious) {
  StateMachineHarness harness;
  harness.add_active_device("new");

  auto previous = make_snapshot("old");
  ASSERT_TRUE(harness.storage.save(display_helper::v2::SnapshotTier::Current, previous));

  auto current = make_snapshot("new");
  harness.display_settings.snapshot = current;

  display_helper::v2::SnapshotCommandPayload payload;
  harness.state_machine.handle_message(display_helper::v2::SnapshotCurrentCommand {payload, harness.cancellation.current_generation()});

  auto stored_prev = harness.storage.load(display_helper::v2::SnapshotTier::Previous);
  ASSERT_TRUE(stored_prev.has_value());
  EXPECT_TRUE(display_helper::v2::topology::equal_snapshot(*stored_prev, previous));

  auto stored_current = harness.storage.load(display_helper::v2::SnapshotTier::Current);
  ASSERT_TRUE(stored_current.has_value());
  EXPECT_TRUE(display_helper::v2::topology::equal_snapshot(*stored_current, current));
  ASSERT_TRUE(harness.snapshot_result.has_value());
  EXPECT_TRUE(*harness.snapshot_result);
}

// f3841ad8: APPLY with no session baseline on disk must self-capture the
// pre-apply state so REVERT always has something to restore.
TEST(DisplayHelperV2StateMachine, ApplySelfCapturesBaselineWhenMissing) {
  StateMachineHarness harness;
  harness.add_active_device("PHYS");
  harness.display_settings.snapshot = make_snapshot("PHYS");

  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};

  EXPECT_FALSE(harness.storage.exists(display_helper::v2::SnapshotTier::Current));
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  auto baseline = harness.storage.load(display_helper::v2::SnapshotTier::Current);
  ASSERT_TRUE(baseline.has_value());
  EXPECT_EQ(baseline->m_topology.front().front(), "PHYS");
}

TEST(DisplayHelperV2StateMachine, ApplyKeepsExistingBaseline) {
  StateMachineHarness harness;
  harness.add_active_device("PHYS");
  harness.display_settings.snapshot = make_snapshot("PHYS");

  auto existing = make_snapshot("EXISTING");
  ASSERT_TRUE(harness.storage.save(display_helper::v2::SnapshotTier::Current, existing));

  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  auto baseline = harness.storage.load(display_helper::v2::SnapshotTier::Current);
  ASSERT_TRUE(baseline.has_value());
  EXPECT_TRUE(display_helper::v2::topology::equal_snapshot(*baseline, existing));
}

// 72b0d996: stream-start probes must not cancel an in-flight unconfirmed restore.
TEST(DisplayHelperV2StateMachine, DisarmIgnoredWhileRestoreUnconfirmed) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();

  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});
  display_helper::v2::RecoveryOutcome recovery_fail;
  recovery_fail.success = false;
  harness.dispatcher.recovery_completion(recovery_fail);
  harness.drain_messages();
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::EventLoop);

  // A real RecoveryOperation marks the attempt unconfirmed; simulate that here.
  harness.restore_state.restore_attempted_unconfirmed.store(true);

  harness.state_machine.handle_message(display_helper::v2::DisarmCommand {harness.cancellation.current_generation()});
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::EventLoop);
  EXPECT_TRUE(harness.state_machine.recovery_armed());
  EXPECT_EQ(harness.task_manager.deleted, 0);
}

// 72b0d996: SNAPSHOT_CURRENT must not overwrite the baseline mid-restore.
TEST(DisplayHelperV2StateMachine, SnapshotCurrentSkippedWhileRestorePending) {
  StateMachineHarness harness;
  harness.add_active_device("new");
  harness.seed_current_snapshot("old");

  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});
  display_helper::v2::RecoveryOutcome recovery_fail;
  recovery_fail.success = false;
  harness.dispatcher.recovery_completion(recovery_fail);
  harness.drain_messages();
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::EventLoop);

  harness.display_settings.snapshot = make_snapshot("new");
  display_helper::v2::SnapshotCommandPayload payload;
  harness.state_machine.handle_message(display_helper::v2::SnapshotCurrentCommand {payload, harness.cancellation.current_generation()});

  auto stored_current = harness.storage.load(display_helper::v2::SnapshotTier::Current);
  ASSERT_TRUE(stored_current.has_value());
  EXPECT_EQ(stored_current->m_topology.front().front(), "old");
  ASSERT_TRUE(harness.snapshot_result.has_value());
  EXPECT_FALSE(*harness.snapshot_result);
}

TEST(DisplayHelperV2StateMachine, SnapshotCurrentSkippedWhileApplyIsInFlight) {
  StateMachineHarness harness;
  harness.add_active_device("new");
  harness.seed_current_snapshot("old");

  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  harness.display_settings.snapshot = make_snapshot("new");
  display_helper::v2::SnapshotCommandPayload payload;
  harness.state_machine.handle_message(display_helper::v2::SnapshotCurrentCommand {payload, harness.cancellation.current_generation()});

  auto stored_current = harness.storage.load(display_helper::v2::SnapshotTier::Current);
  ASSERT_TRUE(stored_current.has_value());
  EXPECT_EQ(stored_current->m_topology.front().front(), "old");
  ASSERT_TRUE(harness.snapshot_result.has_value());
  EXPECT_FALSE(*harness.snapshot_result);
}

TEST(DisplayHelperV2StateMachine, ResetSerializesBehindApplyWithoutCancellingSession) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  harness.state_machine.handle_message(display_helper::v2::ResetCommand {harness.cancellation.current_generation()});

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::InProgress);
  EXPECT_EQ(harness.dispatcher.reset_dispatch_count, 0);

  display_helper::v2::ApplyOutcome applied;
  applied.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(applied);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Verification);
  EXPECT_EQ(harness.dispatcher.reset_dispatch_count, 1);
  EXPECT_EQ(harness.task_manager.deleted, 0);
  ASSERT_TRUE(harness.dispatcher.reset_completion);
  harness.dispatcher.reset_completion(true);
  harness.drain_messages();

  ASSERT_TRUE(harness.dispatcher.verification_completion);
  harness.dispatcher.verification_completion(true);
  harness.drain_messages();
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  EXPECT_TRUE(harness.state_machine.recovery_armed());
  EXPECT_EQ(harness.task_manager.deleted, 0);
}

TEST(DisplayHelperV2StateMachine, ResetDuringRecoveryDoesNotCancelOrDeleteRecoveryGuard) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();
  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});
  ASSERT_TRUE(harness.dispatcher.recovery_completion);

  harness.state_machine.handle_message(display_helper::v2::ResetCommand {harness.cancellation.current_generation()});
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);
  EXPECT_EQ(harness.dispatcher.reset_dispatch_count, 0);
  EXPECT_EQ(harness.task_manager.deleted, 0);

  display_helper::v2::RecoveryOutcome failed;
  harness.dispatcher.recovery_completion(failed);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::EventLoop);
  EXPECT_TRUE(harness.state_machine.recovery_armed());
  EXPECT_EQ(harness.task_manager.deleted, 0);
  EXPECT_EQ(harness.dispatcher.reset_dispatch_count, 1);
  ASSERT_TRUE(harness.dispatcher.reset_completion);
  harness.dispatcher.reset_completion(true);
  harness.drain_messages();
}

TEST(DisplayHelperV2StateMachine, RecoveryExitWaitsForQueuedResetCompletion) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();
  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});
  ASSERT_TRUE(harness.dispatcher.recovery_completion);

  harness.state_machine.handle_message(display_helper::v2::ResetCommand {harness.cancellation.current_generation()});
  display_helper::v2::RecoveryOutcome recovered;
  recovered.success = true;
  recovered.snapshot = make_snapshot("A");
  recovered.display_may_have_changed = true;
  harness.dispatcher.recovery_completion(recovered);
  harness.drain_messages();
  ASSERT_TRUE(harness.dispatcher.recovery_validation_completion);

  harness.dispatcher.recovery_validation_completion(true);
  harness.drain_messages();

  EXPECT_EQ(harness.dispatcher.reset_dispatch_count, 1);
  EXPECT_FALSE(harness.exit_code.has_value());
  ASSERT_TRUE(harness.dispatcher.reset_completion);
  harness.dispatcher.reset_completion(true);
  harness.drain_messages();

  ASSERT_TRUE(harness.exit_code.has_value());
  EXPECT_EQ(*harness.exit_code, 0);
}

TEST(DisplayHelperV2StateMachine, FailedRecoveryStagedResetRetriesBeforeReusingHelper) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();

  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});
  display_helper::v2::ApplyOutcome applied;
  applied.status = display_helper::v2::ApplyStatus::Ok;
  applied.staged_state_prepared = true;
  harness.dispatcher.apply_completion(applied);
  harness.drain_messages();
  ASSERT_TRUE(harness.dispatcher.verification_completion);
  harness.dispatcher.verification_completion(true);
  harness.drain_messages();

  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});
  display_helper::v2::RecoveryOutcome recovered;
  recovered.success = true;
  recovered.snapshot = make_snapshot("A");
  recovered.staged_state_reset_attempted = true;
  recovered.staged_state_reset_succeeded = false;
  harness.dispatcher.recovery_completion(recovered);
  harness.drain_messages();
  ASSERT_TRUE(harness.dispatcher.recovery_validation_completion);
  harness.dispatcher.recovery_validation_completion(true);
  harness.drain_messages();

  EXPECT_EQ(harness.dispatcher.reset_dispatch_count, 1);
  EXPECT_FALSE(harness.exit_code.has_value());
  ASSERT_TRUE(harness.dispatcher.reset_completion);
  harness.dispatcher.reset_completion(false);
  harness.drain_messages();

  ASSERT_TRUE(harness.exit_code.has_value());
  EXPECT_EQ(*harness.exit_code, 1);
}

TEST(DisplayHelperV2StateMachine, ApplyPublishesDisconnectPolicyBeforeWorkerCompletes) {
  StateMachineHarness harness;
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  request.restore_on_disconnect = false;

  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  EXPECT_FALSE(harness.restore_state.restore_on_disconnect.load(std::memory_order_acquire));
}

// 3b7a52c4 / 0add1f80: paused sessions with revert_on_disconnect=false must keep
// their display state when the connection drops.
TEST(DisplayHelperV2StateMachine, NoRevertOnDisconnectWhenPolicyFalse) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();

  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  request.restore_on_disconnect = false;
  harness.state_machine.handle_message(display_helper::v2::ApplyCommand {request, harness.cancellation.current_generation()});

  display_helper::v2::ApplyOutcome applied;
  applied.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(applied);
  harness.drain_messages();
  ASSERT_TRUE(harness.dispatcher.verification_completion);
  harness.dispatcher.verification_completion(true);
  harness.drain_messages();
  ASSERT_TRUE(harness.state_machine.recovery_armed());

  display_helper::v2::RevertCommand disconnect_revert {harness.cancellation.current_generation()};
  disconnect_revert.from_disconnect = true;
  harness.state_machine.handle_message(disconnect_revert);

  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, 0);
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  EXPECT_EQ(harness.task_manager.deleted, 1);

  // An explicit client REVERT still restores.
  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});
  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, 1);
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);
}

TEST(DisplayHelperV2StateMachine, ExplicitRevertSurvivesLaterDisconnectWhenPolicyFalse) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();
  harness.restore_state.restore_on_disconnect.store(false, std::memory_order_release);

  harness.state_machine.handle_message(display_helper::v2::RevertCommand {
    harness.cancellation.current_generation(),
  });
  ASSERT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);
  ASSERT_TRUE(harness.dispatcher.recovery_completion);

  display_helper::v2::RecoveryOutcome failed;
  failed.success = false;
  harness.dispatcher.recovery_completion(failed);
  harness.drain_messages();
  ASSERT_EQ(harness.state_machine.state(), display_helper::v2::State::EventLoop);

  display_helper::v2::RevertCommand disconnect {harness.cancellation.current_generation()};
  disconnect.from_disconnect = true;
  harness.state_machine.handle_message(disconnect);

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);
  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, 2);
}

// Restore retry pacing: failed attempts back off, display events reset the
// backoff, and an exhausted window pauses attempts until the next event.
TEST(DisplayHelperV2StateMachine, TickDrivesScheduledRestoreRetries) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();

  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});
  display_helper::v2::RecoveryOutcome recovery_fail;
  recovery_fail.success = false;
  harness.dispatcher.recovery_completion(recovery_fail);
  harness.drain_messages();
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::EventLoop);
  const int dispatches_after_first = harness.dispatcher.recovery_dispatch_count;

  // Backoff (1s after first failure): an immediate tick must not retry.
  harness.state_machine.handle_tick();
  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, dispatches_after_first);

  // After the backoff elapses, the tick retries.
  harness.clock.advance(std::chrono::milliseconds(1100));
  harness.state_machine.handle_tick();
  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, dispatches_after_first + 1);
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);

  // Fail again, then exhaust the primary window: ticks stop retrying.
  harness.dispatcher.recovery_completion(recovery_fail);
  harness.drain_messages();
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::EventLoop);
  harness.clock.advance(std::chrono::minutes(3));
  harness.state_machine.handle_tick();
  harness.state_machine.handle_tick();
  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, dispatches_after_first + 1);

  // A display event re-opens an event window and retries immediately.
  harness.state_machine.handle_message(display_helper::v2::DisplayEventMessage {
    display_helper::v2::DisplayEvent::DisplayChange,
    harness.cancellation.current_generation()});
  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, dispatches_after_first + 2);
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);
}

TEST(DisplayHelperV2StateMachine, DisconnectRearmsAnExhaustedRestoreWindow) {
  StateMachineHarness harness;
  harness.seed_current_snapshot();

  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});
  display_helper::v2::RecoveryOutcome failed;
  harness.dispatcher.recovery_completion(failed);
  harness.drain_messages();
  ASSERT_EQ(harness.state_machine.state(), display_helper::v2::State::EventLoop);
  ASSERT_EQ(harness.dispatcher.recovery_dispatch_count, 1);

  harness.clock.advance(std::chrono::minutes(3));
  harness.state_machine.handle_tick();
  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, 1);

  display_helper::v2::RevertCommand disconnect {harness.cancellation.current_generation()};
  disconnect.from_disconnect = true;
  harness.state_machine.handle_message(disconnect);

  EXPECT_EQ(harness.dispatcher.recovery_dispatch_count, 2);
  EXPECT_EQ(harness.dispatcher.recovery_delay, std::chrono::milliseconds(5000));
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);
}

TEST(DisplayHelperV2StateMachine, SnapshotCurrentRefreshFailureKeepsBaseline) {
  StateMachineHarness harness;
  // No enumerated devices: the capture filter rejects the new snapshot, so the
  // existing baseline chain must remain untouched (53cd8b4c / 62839421).

  auto previous = make_snapshot("old");
  ASSERT_TRUE(harness.storage.save(display_helper::v2::SnapshotTier::Current, previous));
  harness.display_settings.snapshot = make_snapshot("new");

  display_helper::v2::SnapshotCommandPayload payload;
  harness.state_machine.handle_message(display_helper::v2::SnapshotCurrentCommand {payload, harness.cancellation.current_generation()});

  auto stored_current = harness.storage.load(display_helper::v2::SnapshotTier::Current);
  ASSERT_TRUE(stored_current.has_value());
  EXPECT_TRUE(display_helper::v2::topology::equal_snapshot(*stored_current, previous));
  EXPECT_FALSE(harness.storage.load(display_helper::v2::SnapshotTier::Previous).has_value());
  ASSERT_TRUE(harness.snapshot_result.has_value());
  EXPECT_FALSE(*harness.snapshot_result);
}

#endif  // _WIN32
