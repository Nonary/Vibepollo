#pragma once

#include "src/platform/windows/display_helper_v2/golden_health.h"
#include "src/platform/windows/display_helper_v2/interfaces.h"
#include "src/platform/windows/display_helper_v2/runtime_support.h"
#include "src/platform/windows/display_helper_v2/snapshot.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

namespace display_helper::v2 {
  struct ApplyOutcome {
    ApplyStatus status = ApplyStatus::Fatal;
    std::optional<ActiveTopology> expected_topology;
    bool virtual_display_requested = false;
  };

  struct RecoveryOutcome {
    bool success = false;
    std::optional<Snapshot> snapshot;
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
    static constexpr std::chrono::milliseconds kRetryDelay {300};
    static constexpr int kMaxApplyAttempts = 3;
  };

  class ApplyOperation {
  public:
    ApplyOperation(IDisplaySettings &display, IClock &clock);

    ApplyOutcome run(const ApplyRequest &request, const CancellationToken &token);
    bool set_refresh_rate(const std::string &device_id, unsigned int numerator, unsigned int denominator);

  private:
    void apply_monitor_positions(const ApplyRequest &request, const CancellationToken &token);
    void apply_refresh_rate_overrides(const ApplyRequest &request, const CancellationToken &token);

    IDisplaySettings &display_;
    IClock &clock_;
  };

  class VerificationOperation {
  public:
    VerificationOperation(IDisplaySettings &display, IClock &clock);

    bool run(
      const ApplyRequest &request,
      const std::optional<ActiveTopology> &expected_topology,
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
