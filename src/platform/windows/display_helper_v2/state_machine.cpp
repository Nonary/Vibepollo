#include "src/platform/windows/display_helper_v2/state_machine.h"

#include <utility>

namespace display_helper::v2 {
  StateMachine::StateMachine(
    ApplyPipeline &apply,
    RecoveryPipeline &recovery,
    SnapshotLedger &snapshots,
    SystemPorts &system)
    : apply_(apply),
      recovery_(recovery),
      snapshots_(snapshots),
      system_(system) {}

  void StateMachine::set_state_observer(StateObserver observer) {
    observer_ = std::move(observer);
  }

  void StateMachine::set_apply_result_callback(std::function<void(ApplyStatus)> callback) {
    apply_result_callback_ = std::move(callback);
  }

  void StateMachine::set_verification_result_callback(std::function<void(bool)> callback) {
    verification_result_callback_ = std::move(callback);
  }

  void StateMachine::set_exit_callback(std::function<void(int)> callback) {
    exit_callback_ = std::move(callback);
  }

  void StateMachine::set_snapshot_blacklist(std::set<std::string> blacklist) {
    snapshot_blacklist_ = std::move(blacklist);
  }

  State StateMachine::state() const {
    return state_;
  }

  bool StateMachine::recovery_armed() const {
    return recovery_armed_;
  }

  void StateMachine::handle_message(const Message &message) {
    std::visit([
      this
    ](const auto &payload) {
      using T = std::decay_t<decltype(payload)>;
      if constexpr (std::is_same_v<T, ApplyCommand>) {
        handle_apply_command(payload);
      } else if constexpr (std::is_same_v<T, RevertCommand>) {
        handle_revert_command(payload);
      } else if constexpr (std::is_same_v<T, DisarmCommand>) {
        handle_disarm_command(payload);
      } else if constexpr (std::is_same_v<T, ExportGoldenCommand>) {
        handle_export_golden(payload);
      } else if constexpr (std::is_same_v<T, SnapshotCurrentCommand>) {
        handle_snapshot_current(payload);
      } else if constexpr (std::is_same_v<T, ResetCommand>) {
        handle_reset_command(payload);
      } else if constexpr (std::is_same_v<T, PingCommand>) {
        handle_ping_command(payload);
      } else if constexpr (std::is_same_v<T, StopCommand>) {
        handle_stop_command(payload);
      } else if constexpr (std::is_same_v<T, ApplyCompleted>) {
        handle_apply_completed(payload);
      } else if constexpr (std::is_same_v<T, VerificationCompleted>) {
        handle_verification_completed(payload);
      } else if constexpr (std::is_same_v<T, RecoveryCompleted>) {
        handle_recovery_completed(payload);
      } else if constexpr (std::is_same_v<T, RecoveryValidationCompleted>) {
        handle_recovery_validation_completed(payload);
      } else if constexpr (std::is_same_v<T, DisplayEventMessage>) {
        handle_display_event(payload);
      } else if constexpr (std::is_same_v<T, HelperEventMessage>) {
        handle_helper_event(payload);
      }
    }, message);
  }

  void StateMachine::handle_apply_command(const ApplyCommand &command) {
    if (is_stale(command.generation)) {
      return;
    }

    apply_attempt_ = 1;
    apply_result_sent_ = false;
    current_request_ = command.request;
    expected_topology_.reset();

    snapshots_.set_prefer_golden_first(command.request.prefer_golden_first);

    system_.create_restore_task();

    transition(State::InProgress, ApplyAction::Apply);
    apply_.dispatch_apply(current_request_, std::chrono::milliseconds(0), false);
  }

  void StateMachine::handle_revert_command(const RevertCommand &command) {
    if (is_stale(command.generation)) {
      return;
    }

    system_.cancel_operations();
    recovery_armed_ = true;
    system_.arm_heartbeat();
    system_.delete_restore_task();

    transition(State::Recovery, ApplyAction::Revert);
    recovery_.dispatch_recovery();
  }

  void StateMachine::handle_disarm_command(const DisarmCommand &) {
    system_.cancel_operations();
    recovery_armed_ = false;
    system_.disarm_heartbeat();
    system_.delete_restore_task();
    apply_attempt_ = 0;
    apply_result_sent_ = false;
    expected_topology_.reset();
    recovery_snapshot_.reset();

    transition(State::Waiting, ApplyAction::Disarm);
  }

  void StateMachine::handle_export_golden(const ExportGoldenCommand &command) {
    snapshot_blacklist_.clear();
    for (const auto &id : command.payload.exclude_devices) {
      if (!id.empty()) {
        snapshot_blacklist_.insert(id);
      }
    }

    auto snapshot = snapshots_.capture();
    (void) snapshots_.save(SnapshotTier::Golden, std::move(snapshot), snapshot_blacklist_);
  }

  void StateMachine::handle_snapshot_current(const SnapshotCurrentCommand &command) {
    snapshot_blacklist_.clear();
    for (const auto &id : command.payload.exclude_devices) {
      if (!id.empty()) {
        snapshot_blacklist_.insert(id);
      }
    }

    (void) snapshots_.rotate_current_to_previous();
    auto snapshot = snapshots_.capture();
    (void) snapshots_.save(SnapshotTier::Current, std::move(snapshot), snapshot_blacklist_);
  }

  void StateMachine::handle_reset_command(const ResetCommand &) {
    // Deprecated: no-op.
  }

  void StateMachine::handle_ping_command(const PingCommand &) {
    system_.record_ping();
  }

  void StateMachine::handle_stop_command(const StopCommand &) {
    if (exit_callback_) {
      exit_callback_(0);
    }
  }

  void StateMachine::handle_apply_completed(const ApplyCompleted &completed) {
    if (is_stale(completed.generation)) {
      return;
    }

    expected_topology_ = completed.expected_topology;

    if (completed.status == ApplyStatus::Ok) {
      if (!apply_result_sent_ && apply_result_callback_) {
        apply_result_callback_(completed.status);
        apply_result_sent_ = true;
      }
      transition(State::Verification, ApplyAction::Apply, completed.status);
      apply_.dispatch_verification(current_request_, expected_topology_);
      return;
    }

    if (completed.status == ApplyStatus::NeedsVirtualDisplayReset) {
      const auto decision = apply_.maybe_reset_virtual_display(
        completed.status,
        completed.virtual_display_requested);
      if (decision == PolicyDecision::ResetVirtualDisplay) {
        apply_.dispatch_apply(current_request_, std::chrono::milliseconds(0), true);
        return;
      }
    }

    if (completed.status == ApplyStatus::Retryable || completed.status == ApplyStatus::VerificationFailed) {
      if (apply_.can_retry(apply_attempt_)) {
        const auto delay = apply_.retry_delay(apply_attempt_);
        ++apply_attempt_;
        apply_.dispatch_apply(current_request_, delay, false);
        return;
      }
    }

    if (!apply_result_sent_ && apply_result_callback_) {
      apply_result_callback_(completed.status);
      apply_result_sent_ = true;
    }

    transition(State::Waiting, ApplyAction::Apply, completed.status);
  }

  void StateMachine::handle_verification_completed(const VerificationCompleted &completed) {
    if (is_stale(completed.generation)) {
      return;
    }

    if (verification_result_callback_) {
      verification_result_callback_(completed.success);
    }

    if (completed.success) {
      recovery_armed_ = true;
      system_.arm_heartbeat();
      system_.refresh_shell();
      system_.blank_hdr_states(std::chrono::milliseconds(1000));
    }

    transition(State::Waiting, ApplyAction::Apply, completed.success ? std::make_optional(ApplyStatus::Ok) : std::nullopt);
  }

  void StateMachine::handle_recovery_completed(const RecoveryCompleted &completed) {
    if (is_stale(completed.generation)) {
      return;
    }

    if (completed.success && completed.snapshot) {
      recovery_snapshot_ = completed.snapshot;
      transition(State::RecoveryValidation, ApplyAction::Revert);
      recovery_.dispatch_recovery_validation(*recovery_snapshot_);
      return;
    }

    transition(State::EventLoop, ApplyAction::Revert);
  }

  void StateMachine::handle_recovery_validation_completed(const RecoveryValidationCompleted &completed) {
    if (is_stale(completed.generation)) {
      return;
    }

    if (completed.success) {
      recovery_armed_ = false;
      system_.disarm_heartbeat();
      system_.delete_restore_task();
      if (exit_callback_) {
        exit_callback_(0);
      }
      return;
    }

    transition(State::EventLoop, ApplyAction::Revert);
  }

  void StateMachine::handle_display_event(const DisplayEventMessage &event) {
    if (is_stale(event.generation)) {
      return;
    }
    if (state_ != State::EventLoop) {
      return;
    }
    if (!recovery_armed_) {
      return;
    }

    transition(State::Recovery, ApplyAction::Revert);
    recovery_.dispatch_recovery();
  }

  void StateMachine::handle_helper_event(const HelperEventMessage &event) {
    if (is_stale(event.generation)) {
      return;
    }
    if (event.event != HelperEvent::HeartbeatTimeout) {
      return;
    }
    if (!recovery_armed_) {
      return;
    }

    transition(State::Recovery, ApplyAction::Revert);
    recovery_.dispatch_recovery();
  }

  void StateMachine::transition(State next, ApplyAction trigger, std::optional<ApplyStatus> status) {
    if (next == state_) {
      return;
    }
    if (observer_) {
      observer_(StateTransition {
        .from = state_,
        .to = next,
        .trigger = trigger,
        .result_status = status,
        .timestamp = system_.now(),
      });
    }
    state_ = next;
  }

  bool StateMachine::is_stale(std::uint64_t generation) const {
    return generation != system_.current_generation();
  }
}  // namespace display_helper::v2
