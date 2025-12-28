/**
 * @file tests/unit/test_display_helper_v2_state_machine.cpp
 * @brief Unit tests for display helper v2 state machine and operations.
 */
#include "../tests_common.h"

#include "src/platform/windows/display_helper_v2/async_dispatcher.h"
#include "src/platform/windows/display_helper_v2/operations.h"
#include "src/platform/windows/display_helper_v2/runtime_support.h"
#include "src/platform/windows/display_helper_v2/snapshot.h"
#include "src/platform/windows/display_helper_v2/state_machine.h"

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
      const display_helper::v2::CancellationToken &,
      std::function<void(bool)> completion) override {
      verification_request = request;
      verification_topology = expected_topology;
      verification_completion = std::move(completion);
      verification_dispatch_count += 1;
    }

    void dispatch_recovery(
      const display_helper::v2::CancellationToken &,
      std::function<void(const display_helper::v2::RecoveryOutcome &)> completion) override {
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

    display_helper::v2::ApplyRequest apply_request;
    std::chrono::milliseconds apply_delay {0};
    bool apply_reset_virtual_display = false;
    int apply_dispatch_count = 0;
    std::function<void(const display_helper::v2::ApplyOutcome &)> apply_completion;

    display_helper::v2::ApplyRequest verification_request;
    std::optional<display_helper::v2::ActiveTopology> verification_topology;
    int verification_dispatch_count = 0;
    std::function<void(bool)> verification_completion;

    int recovery_dispatch_count = 0;
    std::function<void(const display_helper::v2::RecoveryOutcome &)> recovery_completion;

    display_helper::v2::Snapshot recovery_validation_snapshot;
    int recovery_validation_dispatch_count = 0;
    std::function<void(bool)> recovery_validation_completion;
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
      return "virtual";
    }

    bool available = true;
    bool disabled = false;
    bool enabled = false;
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
    std::deque<display_helper::v2::Message> messages;
    std::optional<display_helper::v2::ApplyStatus> apply_result;
    std::optional<bool> verification_result;
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
    display_helper::v2::SnapshotLedger snapshot_ledger {snapshot_service, snapshot_persistence};

    display_helper::v2::StateMachine state_machine {
      apply_pipeline,
      recovery_pipeline,
      snapshot_ledger,
      system_ports
    };

    StateMachineHarness() {
      state_machine.set_apply_result_callback([this](display_helper::v2::ApplyStatus status) {
        apply_result = status;
      });
      state_machine.set_verification_result_callback([this](bool success) {
        verification_result = success;
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

TEST(DisplayHelperV2Heartbeat, TriggersTimeoutWhenArmed) {
  FakeClock clock;
  display_helper::v2::HeartbeatMonitor monitor(clock);

  monitor.arm();
  clock.advance(std::chrono::seconds(31));
  EXPECT_TRUE(monitor.check_timeout());
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
  EXPECT_EQ(harness.task_manager.created, 1);
  ASSERT_TRUE(harness.dispatcher.apply_completion);

  display_helper::v2::ApplyOutcome outcome;
  outcome.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(outcome);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Verification);
  ASSERT_TRUE(harness.dispatcher.verification_completion);

  harness.dispatcher.verification_completion(true);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  ASSERT_TRUE(harness.verification_result.has_value());
  EXPECT_TRUE(harness.verification_result.value());
  EXPECT_TRUE(harness.state_machine.recovery_armed());
  EXPECT_EQ(harness.workarounds.refresh_calls, 1);
  EXPECT_EQ(harness.workarounds.blank_calls, 1);
  EXPECT_EQ(harness.workarounds.last_delay, std::chrono::milliseconds(1000));
  ASSERT_TRUE(harness.apply_result.has_value());
  EXPECT_EQ(harness.apply_result, display_helper::v2::ApplyStatus::Ok);
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
  EXPECT_EQ(harness.dispatcher.apply_delay, std::chrono::milliseconds(300));
  EXPECT_FALSE(harness.apply_result.has_value());
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

TEST(DisplayHelperV2StateMachine, RevertRunsRecoveryAndValidation) {
  StateMachineHarness harness;

  harness.state_machine.handle_message(display_helper::v2::RevertCommand {harness.cancellation.current_generation()});
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Recovery);
  EXPECT_EQ(harness.task_manager.deleted, 1);

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
}

TEST(DisplayHelperV2StateMachine, RecoveryFailureKeepsEventLoopArmed) {
  StateMachineHarness harness;

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
}

TEST(DisplayHelperV2StateMachine, EventLoopTriggersRecovery) {
  StateMachineHarness harness;
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
  EXPECT_EQ(harness.task_manager.deleted, 2);

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
  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
  EXPECT_EQ(harness.task_manager.deleted, 1);

  display_helper::v2::ApplyOutcome outcome;
  outcome.status = display_helper::v2::ApplyStatus::Ok;
  harness.dispatcher.apply_completion(outcome);
  harness.drain_messages();

  EXPECT_EQ(harness.state_machine.state(), display_helper::v2::State::Waiting);
}

TEST(DisplayHelperV2StateMachine, ExportGoldenCapturesSnapshotWithBlacklist) {
  StateMachineHarness harness;

  display_device::DisplaySettingsSnapshot snapshot;
  snapshot.m_topology = {{"A", "B"}};
  snapshot.m_modes["A"] = display_device::DisplayMode {};
  snapshot.m_modes["B"] = display_device::DisplayMode {};
  snapshot.m_hdr_states["A"] = std::nullopt;
  snapshot.m_hdr_states["B"] = std::nullopt;
  harness.display_settings.snapshot = snapshot;

  display_helper::v2::SnapshotCommandPayload payload;
  payload.exclude_devices = {"B"};
  harness.state_machine.handle_message(display_helper::v2::ExportGoldenCommand {payload, harness.cancellation.current_generation()});

  auto stored = harness.storage.load(display_helper::v2::SnapshotTier::Golden);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->m_topology.size(), 1u);
  ASSERT_EQ(stored->m_topology.front().size(), 1u);
  EXPECT_EQ(stored->m_topology.front().front(), "A");
  EXPECT_EQ(stored->m_modes.count("B"), 0u);
}

TEST(DisplayHelperV2StateMachine, SnapshotCurrentRotatesAndCaptures) {
  StateMachineHarness harness;

  auto previous = make_snapshot("old");
  ASSERT_TRUE(harness.storage.save(display_helper::v2::SnapshotTier::Current, previous));

  auto current = make_snapshot("new");
  harness.display_settings.snapshot = current;

  display_helper::v2::SnapshotCommandPayload payload;
  harness.state_machine.handle_message(display_helper::v2::SnapshotCurrentCommand {payload, harness.cancellation.current_generation()});

  auto stored_prev = harness.storage.load(display_helper::v2::SnapshotTier::Previous);
  ASSERT_TRUE(stored_prev.has_value());
  EXPECT_EQ(*stored_prev, previous);

  auto stored_current = harness.storage.load(display_helper::v2::SnapshotTier::Current);
  ASSERT_TRUE(stored_current.has_value());
  EXPECT_EQ(*stored_current, current);
}

TEST(DisplayHelperV2RecoveryOperation, OrdersAndRetriesSnapshots) {
  FakeClock clock;
  FakeDisplaySettings display;
  display.valid_topology_ids = {"previous", "golden"};
  display.apply_snapshot_ids = {"previous", "golden"};
  display.match_sequence["previous"] = {false, true};
  display.match_sequence["golden"] = {true};

  display_helper::v2::SnapshotService snapshot_service(display);
  display_helper::v2::InMemorySnapshotStorage storage;
  display_helper::v2::SnapshotPersistence persistence(storage);
  display_helper::v2::ApplyPolicy policy(clock);

  storage.save(display_helper::v2::SnapshotTier::Current, make_snapshot("current"));
  storage.save(display_helper::v2::SnapshotTier::Previous, make_snapshot("previous"));
  storage.save(display_helper::v2::SnapshotTier::Golden, make_snapshot("golden"));

  display.devices.push_back({"previous", "Display", "", std::nullopt});
  display.devices.push_back({"golden", "Display", "", std::nullopt});

  display_helper::v2::RecoveryOperation recovery(display, snapshot_service, persistence, policy, clock);
  display_helper::v2::CancellationSource cancel;

  auto outcome = recovery.run(cancel.token());
  EXPECT_TRUE(outcome.success);
  ASSERT_TRUE(outcome.snapshot.has_value());
  EXPECT_EQ(outcome.snapshot->m_topology.front().front(), "previous");
}

TEST(DisplayHelperV2RecoveryOperation, CancelsDuringRetryDelay) {
  class CancellingClock final : public display_helper::v2::IClock {
  public:
    explicit CancellingClock(display_helper::v2::CancellationSource &source)
      : source_(source) {}

    std::chrono::steady_clock::time_point now() override {
      return now_;
    }

    void sleep_for(std::chrono::milliseconds duration) override {
      now_ += duration;
      sleep_calls += 1;
      if (sleep_calls == 1) {
        source_.cancel();
      }
    }

    int sleep_calls = 0;

  private:
    display_helper::v2::CancellationSource &source_;
    std::chrono::steady_clock::time_point now_ {std::chrono::steady_clock::now()};
  };

  FakeDisplaySettings display;
  display.valid_topology_ids = {"A"};
  display.devices.push_back({"A", "Display", "", std::nullopt});

  display_helper::v2::SnapshotService snapshot_service(display);
  display_helper::v2::InMemorySnapshotStorage storage;
  display_helper::v2::SnapshotPersistence persistence(storage);

  display_helper::v2::CancellationSource cancel;
  CancellingClock clock(cancel);
  display_helper::v2::ApplyPolicy policy(clock);

  storage.save(display_helper::v2::SnapshotTier::Current, make_snapshot("A"));

  display_helper::v2::RecoveryOperation recovery(display, snapshot_service, persistence, policy, clock);
  auto outcome = recovery.run(cancel.token());

  EXPECT_FALSE(outcome.success);
  EXPECT_FALSE(outcome.snapshot.has_value());
  EXPECT_EQ(display.apply_snapshot_calls, 1);
  EXPECT_EQ(clock.sleep_calls, 1);
}
