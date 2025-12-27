#pragma once

#include "src/platform/windows/display_helper_v2/interfaces.h"
#include "src/platform/windows/display_helper_v2/runtime_support.h"
#include "src/platform/windows/display_helper_v2/snapshot.h"

#include <chrono>

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
    explicit ApplyOperation(IDisplaySettings &display);

    ApplyOutcome run(const ApplyRequest &request, const CancellationToken &token);

  private:
    IDisplaySettings &display_;
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

  class RecoveryOperation {
  public:
    RecoveryOperation(
      IDisplaySettings &display,
      SnapshotService &snapshot_service,
      SnapshotPersistence &snapshot_persistence,
      ApplyPolicy &apply_policy,
      IClock &clock);

    RecoveryOutcome run(const CancellationToken &token);

  private:
    IDisplaySettings &display_;
    SnapshotService &snapshot_service_;
    SnapshotPersistence &snapshot_persistence_;
    ApplyPolicy &apply_policy_;
    IClock &clock_;

    std::set<std::string> available_devices() const;
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
