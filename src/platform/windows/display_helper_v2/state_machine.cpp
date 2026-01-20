#include "src/platform/windows/display_helper_v2/state_machine.h"

#include <boost/algorithm/string/predicate.hpp>
#include <utility>

#include "src/logging.h"

namespace display_helper::v2 {
  namespace {
    const char *state_to_string(State state) {
      switch (state) {
        case State::Waiting:
          return "Waiting";
        case State::InProgress:
          return "InProgress";
        case State::Verification:
          return "Verification";
        case State::Recovery:
          return "Recovery";
        case State::RecoveryValidation:
          return "RecoveryValidation";
        case State::EventLoop:
          return "EventLoop";
        case State::VirtualDisplayMonitoring:
          return "VirtualDisplayMonitoring";
        default:
          return "Unknown";
      }
    }

    const char *action_to_string(ApplyAction action) {
      switch (action) {
        case ApplyAction::Apply:
          return "Apply";
        case ApplyAction::Revert:
          return "Revert";
        case ApplyAction::Disarm:
          return "Disarm";
        case ApplyAction::ExportGolden:
          return "ExportGolden";
        case ApplyAction::SnapshotCurrent:
          return "SnapshotCurrent";
        case ApplyAction::Reset:
          return "Reset";
        case ApplyAction::Ping:
          return "Ping";
        case ApplyAction::Stop:
          return "Stop";
        default:
          return "Unknown";
      }
    }

    const char *display_event_to_string(DisplayEvent event) {
      switch (event) {
        case DisplayEvent::DisplayChange:
          return "DisplayChange";
        case DisplayEvent::PowerResume:
          return "PowerResume";
        case DisplayEvent::DeviceArrival:
          return "DeviceArrival";
        case DisplayEvent::DeviceRemoval:
          return "DeviceRemoval";
        default:
          return "Unknown";
      }
    }

    const char *apply_status_to_string(ApplyStatus status) {
      switch (status) {
        case ApplyStatus::Ok:
          return "Ok";
        case ApplyStatus::HelperUnavailable:
          return "HelperUnavailable";
        case ApplyStatus::InvalidRequest:
          return "InvalidRequest";
        case ApplyStatus::VerificationFailed:
          return "VerificationFailed";
        case ApplyStatus::NeedsVirtualDisplayReset:
          return "NeedsVirtualDisplayReset";
        case ApplyStatus::Retryable:
          return "Retryable";
        case ApplyStatus::Fatal:
          return "Fatal";
        default:
          return "Unknown";
      }
    }
  }  // namespace
  StateMachine::StateMachine(
    ApplyPipeline &apply,
    RecoveryPipeline &recovery,
    SnapshotLedger &snapshots,
    SystemPorts &system,
    IVirtualDisplayDriver &virtual_display)
    : apply_(apply),
      recovery_(recovery),
      snapshots_(snapshots),
      system_(system),
      virtual_display_(virtual_display) {}

  void StateMachine::retarget_virtual_display_device_id_if_needed() {
    if (!current_request_.virtual_layout.has_value()) {
      return;
    }
    if (!current_request_.configuration) {
      return;
    }

    const std::string resolved = virtual_display_.device_id();
    if (resolved.empty()) {
      return;
    }

    auto &cfg = *current_request_.configuration;
    const std::string previous = cfg.m_device_id;
    if (!previous.empty() && boost::iequals(previous, resolved)) {
      return;
    }

    BOOST_LOG(info) << "Display helper: retargeting virtual display device_id from '"
                    << (previous.empty() ? std::string("(empty)") : previous)
                    << "' to '" << resolved << "' for monitoring re-apply.";

    cfg.m_device_id = resolved;

    if (current_request_.topology) {
      for (auto &group : *current_request_.topology) {
        for (auto &device_id : group) {
          if (previous.empty()) {
            continue;
          }
          if (boost::iequals(device_id, previous)) {
            device_id = resolved;
          }
        }
      }
    }

    for (auto &entry : current_request_.monitor_positions) {
      if (!previous.empty() && boost::iequals(entry.first, previous)) {
        entry.first = resolved;
      }
    }
  }

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

    BOOST_LOG(info) << "Display helper: received Apply command"
                    << (command.request.configuration ? " with configuration" : " without configuration")
                    << ", prefer_golden_first=" << (command.request.prefer_golden_first ? "true" : "false")
                    << (command.request.virtual_layout ? ", virtual_layout=" + *command.request.virtual_layout : "");

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

    BOOST_LOG(info) << "Display helper: received Revert command, initiating recovery";

    system_.cancel_operations();
    recovery_armed_ = true;
    system_.arm_heartbeat();
    system_.delete_restore_task();

    transition(State::Recovery, ApplyAction::Revert);
    recovery_.dispatch_recovery();
  }

  void StateMachine::handle_disarm_command(const DisarmCommand &) {
    BOOST_LOG(info) << "Display helper: received Disarm command, resetting state";

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
    BOOST_LOG(info) << "Display helper: received STOP command, exiting gracefully.";
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

      // For virtual displays, enter monitoring state to handle device crashes
      if (current_request_.virtual_layout.has_value()) {
        transition(State::VirtualDisplayMonitoring, ApplyAction::Apply, ApplyStatus::Ok);
        return;
      }
    }

    transition(State::Waiting, ApplyAction::Apply, completed.success ? std::make_optional(ApplyStatus::Ok) : std::nullopt);
  }

  void StateMachine::handle_recovery_completed(const RecoveryCompleted &completed) {
    if (is_stale(completed.generation)) {
      return;
    }

    BOOST_LOG(info) << "Display helper: recovery operation completed, success=" << (completed.success ? "true" : "false")
                    << ", has_snapshot=" << (completed.snapshot ? "true" : "false");

    if (completed.success && completed.snapshot) {
      recovery_snapshot_ = completed.snapshot;
      transition(State::RecoveryValidation, ApplyAction::Revert);
      recovery_.dispatch_recovery_validation(*recovery_snapshot_);
      return;
    }

    BOOST_LOG(warning) << "Display helper: recovery failed or no valid snapshot found, entering event loop";
    transition(State::EventLoop, ApplyAction::Revert);
  }

  void StateMachine::handle_recovery_validation_completed(const RecoveryValidationCompleted &completed) {
    if (is_stale(completed.generation)) {
      return;
    }

    if (completed.success) {
      BOOST_LOG(info) << "Display helper: recovery validation succeeded, display settings restored. Exiting gracefully.";
      recovery_armed_ = false;
      system_.disarm_heartbeat();
      system_.delete_restore_task();
      if (exit_callback_) {
        exit_callback_(0);
      }
      return;
    }

    BOOST_LOG(warning) << "Display helper: recovery validation failed, entering event loop for retry.";
    transition(State::EventLoop, ApplyAction::Revert);
  }

  void StateMachine::handle_display_event(const DisplayEventMessage &event) {
    if (is_stale(event.generation)) {
      BOOST_LOG(debug) << "Display helper: ignoring stale display event " << display_event_to_string(event.event);
      return;
    }

    BOOST_LOG(info) << "Display helper: received display event '" << display_event_to_string(event.event)
                    << "' in state " << state_to_string(state_);

    // Virtual display monitoring: re-apply configuration when device crashes/recovers
    if (state_ == State::VirtualDisplayMonitoring) {
      BOOST_LOG(info) << "Display helper: display event while monitoring virtual display, re-applying configuration.";
      retarget_virtual_display_device_id_if_needed();
      apply_attempt_ = 1;
      apply_result_sent_ = false;
      transition(State::InProgress, ApplyAction::Apply);
      apply_.dispatch_apply(current_request_, std::chrono::milliseconds(0), false);
      return;
    }

    // During active apply with virtual display, restart the apply operation
    if ((state_ == State::InProgress || state_ == State::Verification) &&
        current_request_.virtual_layout.has_value()) {
      if (current_request_.configuration) {
        const std::string resolved = virtual_display_.device_id();
        if (!resolved.empty() && boost::iequals(current_request_.configuration->m_device_id, resolved)) {
          // Applying modes/HDR to an IDD can generate display events that do not require a full restart.
          // Only restart when the virtual display device_id changes (e.g. device crash/recreate).
          BOOST_LOG(debug) << "Display helper: display event during virtual display apply ignored (device id unchanged).";
          return;
        }
      }

      static constexpr auto kDebounce = std::chrono::milliseconds(250);
      static constexpr auto kRestartDelay = std::chrono::milliseconds(100);

      const auto now = system_.now();
      if (last_virtual_apply_display_event_restart_.time_since_epoch().count() != 0) {
        const auto elapsed = now - last_virtual_apply_display_event_restart_;
        if (elapsed < kDebounce) {
          BOOST_LOG(debug) << "Display helper: coalescing display event during virtual display apply.";
          return;
        }
      }
      last_virtual_apply_display_event_restart_ = now;

      BOOST_LOG(info) << "Display helper: display event during virtual display apply, restarting apply.";

      // Cancel in-flight apply/verification work so their completions become stale.
      system_.cancel_operations();
      expected_topology_.reset();
      retarget_virtual_display_device_id_if_needed();
      transition(State::InProgress, ApplyAction::Apply);
      apply_.dispatch_apply(current_request_, kRestartDelay, false);
      return;
    }

    // Standard recovery from EventLoop state
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

    BOOST_LOG(warning) << "Display helper: heartbeat timeout detected in state " << state_to_string(state_)
                       << ", recovery_armed=" << (recovery_armed_ ? "true" : "false");

    if (!recovery_armed_) {
      return;
    }

    BOOST_LOG(info) << "Display helper: initiating recovery due to heartbeat timeout";
    transition(State::Recovery, ApplyAction::Revert);
    recovery_.dispatch_recovery();
  }

  void StateMachine::transition(State next, ApplyAction trigger, std::optional<ApplyStatus> status) {
    if (next == state_) {
      return;
    }

    if (status) {
      BOOST_LOG(info) << "Display helper: state transition " << state_to_string(state_)
                      << " -> " << state_to_string(next)
                      << " (trigger: " << action_to_string(trigger)
                      << ", status: " << apply_status_to_string(*status) << ")";
    } else {
      BOOST_LOG(info) << "Display helper: state transition " << state_to_string(state_)
                      << " -> " << state_to_string(next)
                      << " (trigger: " << action_to_string(trigger) << ")";
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
