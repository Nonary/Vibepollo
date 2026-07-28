#pragma once

#include "src/platform/windows/display_helper_v2/golden_health.h"
#include "src/platform/windows/display_helper_v2/interfaces.h"
#include "src/platform/windows/display_helper_v2/runtime_support.h"
#include "src/platform/windows/display_helper_v2/snapshot.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace display_helper::v2 {
  enum class TopologyValidationMode {
    StrictApply,
    StructuralRestore,
  };

  struct TopologyTransitionOutcome {
    ApplyStatus status = ApplyStatus::Fatal;
    std::optional<ActiveTopology> applied_topology;
    /// True once a topology activation or display-stack recovery has been
    /// attempted. Even a failing Windows call can leave a partial change.
    bool display_may_have_changed = false;
    /// Set when the caller's durable recovery guard was armed before the
    /// first potentially mutating topology operation.
    bool durable_recovery_armed = false;
  };

  /**
   * @brief Staged Windows topology transition shared by APPLY and REVERT.
   *
   * Windows may return from SetDisplayConfig before newly activated paths are
   * usable by the mode/HDR APIs. This component owns the bounded validation,
   * recovery, retry, and enumeration wait required before later stages run.
   */
  class TopologyTransition {
  public:
    using MutationBoundary = std::function<bool()>;

    TopologyTransition(IDisplaySettings &display, IClock &clock);

    TopologyTransitionOutcome run(
      const ActiveTopology &topology,
      TopologyValidationMode validation_mode,
      const TopologyActivationTarget &activation_target,
      const CancellationToken &token,
      const MutationBoundary &mutation_boundary = {}
    );

  private:
    std::optional<ActiveTopology> topology_ready(
      const ActiveTopology &requested_topology,
      const TopologyActivationTarget &activation_target,
      bool allow_os_adjustment);
    std::optional<ActiveTopology> wait_until_ready(
      const ActiveTopology &requested_topology,
      const TopologyActivationTarget &activation_target,
      bool allow_os_adjustment,
      const CancellationToken &token);
    bool recover_and_settle(const CancellationToken &token);
    bool wait_with_cancel(std::chrono::milliseconds duration, const CancellationToken &token);

    IDisplaySettings &display_;
    IClock &clock_;

    static constexpr int kMaxTopologyAttempts = 2;
    static constexpr std::chrono::milliseconds kActivationTimeout {5000};
    static constexpr std::chrono::milliseconds kActivationPollInterval {100};
    static constexpr std::chrono::milliseconds kRecoverySettleDelay {500};
  };

  struct ApplyOutcome {
    ApplyStatus status = ApplyStatus::Fatal;
    std::optional<ActiveTopology> expected_topology;
    /// Target/group context derived from the topology Windows ultimately
    /// accepted. It deliberately does not rewrite the public configuration.
    std::optional<ResolvedConfigurationTarget> resolved_target;
    bool virtual_display_requested = false;
    /// A topology/display-stack operation or later settings stage may have
    /// changed the desktop, so recovery remains required on failure.
    bool display_may_have_changed = false;
    /// Durable recovery was armed synchronously at the first mutation
    /// boundary, before SettingsManager/mode/HDR work can continue.
    bool durable_recovery_armed = false;
    /// True once the backend captured the provisional session baseline used
    /// for consecutive APPLY operations. A terminal no-mutation failure must
    /// discard this provisional state before an unrelated later session.
    bool staged_state_prepared = false;
  };

  struct RecoveryOutcome {
    bool success = false;
    std::optional<Snapshot> snapshot;
    /// True after recovery left its cancellable grace period and began a
    /// restore transaction. A cancelled transaction must be treated as having
    /// possibly changed the desktop even if it has not confirmed a snapshot.
    bool display_may_have_changed = false;
    /// A confirmed restore clears the SettingsManager state retained for a
    /// staged APPLY session. Keep the result explicit so the state machine
    /// never treats a failed backend reset as a clean session.
    bool staged_state_reset_attempted = false;
    bool staged_state_reset_succeeded = false;
  };

  /**
   * @brief Shared restore-engine state set by command handlers and consumed by the
   *        recovery operation (mirrors the legacy ServiceState flags).
   */
  class RestoreState {
  public:
    std::atomic<bool> always_restore_from_golden {false};
    std::atomic<bool> prefer_golden_if_current_missing {true};
    std::atomic<bool> restore_on_disconnect {true};

    /// True after a restore attempt that has not been confirmed yet; DISARM and
    /// SNAPSHOT_CURRENT from later stream-start probes must not cancel or
    /// overwrite that restore baseline (72b0d996).
    std::atomic<bool> restore_attempted_unconfirmed {false};

    /// Consecutive confirmed session fallbacks while golden remains pending.
    std::atomic<std::size_t> golden_pending_session_fallbacks {0};

    /// Guard: if a session restore succeeded recently, suppress golden for a cooldown.
    std::atomic<long long> last_session_restore_success_ms {0};

    void set_exclusions(std::vector<std::string> exclusions) {
      std::lock_guard<std::mutex> lock(mutex_);
      exclusions_ = std::move(exclusions);
    }

    std::vector<std::string> exclusions() const {
      std::lock_guard<std::mutex> lock(mutex_);
      return exclusions_;
    }

    void reset_request_progress() {
      restore_attempted_unconfirmed.store(false, std::memory_order_release);
      golden_pending_session_fallbacks.store(0, std::memory_order_release);
    }

  private:
    mutable std::mutex mutex_;
    std::vector<std::string> exclusions_;
  };

  class ApplyPolicy {
  public:
    explicit ApplyPolicy(IClock &clock);

    PolicyDecision maybe_reset_virtual_display(ApplyStatus status, bool virtual_display_requested);
    std::chrono::milliseconds retry_delay(int attempt) const;
    bool should_skip_tier(ApplyStatus status) const;
    bool can_retry_apply(int attempt) const;

  private:
    IClock &clock_;
    std::chrono::steady_clock::time_point last_reset_ {};
    std::chrono::milliseconds reset_cooldown_ {std::chrono::seconds(30)};
    static constexpr std::chrono::milliseconds kRetryBaseDelay {500};
    static constexpr int kMaxApplyAttempts = 3;
  };

  class ApplyOperation {
  public:
    using MutationBoundary = TopologyTransition::MutationBoundary;

    ApplyOperation(
      IDisplaySettings &display,
      IClock &clock,
      MutationBoundary mutation_boundary = {});

    ApplyOutcome run(
      const ApplyRequest &request,
      const CancellationToken &token,
      bool durable_recovery_already_armed = false);
    /// Arms the same durable recovery boundary used by staged topology/settings
    /// work. AsyncDispatcher uses this before a virtual-display reset, which
    /// can change the desktop before ApplyOperation::run begins.
    bool arm_durable_recovery_boundary();
    bool set_refresh_rate(const std::string &device_id, unsigned int numerator, unsigned int denominator);
    bool reset_staged_apply_state();

  private:
    void apply_monitor_positions(const ApplyRequest &request, const CancellationToken &token);
    void apply_refresh_rate_overrides(const ApplyRequest &request, const CancellationToken &token);
    bool restore_baseline_after_failed_apply(const Snapshot &baseline, const CancellationToken &token);

    IDisplaySettings &display_;
    IClock &clock_;
    MutationBoundary mutation_boundary_;
    TopologyTransition topology_transition_;
  };

  class VerificationOperation {
  public:
    VerificationOperation(IDisplaySettings &display, IClock &clock);

    bool run(
      const ApplyRequest &request,
      const std::optional<ActiveTopology> &expected_topology,
      const std::optional<ResolvedConfigurationTarget> &resolved_target,
      const CancellationToken &token);

  private:
    IDisplaySettings &display_;
    IClock &clock_;
  };

  /**
   * @brief One restore attempt over the snapshot chain, ported from the legacy
   *        helper's try_restore_once_if_valid: golden-first strategy with bounded
   *        session fallbacks, prefer-golden-when-current-missing, stable-read +
   *        quiet-period confirmation, and current->previous promotion on success.
   *        Returns success only when the restore was CONFIRMED on screen.
   */
  class RecoveryOperation {
  public:
    static constexpr std::size_t kGoldenFallbackCompletionThreshold = 3;

    RecoveryOperation(
      IDisplaySettings &display,
      ISnapshotStorage &storage,
      GoldenHealth &golden_health,
      RestoreState &state,
      IClock &clock);

    RecoveryOutcome run(const CancellationToken &token);

  private:
    std::optional<codec::ParsedSnapshot> load_filtered(SnapshotTier tier, const char *label);
    bool read_stable_snapshot(Snapshot &out, std::chrono::milliseconds deadline, std::chrono::milliseconds interval, const CancellationToken &token);
    bool quiet_period(std::chrono::milliseconds duration, std::chrono::milliseconds interval, const CancellationToken &token);
    bool wait_with_cancel(std::chrono::milliseconds duration, const CancellationToken &token);
    bool confirm_matches(const codec::ParsedSnapshot &loaded, const char *label, const CancellationToken &token);
    bool apply_and_confirm(const codec::ParsedSnapshot &loaded, const char *label, const CancellationToken &token);
    bool should_skip_golden(const Snapshot &golden);
    std::set<std::string> known_present_devices();
    void clear_session_snapshots_after_golden();
    long long steady_now_ms() const;

    IDisplaySettings &display_;
    ISnapshotStorage &storage_;
    GoldenHealth &golden_health_;
    RestoreState &state_;
    IClock &clock_;
    TopologyTransition topology_transition_;
  };

  class RecoveryValidationOperation {
  public:
    RecoveryValidationOperation(SnapshotService &snapshot_service, IClock &clock);

    bool run(const Snapshot &snapshot, const CancellationToken &token);

  private:
    SnapshotService &snapshot_service_;
    IClock &clock_;
  };
}  // namespace display_helper::v2
