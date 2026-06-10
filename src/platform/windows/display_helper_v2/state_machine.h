#pragma once

#include "src/platform/windows/display_helper_v2/async_dispatcher.h"
#include "src/platform/windows/display_helper_v2/interfaces.h"
#include "src/platform/windows/display_helper_v2/operations.h"
#include "src/platform/windows/display_helper_v2/runtime_support.h"
#include "src/platform/windows/display_helper_v2/snapshot.h"
#include "src/platform/windows/display_helper_v2/types.h"

#include <chrono>
#include <functional>
#include <optional>
#include <set>

namespace display_helper::v2 {
  class SystemPorts {
  public:
    SystemPorts(
      IPlatformWorkarounds &workarounds,
      IScheduledTaskManager &task_manager,
      HeartbeatMonitor &heartbeat,
      IClock &clock,
      CancellationSource &cancellation)
      : workarounds_(workarounds),
        task_manager_(task_manager),
        heartbeat_(heartbeat),
        clock_(clock),
        cancellation_(cancellation) {}

    std::chrono::steady_clock::time_point now() const {
      return clock_.now();
    }

    std::uint64_t current_generation() const {
      return cancellation_.current_generation();
    }

    CancellationToken token() const {
      return cancellation_.token();
    }

    void cancel_operations() {
      cancellation_.cancel();
    }

    void arm_heartbeat() {
      heartbeat_.arm();
    }

    void disarm_heartbeat() {
      heartbeat_.disarm();
    }

    void record_ping() {
      heartbeat_.record_ping();
    }

    void refresh_shell() {
      workarounds_.refresh_shell();
    }

    void blank_hdr_states(std::chrono::milliseconds delay) {
      workarounds_.blank_hdr_states(delay);
    }

    void create_restore_task() {
      (void) task_manager_.create_restore_task(L"");
    }

    void delete_restore_task() {
      (void) task_manager_.delete_restore_task();
    }

  private:
    IPlatformWorkarounds &workarounds_;
    IScheduledTaskManager &task_manager_;
    HeartbeatMonitor &heartbeat_;
    IClock &clock_;
    CancellationSource &cancellation_;
  };

  class ApplyPipeline {
  public:
    ApplyPipeline(
      IAsyncDispatcher &dispatcher,
      ApplyPolicy &policy,
      SystemPorts &system,
      std::function<void(Message)> enqueue)
      : dispatcher_(dispatcher),
        policy_(policy),
        system_(system),
        enqueue_(std::move(enqueue)) {}

    PolicyDecision maybe_reset_virtual_display(ApplyStatus status, bool virtual_display_requested) const {
      return policy_.maybe_reset_virtual_display(status, virtual_display_requested);
    }

    bool can_retry(int attempt) const {
      return policy_.can_retry_apply(attempt);
    }

    std::chrono::milliseconds retry_delay(int attempt) const {
      return policy_.retry_delay(attempt);
    }

    void dispatch_apply(const ApplyRequest &request, std::chrono::milliseconds delay, bool reset_virtual_display) {
      const auto token = system_.token();
      const auto generation = token.generation();

      dispatcher_.dispatch_apply(
        request,
        token,
        delay,
        reset_virtual_display,
        [enqueue = enqueue_, generation](const ApplyOutcome &outcome) {
          ApplyCompleted completed;
          completed.status = outcome.status;
          completed.expected_topology = outcome.expected_topology;
          completed.virtual_display_requested = outcome.virtual_display_requested;
          completed.generation = generation;
          enqueue(completed);
        });
    }

    void dispatch_verification(
      const ApplyRequest &request,
      const std::optional<ActiveTopology> &expected_topology) {
      const auto token = system_.token();
      const auto generation = token.generation();

      dispatcher_.dispatch_verification(
        request,
        expected_topology,
        token,
        [enqueue = enqueue_, generation](bool success) {
          VerificationCompleted completed;
          completed.success = success;
          completed.generation = generation;
          enqueue(completed);
        });
    }

  private:
    IAsyncDispatcher &dispatcher_;
    ApplyPolicy &policy_;
    SystemPorts &system_;
    std::function<void(Message)> enqueue_;
  };

  class RecoveryPipeline {
  public:
    RecoveryPipeline(
      IAsyncDispatcher &dispatcher,
      SystemPorts &system,
      std::function<void(Message)> enqueue)
      : dispatcher_(dispatcher),
        system_(system),
        enqueue_(std::move(enqueue)) {}

    void dispatch_recovery(std::chrono::milliseconds delay = std::chrono::milliseconds(0)) {
      const auto token = system_.token();
      const auto generation = token.generation();

      dispatcher_.dispatch_recovery(
        token,
        delay,
        [enqueue = enqueue_, generation](const RecoveryOutcome &outcome) {
          RecoveryCompleted completed;
          completed.success = outcome.success;
          completed.snapshot = outcome.snapshot;
          completed.generation = generation;
          enqueue(completed);
        });
    }

    void dispatch_recovery_validation(const Snapshot &snapshot) {
      const auto token = system_.token();
      const auto generation = token.generation();

      dispatcher_.dispatch_recovery_validation(
        snapshot,
        token,
        [enqueue = enqueue_, generation](bool success) {
          RecoveryValidationCompleted completed;
          completed.success = success;
          completed.generation = generation;
          enqueue(completed);
        });
    }

  private:
    IAsyncDispatcher &dispatcher_;
    SystemPorts &system_;
    std::function<void(Message)> enqueue_;
  };

  class SnapshotLedger {
  public:
    SnapshotLedger(SnapshotService &service, SnapshotPersistence &persistence, IClock &clock)
      : service_(service),
        persistence_(persistence),
        clock_(clock) {}

    void set_prefer_golden_first(bool prefer) {
      persistence_.set_prefer_golden_first(prefer);
    }

    Snapshot capture() {
      return service_.capture();
    }

    bool save(SnapshotTier tier, Snapshot snapshot, const std::set<std::string> &blacklist) {
      return persistence_.save(tier, std::move(snapshot), blacklist);
    }

    bool rotate_current_to_previous() {
      return persistence_.rotate_current_to_previous();
    }

    bool tier_exists(SnapshotTier tier) {
      return persistence_.storage().exists(tier);
    }

    /**
     * @brief Legacy capture pipeline: capture, gate on topology validity, filter
     *        (active virtual displays, exclusions, display_name-less devices),
     *        attach layout rotations, then save with retries.
     */
    bool capture_filtered_and_save(SnapshotTier tier, const std::vector<std::string> &exclusions, const char *reason);

    /**
     * @brief Legacy refresh_current_snapshot_preserving_previous: capture/filter
     *        first; only rotate current->previous once the new capture is known
     *        good, so a failed capture never destroys the existing baseline.
     */
    bool refresh_current_preserving_previous(const std::vector<std::string> &exclusions);

  private:
    std::optional<std::pair<Snapshot, codec::layout_rotation_map_t>> capture_filtered(const std::vector<std::string> &exclusions, const char *reason);

    SnapshotService &service_;
    SnapshotPersistence &persistence_;
    IClock &clock_;
  };

  struct StateTransition {
    State from;
    State to;
    ApplyAction trigger;
    std::optional<ApplyStatus> result_status;
    std::chrono::steady_clock::time_point timestamp;
  };

  using StateObserver = std::function<void(const StateTransition &)>;

  class StateMachine {
  public:
    StateMachine(
      ApplyPipeline &apply,
      RecoveryPipeline &recovery,
      SnapshotLedger &snapshots,
      SystemPorts &system,
      IVirtualDisplayDriver &virtual_display,
      GoldenHealth &golden_health,
      RestoreState &restore_state);

    void set_state_observer(StateObserver observer);
    void set_apply_result_callback(std::function<void(ApplyStatus)> callback);
    void set_verification_result_callback(std::function<void(bool)> callback);
    void set_exit_callback(std::function<void(int)> callback);
    void set_snapshot_blacklist(std::set<std::string> blacklist);

    void handle_message(const Message &message);

    /// Periodic driver for restore window/backoff scheduling (legacy restore_poll_proc).
    /// Call from the helper main loop every ~100ms.
    void handle_tick();

    State state() const;
    bool recovery_armed() const;

    /// True while a restore request is being worked on (Recovery/RecoveryValidation/EventLoop).
    bool restore_pending() const;

  private:
    void retarget_virtual_display_device_id_if_needed();

    void handle_apply_command(const ApplyCommand &command);
    void handle_revert_command(const RevertCommand &command);
    void handle_disarm_command(const DisarmCommand &command);
    void handle_export_golden(const ExportGoldenCommand &command);
    void handle_snapshot_current(const SnapshotCurrentCommand &command);
    void handle_reset_command(const ResetCommand &command);
    void handle_ping_command(const PingCommand &command);
    void handle_stop_command(const StopCommand &command);
    void handle_apply_completed(const ApplyCompleted &completed);
    void handle_verification_completed(const VerificationCompleted &completed);
    void handle_recovery_completed(const RecoveryCompleted &completed);
    void handle_recovery_validation_completed(const RecoveryValidationCompleted &completed);
    void handle_display_event(const DisplayEventMessage &event);
    void handle_helper_event(const HelperEventMessage &event);

    void transition(State next, ApplyAction trigger, std::optional<ApplyStatus> status = std::nullopt);
    bool is_stale(std::uint64_t generation) const;

    std::vector<std::string> exclusions_vector() const;
    void update_blacklist(const std::vector<std::string> &exclude_devices);
    void start_recovery(std::chrono::milliseconds delay, ApplyAction trigger);

    ApplyPipeline &apply_;
    RecoveryPipeline &recovery_;
    SnapshotLedger &snapshots_;
    SystemPorts &system_;
    IVirtualDisplayDriver &virtual_display_;
    GoldenHealth &golden_health_;
    RestoreState &restore_state_;
    RestoreScheduler scheduler_;

    State state_ = State::Waiting;
    bool recovery_armed_ = false;
    int apply_attempt_ = 0;
    bool apply_result_sent_ = false;
    ApplyRequest current_request_ {};
    std::optional<ActiveTopology> expected_topology_;
    std::optional<Snapshot> recovery_snapshot_;
    std::set<std::string> snapshot_blacklist_;

    std::chrono::steady_clock::time_point last_virtual_apply_display_event_restart_ {};

    StateObserver observer_;
    std::function<void(ApplyStatus)> apply_result_callback_;
    std::function<void(bool)> verification_result_callback_;
    std::function<void(int)> exit_callback_;
  };
}  // namespace display_helper::v2
