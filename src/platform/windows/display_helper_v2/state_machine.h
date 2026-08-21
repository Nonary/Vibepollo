#pragma once

#include "src/platform/windows/display_helper_v2/async_dispatcher.h"
#include "src/platform/windows/display_helper_v2/interfaces.h"
#include "src/platform/windows/display_helper_v2/operations.h"
#include "src/platform/windows/display_helper_v2/runtime_support.h"
#include "src/platform/windows/display_helper_v2/snapshot.h"
#include "src/platform/windows/display_helper_v2/types.h"

#include <chrono>
#include <deque>
#include <functional>
#include <optional>
#include <set>
#include <variant>

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

    void clear_pending_hdr_blank() {
      workarounds_.clear_pending_hdr_blank();
    }

    bool create_restore_task() {
      return task_manager_.create_restore_task(L"");
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

    std::uint64_t dispatch_apply(const ApplyRequest &request, std::chrono::milliseconds delay, bool reset_virtual_display) {
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
          completed.resolved_target = outcome.resolved_target;
          completed.virtual_display_requested = outcome.virtual_display_requested;
          completed.display_may_have_changed = outcome.display_may_have_changed;
          completed.durable_recovery_armed = outcome.durable_recovery_armed;
          completed.durable_recovery_attempted = outcome.durable_recovery_attempted;
          completed.staged_state_prepared = outcome.staged_state_prepared;
          completed.generation = generation;
          enqueue(completed);
        });
      return generation;
    }

    void dispatch_verification(
      const ApplyRequest &request,
      const std::optional<ActiveTopology> &expected_topology,
      const std::optional<ResolvedConfigurationTarget> &resolved_target,
      std::chrono::milliseconds delay = std::chrono::milliseconds(0),
      VerificationPurpose purpose = VerificationPurpose::Initial) {
      const auto token = system_.token();
      const auto generation = token.generation();

      const auto completion = [enqueue = enqueue_, generation, purpose](bool success) {
          VerificationCompleted completed;
          completed.success = success;
          completed.purpose = purpose;
          completed.generation = generation;
          enqueue(completed);
      };
      if (delay > std::chrono::milliseconds::zero()) {
        dispatcher_.dispatch_verification_after(request, expected_topology, resolved_target, token, delay, completion);
      } else {
        dispatcher_.dispatch_verification(request, expected_topology, resolved_target, token, completion);
      }
    }

    void dispatch_reset_staged_apply_state() {
      const auto generation = system_.current_generation();
      dispatcher_.dispatch_reset_staged_apply_state(
        [enqueue = enqueue_, generation](bool success) {
          ResetCompleted completed;
          completed.success = success;
          completed.generation = generation;
          enqueue(completed);
        });
    }

    std::uint64_t dispatch_refresh_rate(const RefreshRateCommand &command) {
      const auto token = system_.token();
      const auto generation = token.generation();
      dispatcher_.dispatch_refresh_rate(
        command.device_id,
        command.numerator,
        command.denominator,
        token,
        [enqueue = enqueue_, generation, command](bool success) {
          RefreshRateCompleted completed;
          completed.success = success;
          completed.device_id = command.device_id;
          completed.numerator = command.numerator;
          completed.denominator = command.denominator;
          completed.generation = generation;
          completed.connection_epoch = command.connection_epoch;
          completed.request_id = command.request_id;
          enqueue(completed);
        });
      return generation;
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

    std::uint64_t dispatch_recovery(std::chrono::milliseconds delay = std::chrono::milliseconds(0)) {
      const auto token = system_.token();
      const auto generation = token.generation();

      dispatcher_.dispatch_recovery(
        token,
        delay,
        [enqueue = enqueue_, generation](const RecoveryOutcome &outcome) {
          RecoveryCompleted completed;
          completed.success = outcome.success;
          completed.snapshot = outcome.snapshot;
          completed.display_may_have_changed = outcome.display_may_have_changed;
          completed.staged_state_reset_attempted = outcome.staged_state_reset_attempted;
          completed.staged_state_reset_succeeded = outcome.staged_state_reset_succeeded;
          completed.generation = generation;
          enqueue(completed);
        });
      return generation;
    }

    std::uint64_t dispatch_recovery_validation(const Snapshot &snapshot) {
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
      return generation;
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
     * @brief Build the live topology plus physical baseline devices that have
     *        become active but are not yet members of the active topology.
     *
     * The persisted snapshot remains authoritative and is never rewritten.
     * Inactive, excluded, and virtual devices are not admitted.
     */
    std::optional<ActiveTopology> topology_with_returned_active_baseline_devices(
      const std::string &virtual_device_id,
      const std::vector<std::string> &exclusions);

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
    void set_apply_result_callback(std::function<void(ApplyStatus, std::uint64_t, std::uint64_t)> callback);
    void set_verification_result_callback(std::function<void(bool, std::uint64_t, std::uint64_t)> callback);
    void set_snapshot_result_callback(std::function<void(bool, std::uint64_t, std::uint64_t)> callback);
    void set_refresh_rate_result_callback(std::function<void(bool, std::uint64_t, std::uint64_t)> callback);
    void set_exit_callback(std::function<void(int)> callback);
    /// The pipe owner updates this monotonically as connections are replaced.
    /// Commands tagged by an older connection are discarded before they can
    /// mutate the long-lived helper state.
    void set_connection_epoch_provider(std::function<std::uint64_t()> provider);
    void set_snapshot_blacklist(std::set<std::string> blacklist);

    void handle_message(const Message &message);

    /// Periodic driver for restore window/backoff scheduling (legacy restore_poll_proc).
    /// Call from the helper main loop every ~100ms.
    void handle_tick();

    State state() const;
    bool recovery_armed() const;
    /// True when a disconnect must retain/enter recovery because this helper
    /// may have changed the desktop or is actively applying/restoring it.
    bool requires_disconnect_recovery() const;
    /// v1 treats a pipe break immediately after APPLY as startup churn: retain
    /// the session and run its bounded disconnect checks instead of restoring
    /// the desktop just as Windows is activating it.
    bool should_defer_disconnect_recovery() const;
    /// Starts/arms the bounded v1-compatible verify-before-repair work for a
    /// recent APPLY disconnect. Returns true only when restore recovery should
    /// be deferred.
    bool begin_transient_disconnect_settlement();

    /// True while a restore request is being worked on (Recovery/RecoveryValidation/EventLoop).
    bool restore_pending() const;
    bool recovery_worker_in_progress() const;

  private:
    struct VirtualDisplayRepairIntent {
      std::string resolved_device_id;
      std::uint64_t connection_epoch = 0;
    };

    using DeferredMutationCommand = std::variant<ApplyCommand, VirtualDisplayRepairIntent, RevertCommand, DisarmCommand, ResetCommand>;

    enum class MutationWorkerKind {
      Apply,
      RefreshRate,
      Recovery,
    };

    struct ActiveMutationWorker {
      MutationWorkerKind kind;
      std::uint64_t generation;
    };

    void retarget_virtual_display_device_id_if_needed(const std::string &resolved_device_id);
    ApplyRequest verification_request() const;

    void handle_apply_command(const ApplyCommand &command);
    void handle_revert_command(const RevertCommand &command);
    void handle_disarm_command(const DisarmCommand &command);
    void handle_export_golden(const ExportGoldenCommand &command);
    void handle_snapshot_current(const SnapshotCurrentCommand &command);
    void handle_refresh_rate_command(const RefreshRateCommand &command);
    void handle_virtual_display_repair_intent(const VirtualDisplayRepairIntent &intent);
    void handle_reset_command(const ResetCommand &command);
    void handle_ping_command(const PingCommand &command);
    void handle_stop_command(const StopCommand &command);
    void handle_apply_completed(const ApplyCompleted &completed);
    void finish_apply_completed(const ApplyCompleted &completed);
    void handle_verification_completed(const VerificationCompleted &completed);
    void handle_reset_completed(const ResetCompleted &completed);
    void handle_refresh_rate_completed(const RefreshRateCompleted &completed);
    void handle_recovery_completed(const RecoveryCompleted &completed);
    void handle_recovery_validation_completed(const RecoveryValidationCompleted &completed);
    void handle_display_event(const DisplayEventMessage &event);
    void handle_helper_event(const HelperEventMessage &event);

    void transition(State next, ApplyAction trigger, std::optional<ApplyStatus> status = std::nullopt);
    bool is_stale(std::uint64_t generation) const;
    bool is_stale_connection(std::uint64_t connection_epoch) const;
    bool apply_in_flight() const;

    std::vector<std::string> exclusions_vector() const;
    void update_blacklist(const std::vector<std::string> &exclude_devices);
    void start_recovery(std::chrono::milliseconds delay, ApplyAction trigger);
    void clear_recovery_state(bool delete_restore_task);
    void activate_recovery_lease();
    void send_apply_result(ApplyStatus status);
    void send_verification_result(bool success);
    void send_snapshot_result(bool success, std::uint64_t connection_epoch, std::uint64_t request_id);
    void send_refresh_rate_result(bool success, std::uint64_t connection_epoch, std::uint64_t request_id);
    void enter_steady_state();
    void reset_apply_verification_state();
    void reset_transient_disconnect_settlement();
    void prepare_transient_disconnect_settlement();
    bool dispatch_next_verification_reapply();
    bool dispatch_next_transient_disconnect_verification();
    void dispatch_transient_disconnect_repair();
    void dispatch_settings_only_repair();
    void dispatch_apply_worker(const ApplyRequest &request, std::chrono::milliseconds delay, bool reset_virtual_display);
    void queue_after_active_mutation(
      DeferredMutationCommand command,
      const char *label,
      bool cancel_active_mutation = true);
    /// Queue a control intent behind a worker/reset barrier.  APPLY, REVERT,
    /// and DISARM describe the desired next session state, so only the newest
    /// such intent may survive. RESET is deliberately retained as an ordered
    /// persistence barrier rather than being folded into that session intent.
    void enqueue_deferred_mutation_command(DeferredMutationCommand command, const char *label);
    /// Returns whether the mutation fence already holds a newer session start.
    /// Autonomous recovery must not coalesce that APPLY away.
    bool has_deferred_apply_intent() const;
    void drain_deferred_mutation_commands();
    void begin_staged_state_reset();
    void retain_recovery_after_display_mutation(const ApplyCompleted &completed);
    void retain_recovery_after_cancelled_recovery();
    void apply_recovery_policy_from_current_request();
    void reset_uncommitted_staged_state_if_needed();
    bool mutation_worker_active() const;
    bool owns_active_worker(MutationWorkerKind kind, std::uint64_t generation) const;
    bool refresh_targets_active_request(const std::string &device_id) const;
    bool virtual_identity_repair_available() const;
    bool consume_virtual_identity_discovery();
    bool consume_virtual_identity_repair();

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
    bool display_changes_pending_recovery_ = false;
    // An explicit client REVERT must survive later autonomous disconnect
    // policy decisions. This is separate from restore_on_disconnect, which
    // controls only whether a disconnected live session begins a recovery.
    bool explicit_recovery_required_ = false;
    bool virtual_hdr_fallback_attempted_ = false;
    bool baseline_topology_repair_available_ = false;
    bool baseline_topology_repair_in_flight_ = false;
    bool apply_result_sent_ = false;
    bool verification_result_sent_ = false;
    // A result may have been sent for an initial failure. Keep successful
    // verification distinct so only an already-verified session treats a
    // later virtual-device replacement repair as best effort.
    bool session_was_verified_ = false;
    bool restore_task_created_ = false;
    // Capture remains closed for one immediate mismatch repair. There is no
    // proactive post-Apply retry staircase once a session has been verified.
    bool verification_reapply_available_ = false;
    // v1 also performs a short 250/750ms verify-before-reapply settlement
    // sequence if the control pipe disappears right after APPLY. This is
    // distinct from the initial capture gate because it can begin after an
    // interrupted transaction with no client response lane.
    bool transient_disconnect_settlement_requested_ = false;
    bool transient_disconnect_check_pending_ = false;
    bool transient_disconnect_repair_in_flight_ = false;
    std::size_t next_transient_disconnect_check_ = 0;
    std::optional<std::chrono::steady_clock::time_point> last_apply_started_;
    ApplyRequest current_request_ {};
    std::optional<ResolvedConfigurationTarget> resolved_target_;
    // Candidate physical-return topology. It remains separate from the
    // authoritative current request until the exact topology verifies.
    std::optional<ActiveTopology> expected_topology_;
    std::optional<Snapshot> recovery_snapshot_;
    std::optional<std::chrono::steady_clock::time_point> recovery_event_feedback_quiet_until_;
    std::size_t virtual_identity_discoveries_remaining_ = 0;
    std::size_t virtual_identity_repairs_remaining_ = 0;
    std::set<std::string> snapshot_blacklist_;
    std::uint64_t current_connection_epoch_ = 0;

    // A durable task is armed by the worker immediately before a mutation,
    // but only the state machine may remove it. The fence covers both staged
    // APPLY and snapshot recovery work, keeping destructive control messages
    // and replacement APPLYs behind the worker until it reports completion.
    std::optional<ActiveMutationWorker> active_mutation_worker_;
    std::deque<DeferredMutationCommand> deferred_mutation_commands_;
    // If RESET is ordered before a newer Apply, hold the completed older
    // outcome until the barrier either starts that replacement or proves the
    // replacement stale. This prevents both obsolete capture-ready replies
    // and an ownerless InProgress state.
    std::optional<ApplyCompleted> deferred_apply_completion_;
    bool unconfirmed_cancelled_mutation_ = false;
    bool staged_state_reset_pending_ = false;
    // Recovery normally exits the helper after a confirmed restore. If RESET
    // was queued behind that recovery, retain the helper until the ordered
    // backend reset completes.
    bool exit_after_staged_state_reset_ = false;
    // SettingsManager may retain original-state transaction data after it is
    // called. Track it so restore/RESET can clear that session-scoped state.
    bool staged_state_prepared_ = false;
    // The recovery worker clears the backend's retained staged state before
    // validation. Preserve a failed result through validation so a live
    // helper cannot accept another APPLY with an obsolete baseline.
    std::optional<bool> recovery_staged_state_reset_succeeded_;
    bool recovery_lease_before_apply_ = false;


    StateObserver observer_;
    std::function<void(ApplyStatus, std::uint64_t, std::uint64_t)> apply_result_callback_;
    std::function<void(bool, std::uint64_t, std::uint64_t)> verification_result_callback_;
    std::function<void(bool, std::uint64_t, std::uint64_t)> snapshot_result_callback_;
    std::function<void(bool, std::uint64_t, std::uint64_t)> refresh_rate_result_callback_;
    std::function<void(int)> exit_callback_;
    std::function<std::uint64_t()> connection_epoch_provider_;
  };
}  // namespace display_helper::v2
