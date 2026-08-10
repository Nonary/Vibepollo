#include "src/platform/windows/display_helper_v2/state_machine.h"

#include <algorithm>
#include <array>
#include <boost/algorithm/string/predicate.hpp>
#include <utility>

#include "src/platform/windows/display_helper_v2/diagnostics.h"

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
        case ApplyStatus::HdrStateFailed:
          return "HdrStateFailed";
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
    constexpr int kMaxAttempts = 3;
    std::optional<std::pair<Snapshot, codec::layout_rotation_map_t>> captured;
    for (int attempt = 1; attempt <= kMaxAttempts && !captured; ++attempt) {
      captured = capture_filtered(exclusions, "snapshot-only");
      if (!captured && attempt < kMaxAttempts) {
        BOOST_LOG(info) << "Current session snapshot capture retry #" << (attempt + 1) << " scheduled.";
        clock_.sleep_for(std::chrono::milliseconds(50));
      }
    }
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
    recovery_staged_state_reset_succeeded_.reset();
    transition(State::Recovery, trigger);
    active_mutation_worker_ = ActiveMutationWorker {
      .kind = MutationWorkerKind::Recovery,
      .generation = recovery_.dispatch_recovery(delay),
    };
  }

  void StateMachine::clear_recovery_state(bool delete_restore_task) {
    scheduler_.disarm();
    restore_state_.reset_request_progress();
    recovery_armed_ = false;
    display_changes_pending_recovery_ = false;
    explicit_recovery_required_ = false;
    last_apply_started_.reset();
    reset_transient_disconnect_settlement();
    system_.disarm_heartbeat();
    recovery_snapshot_.reset();
    if (delete_restore_task) {
      system_.delete_restore_task();
      restore_task_created_ = false;
    }
  }

  void StateMachine::activate_recovery_lease() {
    // A verified APPLY or a partial mutation must remain recoverable until an
    // explicit DISARM/RESET or a confirmed restore clears it. In particular, a
    // failed replacement APPLY must not drop the lease established by the
    // previous live session.
    recovery_armed_ = true;
    display_changes_pending_recovery_ = true;
    apply_recovery_policy_from_current_request();
  }

  void StateMachine::apply_recovery_policy_from_current_request() {
    restore_state_.always_restore_from_golden.store(
      current_request_.prefer_golden_first,
      std::memory_order_release);
    restore_state_.restore_on_disconnect.store(
      current_request_.restore_on_disconnect,
      std::memory_order_release);
    snapshots_.set_prefer_golden_first(current_request_.prefer_golden_first);
  }

  void StateMachine::send_apply_result(ApplyStatus status) {
    if (apply_result_sent_ || !apply_result_callback_) {
      return;
    }
    apply_result_callback_(status, current_connection_epoch_, current_request_.request_id);
    apply_result_sent_ = true;
  }

  void StateMachine::send_verification_result(bool success) {
    if (verification_result_sent_ || !verification_result_callback_) {
      return;
    }
    verification_result_callback_(success, current_connection_epoch_, current_request_.request_id);
    verification_result_sent_ = true;
  }

  void StateMachine::send_snapshot_result(bool success, std::uint64_t connection_epoch, std::uint64_t request_id) {
    if (snapshot_result_callback_) {
      snapshot_result_callback_(success, connection_epoch, request_id);
    }
  }

  void StateMachine::send_refresh_rate_result(bool success, std::uint64_t connection_epoch, std::uint64_t request_id) {
    if (refresh_rate_result_callback_) {
      refresh_rate_result_callback_(success, connection_epoch, request_id);
    }
  }

  void StateMachine::enter_steady_state() {
    if (current_request_.virtual_layout.has_value()) {
      transition(State::VirtualDisplayMonitoring, ApplyAction::Apply, ApplyStatus::Ok);
    } else {
      transition(State::Waiting, ApplyAction::Apply, ApplyStatus::Ok);
    }
  }

  void StateMachine::reset_post_apply_stabilization() {
    post_apply_check_pending_ = false;
    next_post_apply_stabilization_ = 0;
    post_apply_stabilization_delays_.clear();
    next_verification_reapply_ = 0;
    verification_reapply_delays_.clear();
  }

  void StateMachine::reset_transient_disconnect_settlement() {
    transient_disconnect_settlement_requested_ = false;
    transient_disconnect_check_pending_ = false;
    transient_disconnect_repair_in_flight_ = false;
    next_transient_disconnect_check_ = 0;
  }

  void StateMachine::replace_post_apply_stabilization_with_transient_disconnect_settlement() {
    // v1 cancels its ordinary delayed-reapply worker when the control pipe
    // breaks, then owns the rest of the settling period with the short
    // 250/750ms verify-before-repair ladder. Do the same without allowing a
    // queued 750ms ordinary check to delay or race the transient sequence.
    if (post_apply_check_pending_ ||
        (session_was_verified_ && state_ == State::Verification)) {
      system_.cancel_operations();
    }
    reset_post_apply_stabilization();
  }

  void StateMachine::rewind_post_apply_stabilization() {
    post_apply_check_pending_ = false;
    next_post_apply_stabilization_ = 0;
  }

  void StateMachine::schedule_next_post_apply_stabilization() {
    if (post_apply_check_pending_ || next_post_apply_stabilization_ >= post_apply_stabilization_delays_.size()) {
      return;
    }

    const auto delay = post_apply_stabilization_delays_[next_post_apply_stabilization_++];
    post_apply_check_pending_ = true;
    apply_.dispatch_verification(
      verification_request(),
      expected_topology_,
      resolved_target_,
      delay,
      VerificationPurpose::Stabilization);
  }

  bool StateMachine::dispatch_next_verification_reapply() {
    if (next_verification_reapply_ >= verification_reapply_delays_.size()) {
      return false;
    }

    const std::size_t stage = next_verification_reapply_++;
    const auto delay = verification_reapply_delays_[stage];
    if (current_request_.omit_final_initial_hdr_reapply &&
        !session_was_verified_ &&
        delay == std::chrono::milliseconds(5500)) {
      BOOST_LOG(warning) << "Display helper: omitting the final initial HDR reapply to release the stream-start capture gate.";
      return false;
    }
    // Stage zero is v1's immediate synchronous reapply. Every later stage is
    // also a completed/claimed post-apply settling window, so do not schedule
    // that same check again after the repair succeeds.
    next_post_apply_stabilization_ = std::max(next_post_apply_stabilization_, stage);
    BOOST_LOG(warning) << "Display helper: post-apply verification did not stick; reapplying settled display settings"
                       << (delay > std::chrono::milliseconds::zero() ? " after " + std::to_string(delay.count()) + " ms." : ".");
    transition(State::InProgress, ApplyAction::Apply, ApplyStatus::VerificationFailed);
    dispatch_apply_worker(current_request_, delay, false);
    return true;
  }

  bool StateMachine::dispatch_next_transient_disconnect_verification() {
    static constexpr std::array kDelays {
      std::chrono::milliseconds(250),
      std::chrono::milliseconds(750),
    };
    if (transient_disconnect_check_pending_) {
      return true;
    }
    if (!transient_disconnect_settlement_requested_ ||
        next_transient_disconnect_check_ >= kDelays.size() ||
        !current_request_.configuration) {
      reset_transient_disconnect_settlement();
      return false;
    }

    const auto delay = kDelays[next_transient_disconnect_check_++];
    transient_disconnect_check_pending_ = true;
    BOOST_LOG(info) << "Display helper: validating recent disconnected APPLY after " << delay.count()
                    << "ms before deciding whether a repair is needed.";
    transition(State::Verification, ApplyAction::Apply);
    apply_.dispatch_verification(
      verification_request(),
      expected_topology_,
      resolved_target_,
      delay,
      VerificationPurpose::TransientDisconnect);
    return true;
  }

  void StateMachine::dispatch_transient_disconnect_repair() {
    // The failed verification owns the decision. Cancel every remaining
    // delayed check before the repair so its old snapshot cannot race a new
    // topology/mode apply on the serialized dispatcher.
    system_.cancel_operations();
    transient_disconnect_check_pending_ = false;
    transient_disconnect_repair_in_flight_ = true;
    BOOST_LOG(warning) << "Display helper: recent disconnected APPLY did not stick; repairing now.";
    transition(State::InProgress, ApplyAction::Apply, ApplyStatus::VerificationFailed);
    dispatch_apply_worker(current_request_, std::chrono::milliseconds(0), false);
  }

  void StateMachine::dispatch_immediate_repair_after_stabilization_failure() {
    // The delayed verification that just failed already consumed its stage.
    // Reapply now, as v1's delayed worker did, and reserve the *next* delayed
    // stage if the repair's immediate verification still fails.
    next_verification_reapply_ = std::max(
      next_verification_reapply_,
      next_post_apply_stabilization_ + 1);
    BOOST_LOG(warning) << "Display helper: post-apply stabilization did not stick; reapplying settled display settings now.";
    transition(State::InProgress, ApplyAction::Apply, ApplyStatus::VerificationFailed);
    dispatch_apply_worker(current_request_, std::chrono::milliseconds(0), false);
  }

  void StateMachine::dispatch_apply_worker(
    const ApplyRequest &request,
    std::chrono::milliseconds delay,
    bool reset_virtual_display) {
    active_mutation_worker_ = ActiveMutationWorker {
      .kind = MutationWorkerKind::Apply,
      .generation = apply_.dispatch_apply(request, delay, reset_virtual_display),
    };
  }

  bool StateMachine::mutation_worker_active() const {
    return active_mutation_worker_.has_value();
  }

  bool StateMachine::owns_active_worker(MutationWorkerKind kind, std::uint64_t generation) const {
    return active_mutation_worker_ &&
           active_mutation_worker_->kind == kind &&
           active_mutation_worker_->generation == generation;
  }

  bool StateMachine::refresh_targets_active_request(const std::string &device_id) const {
    return current_request_.configuration &&
           (boost::iequals(current_request_.configuration->m_device_id, device_id) ||
            (current_request_.configuration->m_device_id.empty() &&
             resolved_target_ &&
             std::any_of(
               resolved_target_->duplicate_device_ids.begin(),
               resolved_target_->duplicate_device_ids.end(),
               [&device_id](const std::string &candidate) {
                 return boost::iequals(candidate, device_id);
               })));
  }

  void StateMachine::queue_after_active_mutation(
    DeferredMutationCommand command,
    const char *label,
    bool cancel_active_mutation) {
    // Cancellation is cooperative, but the active worker owns the durable
    // recovery boundary. Queue all commands that can replace or destroy that
    // state until its completion reports whether Windows was touched.
    if (cancel_active_mutation) {
      system_.cancel_operations();
    }
    enqueue_deferred_mutation_command(std::move(command), label);
  }

  void StateMachine::enqueue_deferred_mutation_command(
    DeferredMutationCommand command,
    const char *label) {
    if (!std::holds_alternative<ResetCommand>(command)) {
      // The queue is a recovery/mutation fence, not a historical command log.
      // Once an APPLY, REVERT, or DISARM is waiting behind that fence, a later
      // one expresses the newer desired session state and must supersede it.
      // Keeping the old FIFO intent could restore the desktop immediately
      // before a replacement APPLY, which is both slower and observably unlike
      // v1's synchronous cancel/join behavior.
      const auto old_end = deferred_mutation_commands_.end();
      const auto new_end = std::remove_if(
        deferred_mutation_commands_.begin(),
        old_end,
        [](const DeferredMutationCommand &queued) {
          return !std::holds_alternative<ResetCommand>(queued);
        });
      const bool superseded = new_end != old_end;
      deferred_mutation_commands_.erase(new_end, old_end);
      if (superseded) {
        BOOST_LOG(debug) << "Display helper: superseded an older deferred session intent with "
                         << label << '.';
      }
    }
    deferred_mutation_commands_.push_back(std::move(command));
    BOOST_LOG(info) << "Display helper: queuing " << label
                    << " behind the active display-mutation/reset barrier.";
  }

  bool StateMachine::has_deferred_apply_intent() const {
    return std::any_of(
      deferred_mutation_commands_.begin(),
      deferred_mutation_commands_.end(),
      [](const DeferredMutationCommand &command) {
        return std::holds_alternative<ApplyCommand>(command);
      });
  }

  void StateMachine::drain_deferred_mutation_commands() {
    while (!mutation_worker_active() && !staged_state_reset_pending_ && !deferred_mutation_commands_.empty()) {
      auto command = std::move(deferred_mutation_commands_.front());
      deferred_mutation_commands_.pop_front();
      std::visit([this](const auto &deferred) {
        using T = std::decay_t<decltype(deferred)>;
        if constexpr (std::is_same_v<T, ApplyCommand>) {
          handle_apply_command(deferred);
        } else if constexpr (std::is_same_v<T, RevertCommand>) {
          handle_revert_command(deferred);
        } else if constexpr (std::is_same_v<T, DisarmCommand>) {
          handle_disarm_command(deferred);
        } else {
          handle_reset_command(deferred);
        }
      }, command);
    }
  }

  void StateMachine::begin_staged_state_reset() {
    if (staged_state_reset_pending_) {
      return;
    }
    staged_state_reset_pending_ = true;
    apply_.dispatch_reset_staged_apply_state();
  }

  void StateMachine::retain_recovery_after_display_mutation(const ApplyCompleted &completed) {
    if (completed.durable_recovery_armed) {
      restore_task_created_ = true;
    }
    activate_recovery_lease();
    if (!restore_task_created_) {
      restore_task_created_ = system_.create_restore_task();
      if (!restore_task_created_) {
        BOOST_LOG(warning) << "Display helper v2: unable to arm a durable restore task after display mutation.";
      }
    }
  }

  void StateMachine::retain_recovery_after_cancelled_recovery() {
    // Recovery's snapshot stages can mutate topology, modes, HDR, primary,
    // and layout. A cancelled completion cannot tell us which boundary it
    // reached, so leave the existing restore lease/task in place until a
    // confirmed recovery or a subsequent verified APPLY takes ownership.
    recovery_armed_ = true;
    display_changes_pending_recovery_ = true;
    unconfirmed_cancelled_mutation_ = true;
    system_.arm_heartbeat();
  }

  void StateMachine::reset_uncommitted_staged_state_if_needed() {
    if (!staged_state_prepared_ || recovery_lease_before_apply_ || recovery_armed_ ||
        unconfirmed_cancelled_mutation_) {
      return;
    }

    if (restore_task_created_) {
      system_.delete_restore_task();
      restore_task_created_ = false;
    }
    begin_staged_state_reset();
  }

  ApplyRequest StateMachine::verification_request() const {
    // Keep the public request intact. Empty device_id is meaningful to
    // SettingsManager (the original primary duplicate group); the separate
    // resolved_target_ supplies the accepted topology's concrete scope.
    return current_request_;
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

  void StateMachine::set_apply_result_callback(std::function<void(ApplyStatus, std::uint64_t, std::uint64_t)> callback) {
    apply_result_callback_ = std::move(callback);
  }

  void StateMachine::set_verification_result_callback(std::function<void(bool, std::uint64_t, std::uint64_t)> callback) {
    verification_result_callback_ = std::move(callback);
  }

  void StateMachine::set_snapshot_result_callback(std::function<void(bool, std::uint64_t, std::uint64_t)> callback) {
    snapshot_result_callback_ = std::move(callback);
  }

  void StateMachine::set_refresh_rate_result_callback(std::function<void(bool, std::uint64_t, std::uint64_t)> callback) {
    refresh_rate_result_callback_ = std::move(callback);
  }

  void StateMachine::set_exit_callback(std::function<void(int)> callback) {
    exit_callback_ = std::move(callback);
  }

  void StateMachine::set_connection_epoch_provider(std::function<std::uint64_t()> provider) {
    connection_epoch_provider_ = std::move(provider);
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

  bool StateMachine::requires_disconnect_recovery() const {
    return recovery_armed_ || display_changes_pending_recovery_ || apply_in_flight() || restore_pending();
  }

  bool StateMachine::should_defer_disconnect_recovery() const {
    static constexpr auto kApplyDisconnectGrace = std::chrono::seconds(5);
    // The grace is only v1's post-APPLY startup churn accommodation.  An
    // explicit restore has its own durable intent, and a session that opted
    // out of restore-on-disconnect must disarm normally instead of issuing
    // additional display mutations after its control connection disappears.
    if (explicit_recovery_required_ ||
        !restore_state_.restore_on_disconnect.load(std::memory_order_acquire) ||
        !last_apply_started_) {
      return false;
    }
    const auto elapsed = system_.now() - *last_apply_started_;
    return elapsed >= std::chrono::steady_clock::duration::zero() && elapsed <= kApplyDisconnectGrace;
  }

  bool StateMachine::begin_transient_disconnect_settlement() {
    if (!should_defer_disconnect_recovery() || !current_request_.configuration) {
      return false;
    }
    if (transient_disconnect_settlement_requested_) {
      return true;
    }

    transient_disconnect_settlement_requested_ = true;
    // A recent disconnect replaces v1's normal delayed enforcement with its
    // short 250/750ms verify-before-repair sequence. Cancel an already queued
    // ordinary health check only after the active mutation is known to be
    // finished; cancelling a live APPLY would discard its authoritative
    // completion boundary.
    if (!mutation_worker_active()) {
      replace_post_apply_stabilization_with_transient_disconnect_settlement();
    }

    if (mutation_worker_active() || (!session_was_verified_ && state_ == State::Verification)) {
      // An in-flight APPLY or initial gate owns the next decision. Once it
      // completes, its terminal path starts the transient ladder instead of
      // re-entering the normal post-apply staircase.
      return true;
    }
    return dispatch_next_transient_disconnect_verification();
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
      } else if constexpr (std::is_same_v<T, RefreshRateCommand>) {
        handle_refresh_rate_command(payload);
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
      } else if constexpr (std::is_same_v<T, ResetCompleted>) {
        handle_reset_completed(payload);
      } else if constexpr (std::is_same_v<T, RefreshRateCompleted>) {
        handle_refresh_rate_completed(payload);
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
    if (is_stale_connection(command.connection_epoch)) {
      BOOST_LOG(debug) << "Display helper: ignoring APPLY from a retired IPC connection.";
      return;
    }
    // Apply describes the newest desired session even when it has to wait
    // behind a worker.  Clear older autonomous/explicit recovery intent at
    // ingress so a heartbeat cannot replace this deferred session start.
    explicit_recovery_required_ = false;
    reset_transient_disconnect_settlement();
    if (mutation_worker_active()) {
      queue_after_active_mutation(command, "APPLY");
      return;
    }
    if (staged_state_reset_pending_) {
      enqueue_deferred_mutation_command(command, "APPLY");
      return;
    }
    BOOST_LOG(info) << "Display helper: received Apply command"
                    << (command.request.configuration ? " with configuration" : " without configuration")
                    << ", prefer_golden_first=" << (command.request.prefer_golden_first ? "true" : "false")
                    << (command.request.virtual_layout ? ", virtual_layout=" + *command.request.virtual_layout : "");

    // A new APPLY supersedes any pending restore via IPC instead of forcing a
    // helper restart (72b0d996). Cancel in-flight work and disarm the scheduler.
    recovery_lease_before_apply_ = recovery_armed_ || display_changes_pending_recovery_;
    system_.cancel_operations();
    scheduler_.disarm();
    restore_state_.reset_request_progress();
    // A long staged transition must start with a fresh v1-compatible heartbeat
    // window rather than inherit an already-expiring monitor from a prior
    // session or pending recovery.
    system_.arm_heartbeat();

    apply_attempt_ = 1;
    virtual_hdr_fallback_attempted_ = false;
    apply_result_sent_ = false;
    verification_result_sent_ = false;
    session_was_verified_ = false;
    current_request_ = command.request;
    current_connection_epoch_ = command.connection_epoch;
    expected_topology_.reset();
    resolved_target_.reset();
    reset_post_apply_stabilization();

    if (current_request_.snapshot_exclusions) {
      update_blacklist(*current_request_.snapshot_exclusions);
    }

    if (!current_request_.configuration) {
      BOOST_LOG(error) << "Display helper: rejecting Apply command without a configuration.";
      send_apply_result(ApplyStatus::InvalidRequest);
      transition(State::Waiting, ApplyAction::Apply, ApplyStatus::InvalidRequest);
      return;
    }

    last_apply_started_ = system_.now();

    // v1 installs the recovery preferences with the request, before its
    // synchronous apply/restore join.  v2 queues a competing REVERT behind
    // the mutation worker, so publish the new policy before that queue can
    // run rather than waiting for successful verification.
    apply_recovery_policy_from_current_request();

    // Preserve v1's delayed stickiness checks as bounded FSM stages. The
    // immediate reapply is the legacy helper's second synchronous verify
    // attempt; later entries are its Windows-settling stair-step. HDR
    // transitions receive the longer 750ms/2.5s/5.5s envelope. The dispatcher
    // omits only the final initial repair for a capture-gated stream start;
    // retaining it here keeps the same 5.5s repair available after verification.
    verification_reapply_delays_.push_back(std::chrono::milliseconds(0));
    post_apply_stabilization_delays_.push_back(std::chrono::milliseconds(750));
    verification_reapply_delays_.push_back(std::chrono::milliseconds(750));
    if (current_request_.configuration->m_hdr_state) {
      post_apply_stabilization_delays_.push_back(std::chrono::milliseconds(2500));
      post_apply_stabilization_delays_.push_back(std::chrono::milliseconds(5500));
      verification_reapply_delays_.push_back(std::chrono::milliseconds(2500));
      verification_reapply_delays_.push_back(std::chrono::milliseconds(5500));
    }

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

    transition(State::InProgress, ApplyAction::Apply);
    dispatch_apply_worker(current_request_, std::chrono::milliseconds(0), false);
  }

  void StateMachine::handle_revert_command(const RevertCommand &command) {
    if (is_stale_connection(command.connection_epoch)) {
      BOOST_LOG(debug) << "Display helper: ignoring REVERT from a retired IPC connection.";
      return;
    }
    // Latch explicit restore intent at ingress, before it can be deferred
    // behind an APPLY/RESET worker. A later heartbeat is autonomous policy and
    // must not coalesce this user-requested restore away.
    if (!command.from_disconnect) {
      explicit_recovery_required_ = true;
      reset_transient_disconnect_settlement();
    }
    if (command.from_disconnect && recovery_worker_in_progress()) {
      // A broken/reconnected pipe may report the same disconnect more than
      // once. Preserve the live recovery worker rather than cancelling it
      // and restarting its five-second grace window.
      BOOST_LOG(debug) << "Display helper: joining existing disconnect recovery.";
      return;
    }
    if (mutation_worker_active()) {
      queue_after_active_mutation(command, "REVERT");
      return;
    }
    if (staged_state_reset_pending_) {
      enqueue_deferred_mutation_command(command, "REVERT");
      return;
    }
    // Disconnect-triggered reverts honor the restore-on-disconnect policy: a
    // paused stream with revert_on_disconnect=false must preserve its display
    // state (3b7a52c4 / 0add1f80). Explicit client REVERTs always run.
    if (command.from_disconnect &&
        !explicit_recovery_required_ &&
        !restore_state_.restore_on_disconnect.load(std::memory_order_acquire) &&
        !recovery_worker_in_progress()) {
      BOOST_LOG(info) << "Display helper: disconnect with restore-on-disconnect disabled; not restoring.";
      system_.cancel_operations();
      clear_recovery_state(true);
      reset_post_apply_stabilization();
      transition(State::Waiting, ApplyAction::Disarm);
      return;
    }

    // If there is nothing to restore from, exit rather than spinning (legacy
    // restore_poll_proc early-exit; keeps --restore boot tasks from hanging).
    if (!snapshots_.tier_exists(SnapshotTier::Current) &&
        !snapshots_.tier_exists(SnapshotTier::Previous) &&
        !snapshots_.tier_exists(SnapshotTier::Golden)) {
      BOOST_LOG(info) << "Restore: no session/previous or golden snapshot present; nothing to restore.";
      system_.cancel_operations();
      clear_recovery_state(true);
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
    display_changes_pending_recovery_ = true;
    // Heartbeats protect a verified, live session. A boot or disconnect
    // restore is owned by the restore scheduler and must not queue a second
    // recovery while Windows is settling the first one.
    system_.disarm_heartbeat();
    reset_post_apply_stabilization();

    // Give Sunshine a short window to immediately start a new session and DISARM,
    // avoiding costly restore/apply thrash during fast client switching. The boot
    // --restore path runs immediately.
    const auto grace = command.immediate ? std::chrono::milliseconds(0) : std::chrono::milliseconds(5000);
    scheduler_.arm_primary(system_.now(), grace);
    start_recovery(grace, ApplyAction::Revert);
  }

  void StateMachine::handle_disarm_command(const DisarmCommand &command) {
    if (is_stale_connection(command.connection_epoch)) {
      BOOST_LOG(debug) << "Display helper: ignoring DISARM from a retired IPC connection.";
      return;
    }
    // A restore attempt that has not been confirmed yet must not be cancelled
    // or overwritten by a later stream-start probe (72b0d996). Check this
    // before the generic mutation fence: otherwise DISARM would cancel the
    // recovery, then be ignored after its stale completion, leaving the state
    // machine stranded in Recovery with no worker.
    if (!command.force &&
        restore_pending() &&
        restore_state_.restore_attempted_unconfirmed.load(std::memory_order_acquire)) {
      BOOST_LOG(info) << "DISARM command ignored because an unconfirmed restore attempt is still pending.";
      return;
    }
    // Like APPLY, a later DISARM supersedes a previously requested explicit
    // recovery even if the command must wait behind an active mutation.
    explicit_recovery_required_ = false;
    reset_transient_disconnect_settlement();
    if (staged_state_reset_pending_) {
      enqueue_deferred_mutation_command(command, "DISARM");
      return;
    }
    if (mutation_worker_active()) {
      scheduler_.disarm();
      queue_after_active_mutation(command, "DISARM");
      return;
    }

    BOOST_LOG(info) << "Display helper: received Disarm command, resetting state";

    system_.cancel_operations();
    if (unconfirmed_cancelled_mutation_) {
      BOOST_LOG(warning) << "Display helper: keeping recovery guard after DISARM because a cancelled APPLY may have changed the desktop.";
    } else {
      clear_recovery_state(true);
    }
    apply_attempt_ = 0;
    apply_result_sent_ = false;
    verification_result_sent_ = false;
    session_was_verified_ = false;
    last_apply_started_.reset();
    if (!unconfirmed_cancelled_mutation_) {
      restore_task_created_ = false;
    }
    expected_topology_.reset();
    resolved_target_.reset();
    reset_post_apply_stabilization();

    transition(State::Waiting, ApplyAction::Disarm);
  }

  void StateMachine::handle_export_golden(const ExportGoldenCommand &command) {
    if (is_stale_connection(command.connection_epoch)) {
      BOOST_LOG(debug) << "Display helper: ignoring EXPORT_GOLDEN from a retired IPC connection.";
      return;
    }
    if (apply_in_flight() || restore_pending()) {
      BOOST_LOG(info) << "Skipping golden snapshot export while display state is changing.";
      return;
    }

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
    if (is_stale_connection(command.connection_epoch)) {
      BOOST_LOG(debug) << "Display helper: ignoring SNAPSHOT_CURRENT from a retired IPC connection.";
      return;
    }
    // Never overwrite the restore baseline while a restore is being worked on
    // (72b0d996): the snapshot would capture the un-restored state.
    if (restore_pending() || apply_in_flight()) {
      BOOST_LOG(info) << "Skipping current session snapshot refresh while display state is changing.";
      send_snapshot_result(false, command.connection_epoch, command.request_id);
      return;
    }

    if (command.payload.update_exclusions || !command.payload.exclude_devices.empty()) {
      update_blacklist(command.payload.exclude_devices);
    }

    const bool saved = snapshots_.refresh_current_preserving_previous(exclusions_vector());
    send_snapshot_result(saved, command.connection_epoch, command.request_id);
  }

  void StateMachine::handle_refresh_rate_command(const RefreshRateCommand &command) {
    if (is_stale_connection(command.connection_epoch)) {
      BOOST_LOG(debug) << "Display helper: ignoring refresh-rate request from a retired IPC connection.";
      return;
    }
    if (command.device_id.empty() || command.numerator == 0 || command.denominator == 0) {
      send_refresh_rate_result(false, command.connection_epoch, command.request_id);
      return;
    }
    if (apply_in_flight() || restore_pending()) {
      // Do not let an adaptive refresh command race the topology/configuration
      // transaction or a scheduled restore that owns the device. The caller
      // can retry once the mutation reaches a steady state.
      send_refresh_rate_result(false, command.connection_epoch, command.request_id);
      return;
    }
    if (post_apply_check_pending_ && !refresh_targets_active_request(command.device_id)) {
      // A stabilization verification owns the current session's display
      // contract. Do not let an unrelated refresh mutate another device while
      // that read/repair decision is pending; the caller can retry afterward.
      BOOST_LOG(debug) << "Display helper: rejecting unrelated refresh-rate mutation while post-apply verification is pending.";
      send_refresh_rate_result(false, command.connection_epoch, command.request_id);
      return;
    }
    if (post_apply_check_pending_) {
      // A stabilization verification carries a copy of the pre-refresh
      // request. If it completes after this refresh is dispatched, a failed
      // result would otherwise start a full APPLY with the old refresh rate
      // and replace the refresh worker's ownership fence. Invalidate that
      // verification before the refresh mutation is queued; its completion
      // will then be ignored as stale.
      BOOST_LOG(debug) << "Display helper: cancelling pending post-apply verification before refresh-rate mutation.";
      system_.cancel_operations();
      rewind_post_apply_stabilization();
    }
    // Refresh-rate changes are a display mutation in their own right.  Give
    // them the same ownership fence as APPLY/REVERT so a replacement session
    // cannot begin while Windows is still committing the rate change.
    active_mutation_worker_ = ActiveMutationWorker {
      .kind = MutationWorkerKind::RefreshRate,
      .generation = apply_.dispatch_refresh_rate(command),
    };
  }

  void StateMachine::handle_reset_command(const ResetCommand &command) {
    if (is_stale_connection(command.connection_epoch)) {
      BOOST_LOG(debug) << "Display helper: ignoring RESET from a retired IPC connection.";
      return;
    }
    if (staged_state_reset_pending_) {
      enqueue_deferred_mutation_command(command, "RESET");
      return;
    }
    // Match v1 RESET semantics: it resets SettingsManager persistence only.
    // It must serialize behind a display mutation, but must not cancel that
    // mutation or tear down the live session's recovery lease/task.
    if (mutation_worker_active()) {
      queue_after_active_mutation(command, "RESET", false);
      return;
    }

    BOOST_LOG(info) << "Display helper: received RESET command; resetting staged settings persistence.";
    begin_staged_state_reset();
  }

  void StateMachine::handle_ping_command(const PingCommand &command) {
    if (is_stale_connection(command.connection_epoch)) {
      return;
    }
    system_.record_ping();
  }

  void StateMachine::handle_stop_command(const StopCommand &command) {
    if (is_stale_connection(command.connection_epoch)) {
      return;
    }
    BOOST_LOG(info) << "Display helper: received STOP command, exiting gracefully.";
    if (exit_callback_) {
      exit_callback_(0);
    }
  }

  void StateMachine::handle_apply_completed(const ApplyCompleted &completed) {
    const bool completed_owns_active_worker = owns_active_worker(MutationWorkerKind::Apply, completed.generation);
    if (completed_owns_active_worker) {
      // Retire the worker fence before stale-generation handling. A control
      // command may have cancelled this worker, but its completion is still
      // the authoritative source for whether the desktop/task was touched.
      active_mutation_worker_.reset();
      staged_state_prepared_ = staged_state_prepared_ || completed.staged_state_prepared;
    }

    if (is_stale(completed.generation)) {
      if (completed_owns_active_worker) {
        if (completed.display_may_have_changed) {
          retain_recovery_after_display_mutation(completed);
          unconfirmed_cancelled_mutation_ = true;
          system_.arm_heartbeat();
          BOOST_LOG(warning) << "Display helper: cancelled APPLY had entered a display mutation; preserving recovery state.";
        } else if (completed.durable_recovery_armed) {
          // A guard may have been created just before cancellation. Keep it
          // until the queued command decides whether a new transaction will
          // reuse it or a DISARM/RESET can safely remove it.
          restore_task_created_ = true;
        }
        if (!completed.display_may_have_changed) {
          reset_uncommitted_staged_state_if_needed();
        }
        drain_deferred_mutation_commands();
      }
      return;
    }

    if (!completed_owns_active_worker) {
      BOOST_LOG(debug) << "Display helper: ignoring APPLY completion without an active worker fence.";
      return;
    }

    expected_topology_ = completed.expected_topology;
    resolved_target_ = completed.resolved_target;

    if (completed.status == ApplyStatus::Ok || completed.display_may_have_changed) {
      activate_recovery_lease();
    }

    if (completed.durable_recovery_armed) {
      restore_task_created_ = true;
    }

    // A topology stage can succeed before a later SettingsManager operation
    // reports failure. Install the recovery task at that first mutation
    // boundary, while still avoiding task creation for requests rejected during
    // preflight.
    if ((completed.status == ApplyStatus::Ok || completed.display_may_have_changed) && !restore_task_created_) {
      restore_task_created_ = system_.create_restore_task();
      if (!restore_task_created_) {
        BOOST_LOG(warning) << "Display helper v2: could not arm a durable restore task after display mutation.";
      }
    }

    if (completed.status == ApplyStatus::Ok) {
      if (transient_disconnect_repair_in_flight_) {
        // v1's disconnected settle worker applies immediately after a failed
        // check, then waits for the *next* 250/750ms slot before checking
        // again. Do not reopen the normal initial/750ms staircase here.
        transient_disconnect_repair_in_flight_ = false;
        if (dispatch_next_transient_disconnect_verification()) {
          return;
        }

        // The final v1 repair has no following probe. Keep the recovered
        // session/virtual monitor alive rather than treating an absent control
        // response lane as a new apply failure.
        session_was_verified_ = true;
        activate_recovery_lease();
        unconfirmed_cancelled_mutation_ = false;
        enter_steady_state();
        drain_deferred_mutation_commands();
        return;
      }

      send_apply_result(completed.status);
      transition(State::Verification, ApplyAction::Apply, completed.status);
      apply_.dispatch_verification(
        verification_request(),
        expected_topology_,
        resolved_target_,
        std::chrono::milliseconds(0),
        VerificationPurpose::Initial);
      // RESET is intentionally non-cancelling, mirroring v1's synchronous
      // reset-persistence request. It can run now that SettingsManager has
      // finished mutating, while verification remains read-only.
      drain_deferred_mutation_commands();
      return;
    }

    if (completed.status == ApplyStatus::NeedsVirtualDisplayReset) {
      const auto decision = apply_.maybe_reset_virtual_display(
        completed.status,
        completed.virtual_display_requested);
      if (decision == PolicyDecision::ResetVirtualDisplay) {
        dispatch_apply_worker(current_request_, std::chrono::milliseconds(0), true);
        return;
      }
    }

    if (transient_disconnect_settlement_requested_) {
      transient_disconnect_repair_in_flight_ = false;
      if (dispatch_next_transient_disconnect_verification()) {
        return;
      }
    }

    const bool status_allows_bounded_retry =
      completed.status == ApplyStatus::Retryable ||
      completed.status == ApplyStatus::VerificationFailed ||
      completed.status == ApplyStatus::HdrStateFailed;
    if (status_allows_bounded_retry) {
      if (apply_.can_retry(apply_attempt_)) {
        const auto delay = apply_.retry_delay(apply_attempt_);
        ++apply_attempt_;
        dispatch_apply_worker(current_request_, delay, false);
        return;
      }
    }

    const bool can_fallback_virtual_hdr =
      !virtual_hdr_fallback_attempted_ &&
      completed.virtual_display_requested &&
      current_request_.configuration &&
      current_request_.configuration->m_hdr_state == display_device::HdrState::Enabled &&
      status_allows_bounded_retry;
    if (can_fallback_virtual_hdr) {
      virtual_hdr_fallback_attempted_ = true;
      current_request_.configuration->m_hdr_state = display_device::HdrState::Disabled;
      apply_attempt_ = 1;
      BOOST_LOG(warning) << "Display helper: virtual-display HDR remained unavailable after bounded retries; applying topology and mode with effective SDR.";
      transition(State::InProgress, ApplyAction::Apply, completed.status);
      dispatch_apply_worker(current_request_, std::chrono::milliseconds(0), false);
      return;
    }

    send_apply_result(completed.status);
    if (apply_result_sent_ && !verification_result_sent_) {
      send_verification_result(false);
    }

    if (!recovery_armed_) {
      system_.disarm_heartbeat();
    }

    // prepare_staged_apply latches the original primary/mode/HDR baseline
    // before Windows is touched. A terminal first-transaction failure that
    // never changed (or fully rolled back) the desktop must not leave that
    // provisional state to steer an unrelated later APPLY. The latch spans
    // retries and stale completions, not merely this last worker result.
    if (!completed.display_may_have_changed) {
      reset_uncommitted_staged_state_if_needed();
    }

    // Once the client has received a successful verification gate, later
    // settling repairs are best effort.  They must not tear down virtual
    // display monitoring or retract that already-confirmed session merely
    // because Windows rejected a redundant repair.
    if (session_was_verified_) {
      BOOST_LOG(warning) << "Display helper: stabilization repair failed after the session was verified; preserving the live recovery lease.";
      enter_steady_state();
      drain_deferred_mutation_commands();
      return;
    }

    transition(State::Waiting, ApplyAction::Apply, completed.status);
    drain_deferred_mutation_commands();
  }

  void StateMachine::handle_verification_completed(const VerificationCompleted &completed) {
    if (is_stale(completed.generation)) {
      return;
    }

    const bool stabilization_check = completed.purpose == VerificationPurpose::Stabilization;
    const bool transient_disconnect_check = completed.purpose == VerificationPurpose::TransientDisconnect;
    if (stabilization_check) {
      post_apply_check_pending_ = false;
    }
    if (transient_disconnect_check) {
      transient_disconnect_check_pending_ = false;
    }

    if (transient_disconnect_check) {
      if (completed.success) {
        // The pipe is already gone, so this is a local health decision rather
        // than a response gate. A sticky configuration is enough to retain
        // the recovery lease and virtual-display monitoring without another
        // modeset or an unanswerable heartbeat.
        if (!session_was_verified_) {
          session_was_verified_ = true;
          activate_recovery_lease();
          unconfirmed_cancelled_mutation_ = false;
        }
        reset_transient_disconnect_settlement();
        enter_steady_state();
      } else if (current_request_.configuration) {
        dispatch_transient_disconnect_repair();
      } else {
        reset_transient_disconnect_settlement();
        transition(State::Waiting, ApplyAction::Apply, ApplyStatus::InvalidRequest);
      }
      return;
    }

    if (completed.success) {
      if (!stabilization_check) {
        const bool initial_verification = !verification_result_sent_;
        const bool disconnected_settlement = transient_disconnect_settlement_requested_;
        if (initial_verification) {
          session_was_verified_ = true;
          activate_recovery_lease();
          unconfirmed_cancelled_mutation_ = false;
          // The helper has already observed the pipe break. Do not arm a
          // heartbeat that no client can refresh while its v1-style settling
          // checks are still responsible for the session.
          if (!disconnected_settlement) {
            system_.arm_heartbeat();
          }
          system_.refresh_shell();
          // Verification is also the host's capture-start gate. Publish it
          // only after dispatching the shell display-change refresh so the
          // repaint request cannot lag behind capture startup.
          send_verification_result(true);
          // wa_hdr_toggle is an explicitly requested workaround. Running it on
          // every successful APPLY was an unnecessary monitor off/on cycle.
          if (current_request_.hdr_blank) {
            system_.blank_hdr_states(std::chrono::milliseconds(1000));
          }
        }

        if (transient_disconnect_settlement_requested_) {
          if (dispatch_next_transient_disconnect_verification()) {
            return;
          }
        } else {
          schedule_next_post_apply_stabilization();
        }

        // A repair APPLY has already emitted the client gate result, but it
        // must still return from InProgress/Verification to the same steady
        // state as the initial transaction. Otherwise virtual monitoring and
        // snapshots remain permanently blocked after a successful repair.
        enter_steady_state();
        return;
      }

      if (transient_disconnect_settlement_requested_) {
        if (dispatch_next_transient_disconnect_verification()) {
          return;
        }
        // The ordinary check completed just as disconnect settlement was
        // requested. The transient envelope has either taken ownership or
        // been exhausted; restore the pre-existing live session state.
        enter_steady_state();
      } else {
        schedule_next_post_apply_stabilization();
      }
      return;
    }

    if (stabilization_check) {
      if (transient_disconnect_settlement_requested_) {
        if (dispatch_next_transient_disconnect_verification()) {
          return;
        }
        if (session_was_verified_) {
          enter_steady_state();
        } else {
          transition(State::Waiting, ApplyAction::Apply, ApplyStatus::VerificationFailed);
        }
        return;
      }
      dispatch_immediate_repair_after_stabilization_failure();
      return;
    }

    if (transient_disconnect_settlement_requested_) {
      if (dispatch_next_transient_disconnect_verification()) {
        return;
      }
      if (session_was_verified_) {
        enter_steady_state();
      } else {
        transition(State::Waiting, ApplyAction::Apply, ApplyStatus::VerificationFailed);
      }
      return;
    }

    rewind_post_apply_stabilization();
    if (dispatch_next_verification_reapply()) {
      return;
    }

    if (session_was_verified_) {
      // This was the immediate verification of a best-effort stabilization
      // repair.  v1 left the already-running session alive here; retain that
      // behavior without collapsing the v2 FSM back into background workers.
      BOOST_LOG(warning) << "Display helper: stabilization repair did not verify; keeping the previously verified session active.";
      enter_steady_state();
      return;
    }

    send_verification_result(false);
    transition(State::Waiting, ApplyAction::Apply, ApplyStatus::VerificationFailed);
  }

  void StateMachine::handle_reset_completed(const ResetCompleted &completed) {
    // The backend reset is deliberately non-cancellable once it reaches the
    // dispatcher. Always retire its barrier, even if a heartbeat or another
    // command advanced the cancellation generation while it was queued.
    const bool was_stale = is_stale(completed.generation);
    staged_state_reset_pending_ = false;
    if (completed.success) {
      staged_state_prepared_ = false;
    }
    if (completed.success) {
      BOOST_LOG(info) << "Display helper: reset staged SettingsManager state result=true"
                      << (was_stale ? " (completion followed a newer cancellation generation)" : "");
    } else {
      BOOST_LOG(warning) << "Display helper: reset staged SettingsManager state result=false"
                         << (was_stale ? " (completion followed a newer cancellation generation)" : "");
    }

    const bool deferred_followup_mutation = std::any_of(
      deferred_mutation_commands_.begin(),
      deferred_mutation_commands_.end(),
      [](const DeferredMutationCommand &command) {
        return !std::holds_alternative<ResetCommand>(command);
      });
    if (exit_after_staged_state_reset_ && !completed.success) {
      // Do not drain a queued APPLY into a backend whose stale state could not
      // be cleared. Retire this helper first; the next connection receives a
      // fresh SettingsManager instance.
      exit_after_staged_state_reset_ = false;
      if (exit_callback_) {
        exit_callback_(1);
      }
      return;
    }
    if (exit_after_staged_state_reset_ && deferred_followup_mutation) {
      // A new control transaction superseded the restore's normal helper-exit
      // path while RESET was queued. Let that command own the next lifecycle.
      exit_after_staged_state_reset_ = false;
    }
    drain_deferred_mutation_commands();
    if (exit_after_staged_state_reset_ && !staged_state_reset_pending_) {
      exit_after_staged_state_reset_ = false;
      if (exit_callback_) {
        exit_callback_(0);
      }
    }
  }

  void StateMachine::handle_refresh_rate_completed(const RefreshRateCompleted &completed) {
    const bool completed_owns_active_worker = owns_active_worker(MutationWorkerKind::RefreshRate, completed.generation);
    if (completed_owns_active_worker) {
      active_mutation_worker_.reset();
    }
    if (is_stale(completed.generation)) {
      if (completed_owns_active_worker) {
        drain_deferred_mutation_commands();
      }
      return;
    }
    if (!completed_owns_active_worker) {
      BOOST_LOG(debug) << "Display helper: ignoring refresh-rate completion without an active mutation worker.";
      return;
    }

    if (refresh_targets_active_request(completed.device_id)) {
      if (completed.success) {
        // Adaptive refresh is part of the active transaction, not an unrelated
        // side effect. A later virtual-display recreation/reapply must preserve
        // the refresh rate that Sunshine just confirmed.
        current_request_.configuration->m_refresh_rate = display_device::Rational {
          completed.numerator,
          completed.denominator,
        };
      }
      // Delayed verifications hold a copy of the prior request. Cancel that
      // generation and restart the v1-style settling envelope. This is needed
      // after both success and failure because a failed refresh may still have
      // changed Windows state before reporting its result.
      system_.cancel_operations();
      rewind_post_apply_stabilization();
      if (transient_disconnect_settlement_requested_) {
        replace_post_apply_stabilization_with_transient_disconnect_settlement();
      } else {
        schedule_next_post_apply_stabilization();
      }
    }
    if (transient_disconnect_settlement_requested_) {
      (void) dispatch_next_transient_disconnect_verification();
    }
    send_refresh_rate_result(completed.success, completed.connection_epoch, completed.request_id);
    drain_deferred_mutation_commands();
  }

  void StateMachine::handle_recovery_completed(const RecoveryCompleted &completed) {
    const bool completed_owns_active_worker = owns_active_worker(MutationWorkerKind::Recovery, completed.generation);
    if (is_stale(completed.generation)) {
      if (completed_owns_active_worker) {
        active_mutation_worker_.reset();
        if (completed.display_may_have_changed) {
          retain_recovery_after_cancelled_recovery();
          BOOST_LOG(warning) << "Display helper: cancelled recovery may have changed the desktop; preserving recovery state.";
        } else {
          // Cancellation during AsyncDispatcher's grace period never entered
          // RecoveryOperation, so DISARM/APPLY may safely take over.
          restore_state_.reset_request_progress();
          BOOST_LOG(debug) << "Display helper: cancelled recovery before its mutation boundary.";
        }
        drain_deferred_mutation_commands();
      }
      return;
    }
    if (!completed_owns_active_worker) {
      BOOST_LOG(debug) << "Display helper: ignoring recovery completion without an active mutation worker.";
      return;
    }

    BOOST_LOG(info) << "Display helper: recovery operation completed, success=" << (completed.success ? "true" : "false")
                    << ", has_snapshot=" << (completed.snapshot ? "true" : "false");

    if (completed.success && completed.snapshot) {
      if (completed.staged_state_reset_attempted) {
        recovery_staged_state_reset_succeeded_ = completed.staged_state_reset_succeeded;
      }
      recovery_snapshot_ = completed.snapshot;
      transition(State::RecoveryValidation, ApplyAction::Revert);
      active_mutation_worker_->generation = recovery_.dispatch_recovery_validation(*recovery_snapshot_);
      return;
    }

    active_mutation_worker_.reset();
    BOOST_LOG(warning) << "Display helper: recovery failed or no valid snapshot found, entering event loop";
    scheduler_.on_attempt_failed(system_.now());
    transition(State::EventLoop, ApplyAction::Revert);
    drain_deferred_mutation_commands();
  }

  void StateMachine::handle_recovery_validation_completed(const RecoveryValidationCompleted &completed) {
    const bool completed_owns_active_worker = owns_active_worker(MutationWorkerKind::Recovery, completed.generation);
    if (is_stale(completed.generation)) {
      if (completed_owns_active_worker) {
        active_mutation_worker_.reset();
        retain_recovery_after_cancelled_recovery();
        BOOST_LOG(warning) << "Display helper: cancelled recovery validation left the recovery lease armed.";
        drain_deferred_mutation_commands();
      }
      return;
    }
    if (!completed_owns_active_worker) {
      BOOST_LOG(debug) << "Display helper: ignoring recovery validation without an active mutation worker.";
      return;
    }

    active_mutation_worker_.reset();

    if (completed.success) {
      BOOST_LOG(info) << "Display helper: recovery validation succeeded, display settings restored.";
      recovery_armed_ = false;
      display_changes_pending_recovery_ = false;
      unconfirmed_cancelled_mutation_ = false;
      scheduler_.disarm();
      restore_state_.reset_request_progress();
      system_.disarm_heartbeat();
      system_.refresh_shell();
      system_.delete_restore_task();
      restore_task_created_ = false;
      const bool staged_state_reset_failed =
        staged_state_prepared_ &&
        recovery_staged_state_reset_succeeded_.has_value() &&
        !*recovery_staged_state_reset_succeeded_;
      if (!staged_state_reset_failed) {
        staged_state_prepared_ = false;
      }
      // Return to Waiting before signalling completion: the host may keep the
      // helper alive when a newer connection is active (72b0d996).
      transition(State::Waiting, ApplyAction::Revert, ApplyStatus::Ok);
      if (staged_state_reset_failed) {
        BOOST_LOG(warning) << "Display helper: recovery restored the desktop but could not clear staged SettingsManager state; retrying reset before accepting another APPLY.";
        exit_after_staged_state_reset_ = true;
        begin_staged_state_reset();
      }
      const bool deferred_followup_mutation = std::any_of(
        deferred_mutation_commands_.begin(),
        deferred_mutation_commands_.end(),
        [](const DeferredMutationCommand &command) {
          return !std::holds_alternative<ResetCommand>(command);
        });
      const bool reset_queued_after_recovery =
        staged_state_reset_pending_ ||
        std::any_of(
          deferred_mutation_commands_.begin(),
          deferred_mutation_commands_.end(),
          [](const DeferredMutationCommand &command) {
            return std::holds_alternative<ResetCommand>(command);
          });
      exit_after_staged_state_reset_ = reset_queued_after_recovery;
      drain_deferred_mutation_commands();
      if (deferred_followup_mutation && !staged_state_reset_failed) {
        // A queued APPLY/DISARM/REVERT is the newer transaction. It must not
        // be followed by the restore's automatic helper exit, even when the
        // IPC connection epoch did not change.
        exit_after_staged_state_reset_ = false;
        return;
      }
      if (exit_after_staged_state_reset_) {
        // RESET is asynchronous in v2, unlike v1's in-thread persistence
        // reset. Keep the helper alive until its ordered completion arrives.
        return;
      }
      if (exit_callback_) {
        exit_callback_(0);
      }
      return;
    }

    BOOST_LOG(warning) << "Display helper: recovery validation failed, entering event loop for retry.";
    scheduler_.on_attempt_failed(system_.now());
    transition(State::EventLoop, ApplyAction::Revert);
    drain_deferred_mutation_commands();
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
      const std::string resolved = current_request_.configuration ? virtual_display_.device_id() : std::string {};
      const bool same_virtual_device = current_request_.configuration &&
                                       !resolved.empty() &&
                                       boost::iequals(current_request_.configuration->m_device_id, resolved);
      if (mutation_worker_active()) {
        // RefreshRate intentionally keeps the monitoring state while its
        // Windows mutation is running. Do not let an IDD event overwrite that
        // fence with an APPLY. A changed ID needs a queued repair; a same-ID
        // event is covered by the refresh completion's stabilization check.
        if (!same_virtual_device) {
          retarget_virtual_display_device_id_if_needed();
          queue_after_active_mutation(
            ApplyCommand {
              .request = current_request_,
              .generation = system_.current_generation(),
              .connection_epoch = current_connection_epoch_,
            },
            "virtual-display APPLY restart during mutation");
        } else {
          BOOST_LOG(debug) << "Display helper: same-id virtual display event deferred while a display mutation is active.";
        }
        return;
      }
      if (same_virtual_device) {
          // Refresh-only changes and a same-ID IDD reset look identical at the
          // device-id layer. Verify first so we preserve adaptive refresh when
          // healthy but repair a reset that silently lost topology/mode/HDR.
          if (!post_apply_check_pending_) {
            BOOST_LOG(debug) << "Display helper: validating same-id virtual display event.";
            post_apply_check_pending_ = true;
            apply_.dispatch_verification(
              verification_request(),
              expected_topology_,
              resolved_target_,
              std::chrono::milliseconds(0),
              VerificationPurpose::Stabilization);
          }
          return;
      }

      BOOST_LOG(info) << "Display helper: display event while monitoring virtual display, re-applying configuration.";
      system_.cancel_operations();
      retarget_virtual_display_device_id_if_needed();
      apply_attempt_ = 1;
      rewind_post_apply_stabilization();
      transition(State::InProgress, ApplyAction::Apply);
      dispatch_apply_worker(current_request_, std::chrono::milliseconds(0), false);
      return;
    }

    // During active apply with virtual display, restart the apply operation
    if ((state_ == State::InProgress || state_ == State::Verification) &&
        current_request_.virtual_layout.has_value()) {
      if (current_request_.configuration) {
        const std::string resolved = virtual_display_.device_id();
        if (!resolved.empty() && boost::iequals(current_request_.configuration->m_device_id, resolved)) {
          // The in-flight verification will decide whether this was a benign
          // refresh change or an IDD reset; do not restart blindly.
          BOOST_LOG(debug) << "Display helper: display event during virtual display apply deferred to verification.";
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
      if (mutation_worker_active()) {
        retarget_virtual_display_device_id_if_needed();
        queue_after_active_mutation(
          ApplyCommand {
            .request = current_request_,
            .generation = system_.current_generation(),
            .connection_epoch = current_connection_epoch_,
          },
          "virtual-display APPLY restart");
        return;
      }
      system_.cancel_operations();
      apply_attempt_ = 1;
      expected_topology_.reset();
      rewind_post_apply_stabilization();
      retarget_virtual_display_device_id_if_needed();
      transition(State::InProgress, ApplyAction::Apply);
      dispatch_apply_worker(current_request_, kRestartDelay, false);
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
    if (is_stale_connection(event.connection_epoch)) {
      BOOST_LOG(debug) << "Display helper: ignoring heartbeat event from a retired IPC connection.";
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

    if (state_ == State::Recovery || state_ == State::RecoveryValidation) {
      // Recovery owns this display mutation already. A heartbeat cannot make
      // it safer to interrupt an in-flight restore; let its completion decide
      // whether to retry through the scheduler.
      BOOST_LOG(debug) << "Display helper: heartbeat joined existing recovery.";
      return;
    }

    if (mutation_worker_active()) {
      if (has_deferred_apply_intent()) {
        // A newer connection has already requested a replacement session.
        // Do not let an autonomous heartbeat coalesce it into recovery.
        BOOST_LOG(debug) << "Display helper: heartbeat deferring to queued APPLY intent.";
        return;
      }
      if (!explicit_recovery_required_ &&
          !restore_state_.restore_on_disconnect.load(std::memory_order_acquire)) {
        queue_after_active_mutation(
          DisarmCommand {
            .connection_epoch = event.connection_epoch,
          },
          "heartbeat DISARM");
      } else {
        queue_after_active_mutation(
          RevertCommand {
            .connection_epoch = event.connection_epoch,
            .immediate = true,
            .from_disconnect = true,
          },
          "heartbeat REVERT");
      }
      return;
    }

    // Heartbeat loss means Sunshine crashed/hung: honor the restore-on-disconnect
    // policy the same way a broken pipe would (3b7a52c4).
    if (!explicit_recovery_required_ &&
        !restore_state_.restore_on_disconnect.load(std::memory_order_acquire) &&
        !recovery_worker_in_progress()) {
      BOOST_LOG(info) << "Display helper: heartbeat lost with restore-on-disconnect disabled; not restoring.";
      system_.cancel_operations();
      clear_recovery_state(true);
      reset_post_apply_stabilization();
      transition(State::Waiting, ApplyAction::Disarm);
      return;
    }

    BOOST_LOG(info) << "Display helper: initiating recovery due to heartbeat timeout";
    system_.cancel_operations();
    system_.disarm_heartbeat();
    reset_post_apply_stabilization();
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

  bool StateMachine::recovery_worker_in_progress() const {
    return state_ == State::Recovery || state_ == State::RecoveryValidation;
  }

  bool StateMachine::apply_in_flight() const {
    return mutation_worker_active() ||
           state_ == State::InProgress || state_ == State::Verification;
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

  bool StateMachine::is_stale_connection(std::uint64_t connection_epoch) const {
    return connection_epoch != 0 && connection_epoch_provider_ &&
           connection_epoch != connection_epoch_provider_();
  }
}  // namespace display_helper::v2
