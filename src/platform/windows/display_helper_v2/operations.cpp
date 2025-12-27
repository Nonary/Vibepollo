#include "src/platform/windows/display_helper_v2/operations.h"

#include <algorithm>

namespace display_helper::v2 {
  ApplyPolicy::ApplyPolicy(IClock &clock)
    : clock_(clock) {}

  PolicyDecision ApplyPolicy::maybe_reset_virtual_display(ApplyStatus status, bool virtual_display_requested) {
    if (status != ApplyStatus::NeedsVirtualDisplayReset || !virtual_display_requested) {
      return PolicyDecision::Proceed;
    }

    const auto now = clock_.now();
    if (last_reset_.time_since_epoch().count() != 0) {
      const auto elapsed = now - last_reset_;
      if (elapsed < reset_cooldown_) {
        return PolicyDecision::Proceed;
      }
    }

    last_reset_ = now;
    return PolicyDecision::ResetVirtualDisplay;
  }

  std::chrono::milliseconds ApplyPolicy::retry_delay(int attempt) const {
    if (attempt <= 0) {
      return kRetryDelay;
    }
    return kRetryDelay;
  }

  bool ApplyPolicy::should_skip_tier(ApplyStatus status) const {
    switch (status) {
      case ApplyStatus::InvalidRequest:
      case ApplyStatus::Fatal:
        return true;
      default:
        return false;
    }
  }

  bool ApplyPolicy::can_retry_apply(int attempt) const {
    return attempt < kMaxApplyAttempts;
  }

  ApplyOperation::ApplyOperation(IDisplaySettings &display)
    : display_(display) {}

  ApplyOutcome ApplyOperation::run(const ApplyRequest &request, const CancellationToken &token) {
    ApplyOutcome outcome;
    outcome.virtual_display_requested = request.virtual_layout.has_value();

    if (token.is_cancelled()) {
      outcome.status = ApplyStatus::Fatal;
      return outcome;
    }

    if (!request.configuration) {
      outcome.status = ApplyStatus::InvalidRequest;
      return outcome;
    }

    if (request.topology) {
      outcome.expected_topology = request.topology;
    } else {
      outcome.expected_topology = display_.compute_expected_topology(
        *request.configuration,
        request.topology);
    }

    if (request.topology) {
      const auto topo_status = display_.apply_topology(*request.topology);
      if (topo_status != ApplyStatus::Ok) {
        outcome.status = topo_status;
        return outcome;
      }
    }

    outcome.status = display_.apply(*request.configuration);

    for (const auto &[device_id, origin] : request.monitor_positions) {
      if (!device_id.empty()) {
        (void) display_.set_display_origin(device_id, origin);
      }
    }

    return outcome;
  }

  VerificationOperation::VerificationOperation(IDisplaySettings &display, IClock &clock)
    : display_(display),
      clock_(clock) {}

  bool VerificationOperation::run(
    const ApplyRequest &request,
    const std::optional<ActiveTopology> &expected_topology,
    const CancellationToken &token) {
    if (token.is_cancelled()) {
      return false;
    }

    clock_.sleep_for(std::chrono::milliseconds(250));

    if (token.is_cancelled()) {
      return false;
    }

    if (expected_topology) {
      const auto current = display_.capture_topology();
      if (!display_.is_topology_same(*expected_topology, current)) {
        return false;
      }
    }

    if (request.configuration) {
      if (!display_.configuration_matches(*request.configuration)) {
        return false;
      }
    }

    return true;
  }

  RecoveryOperation::RecoveryOperation(
    IDisplaySettings &display,
    SnapshotService &snapshot_service,
    SnapshotPersistence &snapshot_persistence,
    ApplyPolicy &apply_policy,
    IClock &clock)
    : display_(display),
      snapshot_service_(snapshot_service),
      snapshot_persistence_(snapshot_persistence),
      apply_policy_(apply_policy),
      clock_(clock) {}

  RecoveryOutcome RecoveryOperation::run(const CancellationToken &token) {
    RecoveryOutcome outcome;

    const auto available = available_devices();
    const auto tiers = snapshot_persistence_.recovery_order();

    for (const auto tier : tiers) {
      if (token.is_cancelled()) {
        return outcome;
      }

      auto snapshot = snapshot_persistence_.load(tier, available);
      if (!snapshot) {
        continue;
      }

      if (!snapshot_service_.validate(*snapshot)) {
        continue;
      }

      for (int attempt = 0; attempt < 2; ++attempt) {
        if (token.is_cancelled()) {
          return outcome;
        }

        const auto status = snapshot_service_.apply(*snapshot, token);
        if (status != ApplyStatus::Ok) {
          if (apply_policy_.should_skip_tier(status)) {
            break;
          }
          if (attempt == 0) {
            clock_.sleep_for(std::chrono::milliseconds(300));
            continue;
          }
          break;
        }

        clock_.sleep_for(std::chrono::milliseconds(250));

        if (token.is_cancelled()) {
          return outcome;
        }

        if (snapshot_service_.matches_current(*snapshot)) {
          outcome.success = true;
          outcome.snapshot = *snapshot;
          return outcome;
        }

        if (attempt == 0) {
          clock_.sleep_for(std::chrono::milliseconds(300));
        }
      }
    }

    return outcome;
  }

  std::set<std::string> RecoveryOperation::available_devices() const {
    std::set<std::string> ids;
    auto devices = display_.enumerate(display_device::DeviceEnumerationDetail::Minimal);
    for (const auto &device : devices) {
      std::string id = device.m_device_id.empty() ? device.m_display_name : device.m_device_id;
      if (!id.empty()) {
        ids.insert(std::move(id));
      }
    }
    return ids;
  }

  RecoveryValidationOperation::RecoveryValidationOperation(
    SnapshotService &snapshot_service,
    IClock &clock)
    : snapshot_service_(snapshot_service),
      clock_(clock) {}

  bool RecoveryValidationOperation::run(const Snapshot &snapshot, const CancellationToken &token) {
    if (token.is_cancelled()) {
      return false;
    }

    clock_.sleep_for(std::chrono::milliseconds(250));

    if (token.is_cancelled()) {
      return false;
    }

    return snapshot_service_.matches_current(snapshot);
  }
}  // namespace display_helper::v2
