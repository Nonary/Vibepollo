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
  std::optional<std::pair<Snapshot, codec::layout_rotation_map_t>> SnapshotLedger::capture_filtered(const std::vector<std::string> &exclusions, const char *reason) {
    auto snap = service_.capture();
    if (!service_.topology_is_valid(snap.m_topology)) {
      BOOST_LOG(warning) << "Skipping display snapshot save (" << (reason ? reason : "snapshot")
                         << "); topology is invalid or empty.";
      return std::nullopt;
    }

    std::string reject_reason;
    auto filtered = codec::filter_snapshot_for_save(std::move(snap), service_.enumerate(), exclusions, reject_reason);
    if (!filtered) {
      BOOST_LOG(warning) << "Skipping display snapshot save (" << (reason ? reason : "snapshot")
                         << "); " << reject_reason << ".";
      return std::nullopt;
    }

    const auto layout_ids_vec = codec::flatten_topology_device_ids(filtered->m_topology);
    const std::set<std::string> layout_ids(layout_ids_vec.begin(), layout_ids_vec.end());
    auto layouts = service_.capture_layouts(layout_ids);
    return std::make_pair(std::move(*filtered), std::move(layouts));
  }

  bool SnapshotLedger::capture_filtered_and_save(SnapshotTier tier, const std::vector<std::string> &exclusions, const char *reason) {
    const char *why = reason ? reason : "snapshot";
    constexpr int kMaxAttempts = 3;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
      if (auto captured = capture_filtered(exclusions, why)) {
        if (persistence_.storage().save(tier, captured->first, captured->second)) {
          if (attempt > 1) {
            BOOST_LOG(info) << "Display snapshot save succeeded on retry #" << attempt << " (" << why << ").";
          }
          return true;
        }
      }
      if (attempt < kMaxAttempts) {
        BOOST_LOG(info) << "Display snapshot save retry #" << (attempt + 1) << " scheduled (" << why << ").";
        clock_.sleep_for(std::chrono::milliseconds(50));
      }
    }
    return false;
  }

  bool SnapshotLedger::refresh_current_preserving_previous(const std::vector<std::string> &exclusions) {
    // Capture first; a failed capture must never destroy the existing baseline
    // chain (53cd8b4c / 62839421).
    auto captured = capture_filtered(exclusions, "snapshot-only");
    if (!captured) {
      BOOST_LOG(info) << "Refreshed current session snapshot (snapshot-only): false";
      return false;
    }

    auto &storage = persistence_.storage();
    if (auto current = storage.load_with_metadata(SnapshotTier::Current)) {
      if (!storage.save(SnapshotTier::Previous, current->snapshot, current->layout_rotations)) {
        BOOST_LOG(warning) << "Failed to refresh session snapshot history (snapshot-only): current->previous copy failed.";
      }
    }

    const bool replaced = storage.save(SnapshotTier::Current, captured->first, captured->second);
    BOOST_LOG(info) << "Refreshed current session snapshot (snapshot-only): " << (replaced ? "true" : "false");
    return replaced;
  }

  StateMachine::StateMachine(
    ApplyPipeline &apply,
    RecoveryPipeline &recovery,
    SnapshotLedger &snapshots,
    SystemPorts &system,
    IVirtualDisplayDriver &virtual_display,
    GoldenHealth &golden_health,
    RestoreState &restore_state)
    : apply_(apply),
      recovery_(recovery),
      snapshots_(snapshots),
      system_(system),
      virtual_display_(virtual_display),
      golden_health_(golden_health),
      restore_state_(restore_state) {}

  std::vector<std::string> StateMachine::exclusions_vector() const {
    return {snapshot_blacklist_.begin(), snapshot_blacklist_.end()};
  }

  void StateMachine::update_blacklist(const std::vector<std::string> &exclude_devices) {
    snapshot_blacklist_.clear();
    for (const auto &id : exclude_devices) {
      if (!id.empty()) {
        snapshot_blacklist_.insert(id);
      }
    }
    restore_state_.set_exclusions(exclusions_vector());
  }

  void StateMachine::start_recovery(std::chrono::milliseconds delay, ApplyAction trigger) {
    transition(State::Recovery, trigger);
    recovery_.dispatch_recovery(delay);
  }

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
    restore_state_.set_exclusions(exclusions_vector());
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
    BOOST_LOG(info) << "Display helper: received Apply command"
                    << (command.request.configuration ? " with configuration" : " without configuration")
                    << ", prefer_golden_first=" << (command.request.prefer_golden_first ? "true" : "false")
                    << (command.request.virtual_layout ? ", virtual_layout=" + *command.request.virtual_layout : "");

    // A new APPLY supersedes any pending restore via IPC instead of forcing a
    // helper restart (72b0d996). Cancel in-flight work and disarm the scheduler.
    system_.cancel_operations();
    scheduler_.disarm();
    restore_state_.reset_request_progress();

    restore_state_.always_restore_from_golden.store(command.request.prefer_golden_first, std::memory_order_release);
    restore_state_.restore_on_disconnect.store(command.request.restore_on_disconnect, std::memory_order_release);

    apply_attempt_ = 1;
    apply_result_sent_ = false;
    current_request_ = command.request;
    expected_topology_.reset();

    snapshots_.set_prefer_golden_first(command.request.prefer_golden_first);

    // The session baseline is normally captured earlier via SnapshotCurrent. That
    // request is fire-and-forget and can be lost (helper restart races), which
    // used to leave REVERT with nothing to restore and strand the user on the
    // session-only display layout (f3841ad8). Capture the pre-apply state here as
    // a fallback whenever no baseline exists yet.
    if (!snapshots_.tier_exists(SnapshotTier::Current)) {
      BOOST_LOG(warning) << "Display helper: no session baseline present at APPLY; capturing pre-apply baseline now.";
      if (!snapshots_.capture_filtered_and_save(SnapshotTier::Current, exclusions_vector(), "pre-apply baseline")) {
        BOOST_LOG(warning) << "Display helper: pre-apply baseline capture failed; REVERT may have nothing to restore.";
      }
    }

    system_.create_restore_task();

    transition(State::InProgress, ApplyAction::Apply);
    apply_.dispatch_apply(current_request_, std::chrono::milliseconds(0), false);
  }

  void StateMachine::handle_revert_command(const RevertCommand &command) {
    // Disconnect-triggered reverts honor the restore-on-disconnect policy: a
    // paused stream with revert_on_disconnect=false must preserve its display
    // state (3b7a52c4 / 0add1f80). Explicit client REVERTs always run.
    if (command.from_disconnect &&
        !restore_state_.restore_on_disconnect.load(std::memory_order_acquire) &&
        !restore_pending()) {
      BOOST_LOG(info) << "Display helper: disconnect with restore-on-disconnect disabled; not restoring.";
      return;
    }

    // If there is nothing to restore from, exit rather than spinning (legacy
    // restore_poll_proc early-exit; keeps --restore boot tasks from hanging).
    if (!snapshots_.tier_exists(SnapshotTier::Current) &&
        !snapshots_.tier_exists(SnapshotTier::Previous) &&
        !snapshots_.tier_exists(SnapshotTier::Golden)) {
      BOOST_LOG(info) << "Restore: no session/previous or golden snapshot present; nothing to restore.";
      if (exit_callback_) {
        exit_callback_(0);
      }
      return;
    }

    BOOST_LOG(info) << "Display helper: received Revert command, initiating recovery"
                    << (command.prefer_golden_if_current_missing ? " (prefer golden if current missing)." : ".");

    restore_state_.prefer_golden_if_current_missing.store(command.prefer_golden_if_current_missing, std::memory_order_release);
    if (command.always_restore_from_golden.has_value()) {
      restore_state_.always_restore_from_golden.store(*command.always_restore_from_golden, std::memory_order_release);
    }
    golden_health_.reset_request_tracking();

    system_.cancel_operations();
    recovery_armed_ = true;
    system_.arm_heartbeat();

    // Give Sunshine a short window to immediately start a new session and DISARM,
    // avoiding costly restore/apply thrash during fast client switching. The boot
    // --restore path runs immediately.
    const auto grace = command.immediate ? std::chrono::milliseconds(0) : std::chrono::milliseconds(5000);
    scheduler_.arm_primary(system_.now(), grace);
    start_recovery(grace, ApplyAction::Revert);
  }

  void StateMachine::handle_disarm_command(const DisarmCommand &) {
    // A restore attempt that has not been confirmed yet must not be cancelled or
    // overwritten by a later stream-start probe (72b0d996).
    if (restore_pending() &&
        restore_state_.restore_attempted_unconfirmed.load(std::memory_order_acquire)) {
      BOOST_LOG(info) << "DISARM command ignored because an unconfirmed restore attempt is still pending.";
      return;
    }

    BOOST_LOG(info) << "Display helper: received Disarm command, resetting state";

    system_.cancel_operations();
    scheduler_.disarm();
    restore_state_.reset_request_progress();
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
    if (command.payload.update_exclusions || !command.payload.exclude_devices.empty()) {
      update_blacklist(command.payload.exclude_devices);
    }

    const bool saved = snapshots_.capture_filtered_and_save(SnapshotTier::Golden, exclusions_vector(), "export-golden");
    if (saved) {
      golden_health_.clear_status("snapshot exported");
    }
    BOOST_LOG(info) << "Export golden restore snapshot result=" << (saved ? "true" : "false");
  }

  void StateMachine::handle_snapshot_current(const SnapshotCurrentCommand &command) {
    // Never overwrite the restore baseline while a restore is being worked on
    // (72b0d996): the snapshot would capture the un-restored state.
    if (restore_pending()) {
      BOOST_LOG(info) << "Skipping current session snapshot refresh while restore is pending.";
      return;
    }

    if (command.payload.update_exclusions || !command.payload.exclude_devices.empty()) {
      update_blacklist(command.payload.exclude_devices);
    }

    (void) snapshots_.refresh_current_preserving_previous(exclusions_vector());
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
    scheduler_.on_attempt_failed(system_.now());
    transition(State::EventLoop, ApplyAction::Revert);
  }

  void StateMachine::handle_recovery_validation_completed(const RecoveryValidationCompleted &completed) {
    if (is_stale(completed.generation)) {
      return;
    }

    if (completed.success) {
      BOOST_LOG(info) << "Display helper: recovery validation succeeded, display settings restored.";
      recovery_armed_ = false;
      scheduler_.disarm();
      restore_state_.reset_request_progress();
      system_.disarm_heartbeat();
      system_.refresh_shell();
      system_.delete_restore_task();
      // Return to Waiting before signalling completion: the host may keep the
      // helper alive when a newer connection is active (72b0d996).
      transition(State::Waiting, ApplyAction::Revert, ApplyStatus::Ok);
      if (exit_callback_) {
        exit_callback_(0);
      }
      return;
    }

    BOOST_LOG(warning) << "Display helper: recovery validation failed, entering event loop for retry.";
    scheduler_.on_attempt_failed(system_.now());
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

    // Display events reset the backoff and (re)open the event restore window;
    // the actual attempt fires immediately when allowed (legacy signal_restore_event).
    scheduler_.on_display_event(system_.now());
    if (scheduler_.should_attempt(system_.now())) {
      start_recovery(std::chrono::milliseconds(0), ApplyAction::Revert);
    }
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

    // Heartbeat loss means Sunshine crashed/hung: honor the restore-on-disconnect
    // policy the same way a broken pipe would (3b7a52c4).
    if (!restore_state_.restore_on_disconnect.load(std::memory_order_acquire) && !restore_pending()) {
      BOOST_LOG(info) << "Display helper: heartbeat lost with restore-on-disconnect disabled; not restoring.";
      return;
    }

    BOOST_LOG(info) << "Display helper: initiating recovery due to heartbeat timeout";
    golden_health_.reset_request_tracking();
    scheduler_.arm_primary(system_.now(), std::chrono::milliseconds(5000));
    start_recovery(std::chrono::milliseconds(5000), ApplyAction::Revert);
  }

  void StateMachine::handle_tick() {
    if (state_ != State::EventLoop || !recovery_armed_) {
      return;
    }

    const auto now = system_.now();
    if (scheduler_.window_just_expired(now)) {
      BOOST_LOG(info) << "Restore polling: window exhausted; pausing attempts until next event.";
      golden_health_.register_unresolved("restore window exhausted");
      return;
    }
    if (scheduler_.should_attempt(now)) {
      start_recovery(std::chrono::milliseconds(0), ApplyAction::Revert);
    }
  }

  bool StateMachine::restore_pending() const {
    return state_ == State::Recovery || state_ == State::RecoveryValidation || state_ == State::EventLoop;
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
