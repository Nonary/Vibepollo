#include "src/platform/windows/display_helper_v2/operations.h"

#include <algorithm>
#include <sstream>

#include <boost/algorithm/string/predicate.hpp>

#include "src/logging.h"

namespace display_helper::v2 {
  namespace {
    const char *tier_to_string(SnapshotTier tier) {
      switch (tier) {
        case SnapshotTier::Current:
          return "Current";
        case SnapshotTier::Previous:
          return "Previous";
        case SnapshotTier::Golden:
          return "Golden";
        default:
          return "Unknown";
      }
    }

    std::optional<ResolvedConfigurationTarget> resolve_configuration_target(
      const TopologyActivationTarget &activation_target,
      const ActiveTopology &accepted_topology) {
      if (activation_target.kind == DeviceTargetKind::None ||
          activation_target.acceptable_device_ids.empty()) {
        return std::nullopt;
      }

      for (const auto &group : accepted_topology) {
        const auto matching = std::find_if(group.begin(), group.end(), [&](const std::string &active_id) {
          return std::any_of(
            activation_target.acceptable_device_ids.begin(),
            activation_target.acceptable_device_ids.end(),
            [&](const std::string &candidate) {
              return boost::iequals(active_id, candidate);
            });
        });
        if (matching == group.end()) {
          continue;
        }

        ResolvedConfigurationTarget target;
        target.kind = activation_target.kind;
        target.representative_device_id = *matching;
        target.duplicate_device_ids.insert(group.begin(), group.end());
        target.duplicate_device_ids.erase(std::string {});
        return target;
      }
      return std::nullopt;
    }

    bool topology_contains_device_id(const ActiveTopology &topology, const std::string &device_id) {
      return std::any_of(topology.begin(), topology.end(), [&](const auto &group) {
        return std::any_of(group.begin(), group.end(), [&](const std::string &active_id) {
          return boost::iequals(active_id, device_id);
        });
      });
    }

    bool resolved_target_remains_active(
      const ActiveTopology &topology,
      const ResolvedConfigurationTarget &target) {
      if (target.kind == DeviceTargetKind::DefaultPrimaryGroup) {
        return std::any_of(
          target.duplicate_device_ids.begin(),
          target.duplicate_device_ids.end(),
          [&](const std::string &device_id) {
            return topology_contains_device_id(topology, device_id);
          });
      }

      if (!target.representative_device_id.empty()) {
        return topology_contains_device_id(topology, target.representative_device_id);
      }
      return std::any_of(
        target.duplicate_device_ids.begin(),
        target.duplicate_device_ids.end(),
        [&](const std::string &device_id) {
          return topology_contains_device_id(topology, device_id);
        });
    }
  }  // namespace

  TopologyTransition::TopologyTransition(IDisplaySettings &display, IClock &clock)
    : display_(display),
      clock_(clock) {}

  std::optional<ActiveTopology> TopologyTransition::topology_ready(
    const ActiveTopology &requested_topology,
    const TopologyActivationTarget &activation_target,
    bool allow_os_adjustment) {
    const auto current = display_.capture_topology();
    if (!display_.topology_is_valid(current)) {
      return std::nullopt;
    }

    const bool exact_match = display_.is_topology_same(requested_topology, current);
    if (!exact_match && !allow_os_adjustment) {
      return std::nullopt;
    }

    // A computed APPLY needs its intended target group to become usable, not
    // every unrelated path Windows still reports while a monitor is asleep or
    // changing inputs. Snapshot restore deliberately has no target, so it
    // continues to require every path in the snapshot topology.
    std::vector<std::string> required_device_ids;
    if (activation_target.kind != DeviceTargetKind::None &&
        !activation_target.acceptable_device_ids.empty()) {
      const auto target_group = std::find_if(current.begin(), current.end(), [&](const auto &group) {
        return std::any_of(group.begin(), group.end(), [&](const std::string &active_id) {
          return std::any_of(
            activation_target.acceptable_device_ids.begin(),
            activation_target.acceptable_device_ids.end(),
            [&](const std::string &candidate) {
              return boost::iequals(active_id, candidate);
            });
        });
      });
      if (target_group == current.end()) {
        return std::nullopt;
      }
      required_device_ids = *target_group;
    } else {
      for (const auto &group : current) {
        required_device_ids.insert(
          required_device_ids.end(),
          group.begin(),
          group.end());
      }
    }
    if (required_device_ids.empty()) {
      return std::nullopt;
    }

    const auto devices = display_.enumerate(display_device::DeviceEnumerationDetail::Minimal);
    for (const auto &active_id : required_device_ids) {
      if (active_id.empty()) {
        return std::nullopt;
      }
      const auto found = std::find_if(devices.begin(), devices.end(), [&](const auto &device) {
        return !device.m_device_id.empty() &&
               boost::iequals(device.m_device_id, active_id) &&
               device.m_info.has_value();
      });
      if (found == devices.end()) {
        return std::nullopt;
      }
    }
    return current;
  }

  bool TopologyTransition::wait_with_cancel(
    std::chrono::milliseconds duration,
    const CancellationToken &token) {
    auto remaining = duration;
    while (remaining > std::chrono::milliseconds::zero()) {
      if (token.is_cancelled()) {
        return false;
      }
      const auto slice = std::min(remaining, kActivationPollInterval);
      clock_.sleep_for(slice);
      remaining -= slice;
    }
    return !token.is_cancelled();
  }

  std::optional<ActiveTopology> TopologyTransition::wait_until_ready(
    const ActiveTopology &requested_topology,
    const TopologyActivationTarget &activation_target,
    bool allow_os_adjustment,
    const CancellationToken &token) {
    const auto deadline = clock_.now() + kActivationTimeout;
    while (!token.is_cancelled()) {
      if (auto ready = topology_ready(
            requested_topology,
            activation_target,
            allow_os_adjustment)) {
        return ready;
      }
      const auto now = clock_.now();
      if (now >= deadline) {
        return std::nullopt;
      }
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      if (!wait_with_cancel(std::min(remaining, kActivationPollInterval), token)) {
        return std::nullopt;
      }
    }
    return std::nullopt;
  }

  bool TopologyTransition::recover_and_settle(const CancellationToken &token) {
    if (token.is_cancelled()) {
      return false;
    }
    if (!display_.recover_display_stack()) {
      BOOST_LOG(warning) << "Display helper v2: display-stack recovery failed during topology transition.";
    }
    return wait_with_cancel(kRecoverySettleDelay, token);
  }

  TopologyTransitionOutcome TopologyTransition::run(
    const ActiveTopology &topology,
    TopologyValidationMode validation_mode,
    const TopologyActivationTarget &activation_target,
    const CancellationToken &token,
    const MutationBoundary &mutation_boundary) {
    TopologyTransitionOutcome outcome;
    if (token.is_cancelled()) {
      outcome.status = ApplyStatus::Fatal;
      return outcome;
    }
    if (!display_.topology_is_valid(topology)) {
      BOOST_LOG(error) << "Display helper v2: refusing structurally invalid topology transition.";
      outcome.status = ApplyStatus::InvalidRequest;
      return outcome;
    }
    if (auto ready = topology_ready(topology, activation_target, false)) {
      outcome.status = ApplyStatus::Ok;
      outcome.applied_topology = std::move(ready);
      return outcome;
    }

    bool mutation_boundary_reached = false;
    const auto arm_mutation_boundary = [&]() {
      if (mutation_boundary_reached) {
        return;
      }
      mutation_boundary_reached = true;
      if (mutation_boundary) {
        outcome.durable_recovery_armed = mutation_boundary();
        if (!outcome.durable_recovery_armed) {
          BOOST_LOG(warning) << "Display helper v2: failed to arm durable recovery before topology mutation.";
        }
      }
    };

    if (validation_mode == TopologyValidationMode::StrictApply &&
        !display_.validate_topology_for_apply(topology)) {
      BOOST_LOG(warning) << "Display helper v2: requested topology failed OS validation; recovering the display stack and retrying validation.";
      arm_mutation_boundary();
      // The guard is intentionally armed before this call, but cancellation
      // may arrive in the tiny hand-off window.  Do not begin a recovery call
      // after cancellation merely because its task was created successfully.
      if (token.is_cancelled()) {
        outcome.status = ApplyStatus::Fatal;
        return outcome;
      }
      outcome.display_may_have_changed = true;
      if (!recover_and_settle(token)) {
        outcome.status = token.is_cancelled() ? ApplyStatus::Fatal : ApplyStatus::Retryable;
        return outcome;
      }
      if (!display_.validate_topology_for_apply(topology)) {
        BOOST_LOG(warning) << "Display helper v2: requested topology still fails OS validation after recovery.";
        outcome.status = ApplyStatus::Retryable;
        return outcome;
      }
    }

    for (int attempt = 1; attempt <= kMaxTopologyAttempts; ++attempt) {
      if (token.is_cancelled()) {
        outcome.status = ApplyStatus::Fatal;
        return outcome;
      }

      BOOST_LOG(info) << "Display helper v2: topology activation stage attempt #" << attempt << ".";
      arm_mutation_boundary();
      if (token.is_cancelled()) {
        outcome.status = ApplyStatus::Fatal;
        return outcome;
      }
      // SetDisplayConfig can partially alter the desktop even when it reports
      // an error, so this is the precise point at which rollback/recovery is
      // no longer optional.
      outcome.display_may_have_changed = true;
      const auto apply_status = display_.apply_topology(topology);
      if (apply_status == ApplyStatus::Fatal || apply_status == ApplyStatus::HelperUnavailable ||
          apply_status == ApplyStatus::InvalidRequest) {
        outcome.status = apply_status;
        return outcome;
      }

      // SetDisplayConfig may report success before display names and modes are
      // queryable. It can also report a transient failure after the topology
      // actually landed, so readiness is the authoritative result. A successful
      // call may also yield a valid OS-adjusted topology; accept it when the
      // configured target remains active, matching WinDisplayDevice/v1.
      const bool allow_os_adjustment = apply_status == ApplyStatus::Ok;
      if (auto applied_topology = wait_until_ready(
            topology,
            activation_target,
            allow_os_adjustment,
            token)) {
        if (!display_.is_topology_same(topology, *applied_topology)) {
          BOOST_LOG(warning) << "Display helper v2: accepting usable OS-adjusted topology after activation.";
        } else {
          BOOST_LOG(info) << "Display helper v2: requested topology is active and all devices are enumerable.";
        }
        outcome.status = ApplyStatus::Ok;
        outcome.applied_topology = std::move(applied_topology);
        return outcome;
      }
      if (token.is_cancelled()) {
        outcome.status = ApplyStatus::Fatal;
        return outcome;
      }

      BOOST_LOG(warning) << "Display helper v2: topology did not become ready on attempt #" << attempt << ".";
      if (attempt < kMaxTopologyAttempts && !recover_and_settle(token)) {
        outcome.status = token.is_cancelled() ? ApplyStatus::Fatal : ApplyStatus::Retryable;
        return outcome;
      }
    }

    outcome.status = ApplyStatus::Retryable;
    return outcome;
  }

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
    const int multiplier = std::clamp(attempt, 1, kMaxApplyAttempts);
    return kRetryBaseDelay * multiplier;
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

  ApplyOperation::ApplyOperation(
    IDisplaySettings &display,
    IClock &clock,
    MutationBoundary mutation_boundary)
    : display_(display),
      clock_(clock),
      mutation_boundary_(std::move(mutation_boundary)),
      topology_transition_(display, clock) {}

  bool ApplyOperation::arm_durable_recovery_boundary() {
    return mutation_boundary_ && mutation_boundary_();
  }

  ApplyOutcome ApplyOperation::run(
    const ApplyRequest &request,
    const CancellationToken &token,
    bool durable_recovery_already_armed) {
    ApplyOutcome outcome;
    outcome.virtual_display_requested = request.virtual_layout.has_value();
    bool durable_recovery_armed = durable_recovery_already_armed;
    const auto ensure_durable_recovery = [this, &durable_recovery_armed]() {
      if (durable_recovery_armed) {
        return true;
      }
      durable_recovery_armed = arm_durable_recovery_boundary();
      return durable_recovery_armed;
    };
    const MutationBoundary mutation_boundary = mutation_boundary_ ?
                                              MutationBoundary {ensure_durable_recovery} :
                                              MutationBoundary {};

    if (token.is_cancelled()) {
      outcome.status = ApplyStatus::Fatal;
      return outcome;
    }

    if (!request.configuration) {
      outcome.status = ApplyStatus::InvalidRequest;
      return outcome;
    }

    // Keep an in-memory rollback point as well as the durable session snapshot.
    // The staged engine changes topology before SettingsManager applies primary,
    // mode, and HDR; if that later stage fails, v1's guard semantics restore
    // the caller's layout instead of leaving a half-applied transaction live.
    const auto baseline_snapshot = display_.capture_snapshot();
    const bool have_rollback_baseline = display_.topology_is_valid(baseline_snapshot.m_topology) &&
                                       !baseline_snapshot.m_modes.empty();

    const auto current_topology = display_.capture_topology();
    if (!display_.topology_is_valid(current_topology)) {
      BOOST_LOG(warning) << "Display helper v2: current topology is unavailable before APPLY; deferring for retry.";
      outcome.status = ApplyStatus::Retryable;
      return outcome;
    }
    if (!display_.prepare_staged_apply(current_topology)) {
      BOOST_LOG(warning) << "Display helper v2: could not preserve the session display baseline before APPLY.";
      outcome.status = ApplyStatus::Retryable;
      return outcome;
    }
    outcome.staged_state_prepared = true;

    auto topology_plan = display_.compute_apply_topology_plan(*request.configuration, current_topology);
    if (request.topology) {
      if (topology_plan) {
        topology_plan->topology = *request.topology;
      } else {
        TopologyActivationTarget activation_target;
        activation_target.kind = request.configuration->m_device_id.empty() ?
                                   DeviceTargetKind::DefaultPrimaryGroup :
                                   DeviceTargetKind::ExplicitDevice;
        if (!request.configuration->m_device_id.empty()) {
          activation_target.acceptable_device_ids.insert(request.configuration->m_device_id);
        }
        topology_plan = ApplyTopologyPlan {
          .topology = *request.topology,
          .activation_target = std::move(activation_target),
        };
      }
    }
    if (!topology_plan || !display_.topology_is_valid(topology_plan->topology)) {
      BOOST_LOG(error) << "Display helper v2: could not compute a valid target topology.";
      outcome.status = ApplyStatus::InvalidRequest;
      return outcome;
    }
    // `sunshine_topology` is the staging/base topology for SettingsManager,
    // matching v1. It is not an exact post-apply contract: Windows may legally
    // adjust or regroup unrelated paths. Verification therefore uses the
    // resolved target and requested settings below, rather than pinning every
    // normal APPLY to this intermediate topology.

    const auto topology_outcome = topology_transition_.run(
      topology_plan->topology,
      TopologyValidationMode::StrictApply,
      topology_plan->activation_target,
      token,
      mutation_boundary);
    outcome.status = topology_outcome.status;
    outcome.display_may_have_changed = topology_outcome.display_may_have_changed;
    outcome.durable_recovery_armed = durable_recovery_armed || topology_outcome.durable_recovery_armed;
    if (outcome.status != ApplyStatus::Ok) {
      return outcome;
    }
    if (!topology_outcome.applied_topology) {
      outcome.status = ApplyStatus::VerificationFailed;
      return outcome;
    }
    outcome.resolved_target = resolve_configuration_target(
      topology_plan->activation_target,
      *topology_outcome.applied_topology);
    if (topology_plan->activation_target.kind != DeviceTargetKind::None &&
        !topology_plan->activation_target.acceptable_device_ids.empty() &&
        !outcome.resolved_target) {
      BOOST_LOG(warning) << "Display helper v2: accepted topology did not retain a resolvable configuration target.";
      outcome.status = ApplyStatus::VerificationFailed;
      return outcome;
    }

    if (token.is_cancelled()) {
      outcome.status = ApplyStatus::Fatal;
      return outcome;
    }

    // A topology that was already ready still leaves mode/HDR/primary changes
    // ahead. Arm recovery immediately before that first settings mutation.
    if (!outcome.display_may_have_changed) {
      // This is the first point at which the desktop may have been changed.
      // Arm the boot/logon restore guard before the remaining potentially
      // multi-second settings, positioning, and refresh work runs.
      if (mutation_boundary) {
        outcome.durable_recovery_armed = ensure_durable_recovery();
        if (!outcome.durable_recovery_armed) {
          BOOST_LOG(warning) << "Display helper v2: could not arm durable recovery at topology mutation boundary.";
        }
      }
    }

    // The state machine may have accepted a DISARM/RESET while the worker was
    // arranging the durable guard.  Respect that cancellation before entering
    // SettingsManager, which owns several individually-mutating calls.
    if (token.is_cancelled()) {
      outcome.status = ApplyStatus::Fatal;
      return outcome;
    }

    // Topology is now settled. The backend rebases SettingsManager onto the
    // accepted topology and applies primary/mode/HDR state without another
    // topology transition.
    // `display_may_have_changed` is deliberately flipped at the call itself,
    // not when the durable task is armed, so a cancellation in between can
    // safely clean up the guard without pretending Windows was touched.
    outcome.display_may_have_changed = true;
    outcome.status = display_.apply(*request.configuration);

    if (token.is_cancelled()) {
      outcome.status = ApplyStatus::Fatal;
      return outcome;
    }

    if (outcome.status == ApplyStatus::Ok) {
      apply_monitor_positions(request, token);
      apply_refresh_rate_overrides(request, token);
    } else if (have_rollback_baseline && restore_baseline_after_failed_apply(baseline_snapshot, token)) {
      BOOST_LOG(warning) << "Display helper v2: SettingsManager stage failed; restored the pre-APPLY display baseline.";
      outcome.display_may_have_changed = false;
    } else {
      BOOST_LOG(warning) << "Display helper v2: SettingsManager stage failed and the pre-APPLY baseline could not be restored.";
    }

    if (token.is_cancelled()) {
      // A rollback is itself a staged display mutation. Do not report a clean
      // retryable result if a competing command cancelled during its later
      // settings stage; the state machine must retain the recovery guard.
      outcome.status = ApplyStatus::Fatal;
    }

    return outcome;
  }

  bool ApplyOperation::restore_baseline_after_failed_apply(const Snapshot &baseline, const CancellationToken &token) {
    if (token.is_cancelled() || !display_.topology_is_valid(baseline.m_topology)) {
      return false;
    }

    const auto restore = topology_transition_.run(
      baseline.m_topology,
      TopologyValidationMode::StructuralRestore,
      TopologyActivationTarget {},
      token);
    if (restore.status != ApplyStatus::Ok || token.is_cancelled()) {
      return false;
    }
    if (token.is_cancelled() || !display_.apply_snapshot_settings(baseline)) {
      return false;
    }
    return !token.is_cancelled() && display_.snapshot_matches_current(baseline);
  }

  void ApplyOperation::apply_monitor_positions(const ApplyRequest &request, const CancellationToken &token) {
    if (request.monitor_positions.empty()) {
      return;
    }

    // The whole monitor rect must fit inside the GDI virtual screen (±32767), so
    // the maximum origin shrinks by the monitor size. Clamping only the origin to
    // 32767 always fails SetDisplayConfig with ERROR_INVALID_PARAMETER for any
    // non-zero-sized display (d07fd6cb).
    constexpr int kMinDisplayOrigin = -32768;
    constexpr int kMaxDisplayOrigin = 32767;
    constexpr auto kRepositionRetryInterval = std::chrono::milliseconds(200);
    constexpr int kMaxRepositionAttempts = 15;  // ~3s window at 200ms

    auto pending_overrides = request.monitor_positions;
    int retry_attempt = 0;

    while (!pending_overrides.empty()) {
      if (token.is_cancelled()) {
        return;
      }
      ++retry_attempt;
      std::vector<std::pair<std::string, display_device::Point>> next_pending;
      next_pending.reserve(pending_overrides.size());

      for (const auto &[device_id, origin] : pending_overrides) {
        if (token.is_cancelled()) {
          return;
        }
        if (device_id.empty()) {
          continue;
        }
        if (!display_.can_reposition_device(device_id)) {
          next_pending.emplace_back(device_id, origin);
          continue;
        }
        int max_origin_x = kMaxDisplayOrigin;
        int max_origin_y = kMaxDisplayOrigin;
        if (const auto res = display_.get_display_resolution(device_id)) {
          max_origin_x = std::max(kMinDisplayOrigin, kMaxDisplayOrigin - static_cast<int>(res->m_width) + 1);
          max_origin_y = std::max(kMinDisplayOrigin, kMaxDisplayOrigin - static_cast<int>(res->m_height) + 1);
        }
        const auto clamped_origin = display_device::Point {
          std::clamp(origin.m_x, kMinDisplayOrigin, max_origin_x),
          std::clamp(origin.m_y, kMinDisplayOrigin, max_origin_y)
        };
        if (clamped_origin.m_x != origin.m_x || clamped_origin.m_y != origin.m_y) {
          BOOST_LOG(warning) << "Display helper: clamped monitor position override for device_id=" << device_id
                             << " from (" << origin.m_x << "," << origin.m_y << ") to ("
                             << clamped_origin.m_x << "," << clamped_origin.m_y << ")";
        }
        const bool ok_origin = display_.set_display_origin(device_id, clamped_origin);
        if (!ok_origin) {
          next_pending.emplace_back(device_id, origin);
        }
      }

      pending_overrides = std::move(next_pending);
      if (pending_overrides.empty()) {
        break;
      }
      if (retry_attempt >= kMaxRepositionAttempts) {
        break;
      }
      clock_.sleep_for(kRepositionRetryInterval);
    }

    if (!pending_overrides.empty()) {
      std::string pending_ids;
      for (size_t i = 0; i < pending_overrides.size(); ++i) {
        if (i > 0) {
          pending_ids += ", ";
        }
        pending_ids += pending_overrides[i].first;
      }
      BOOST_LOG(warning) << "Display helper: monitor position overrides not fully applied after "
                         << retry_attempt << " attempt(s); pending device_id(s)=" << pending_ids;
    }
    BOOST_LOG(info) << "Display helper: monitor position overrides applied result="
                    << (pending_overrides.empty() ? "true" : "false");
  }

  void ApplyOperation::apply_refresh_rate_overrides(const ApplyRequest &request, const CancellationToken &token) {
    if (request.refresh_rate_overrides.empty()) {
      return;
    }

    // Restore physical monitor refresh rates from the pre-VD-creation snapshot.
    // When a virtual display is created at (0,0), Windows may reset other monitors'
    // refresh rates (e.g. 240Hz -> 60Hz). This restores the original rates.
    bool rate_result = true;
    for (const auto &[device_id, rate] : request.refresh_rate_overrides) {
      if (token.is_cancelled()) {
        break;
      }
      if (device_id.empty() || rate.first == 0 || rate.second == 0) {
        continue;
      }
      // Skip the virtual display device itself.
      if (request.configuration && device_id == request.configuration->m_device_id) {
        continue;
      }
      const bool ok = display_.set_device_refresh_rate(device_id, rate.first, rate.second);
      if (ok) {
        BOOST_LOG(info) << "Display helper: restored refresh rate for device=" << device_id
                        << " to " << rate.first << "/" << rate.second;
      } else {
        BOOST_LOG(warning) << "Display helper: failed to restore refresh rate for device=" << device_id;
      }
      rate_result = rate_result && ok;
    }
    BOOST_LOG(info) << "Display helper: refresh rate overrides applied result=" << (rate_result ? "true" : "false");
  }

  bool ApplyOperation::set_refresh_rate(
    const std::string &device_id,
    unsigned int numerator,
    unsigned int denominator
  ) {
    const bool success = display_.set_device_refresh_rate(device_id, numerator, denominator);
    BOOST_LOG(success ? info : warning)
      << "Display helper: refresh-only request device=" << device_id
      << " rate=" << numerator << '/' << denominator
      << " result=" << (success ? "true" : "false");
    return success;
  }

  bool ApplyOperation::reset_staged_apply_state() {
    return display_.reset_staged_apply_state();
  }

  VerificationOperation::VerificationOperation(IDisplaySettings &display, IClock &clock)
    : display_(display),
      clock_(clock) {}

  bool VerificationOperation::run(
    const ApplyRequest &request,
    const std::optional<ActiveTopology> &expected_topology,
    const std::optional<ResolvedConfigurationTarget> &resolved_target,
    const CancellationToken &token) {
    if (token.is_cancelled()) {
      return false;
    }

    clock_.sleep_for(std::chrono::milliseconds(250));

    if (token.is_cancelled()) {
      return false;
    }

    // Sticky verification re-evaluates any explicit exact topology contract
    // supplied by a future caller, plus the resolved target and requested
    // settings. Normal APPLY treats `sunshine_topology` as a staging/base
    // topology, matching v1, so unrelated paths may be adjusted while a
    // sleeping monitor comes online.
    const auto matches_requested_state = [&]() {
      std::optional<ActiveTopology> current_topology;
      if (expected_topology || resolved_target) {
        current_topology = display_.capture_topology();
      }
      if (expected_topology) {
        if (!current_topology || !display_.is_topology_same(*expected_topology, *current_topology)) {
          return false;
        }
      }

      if (resolved_target &&
          (!current_topology || !resolved_target_remains_active(*current_topology, *resolved_target))) {
        return false;
      }

      if (!request.configuration) {
        return true;
      }
      if (resolved_target ?
            !display_.configuration_matches(*request.configuration, *resolved_target) :
            !display_.configuration_matches(*request.configuration)) {
        return false;
      }
      if (request.configuration->m_device_prep !=
          SingleDisplayConfiguration::DevicePreparation::EnsurePrimary) {
        return true;
      }
      if (!resolved_target) {
        return request.configuration->m_device_id.empty() ||
               display_.is_primary_device(request.configuration->m_device_id);
      }
      if (resolved_target->kind == DeviceTargetKind::DefaultPrimaryGroup) {
        return std::any_of(
          resolved_target->duplicate_device_ids.begin(),
          resolved_target->duplicate_device_ids.end(),
          [this](const std::string &device_id) {
            return display_.is_primary_device(device_id);
          });
      }
      return !resolved_target->representative_device_id.empty() &&
             display_.is_primary_device(resolved_target->representative_device_id);
    };

    if (!matches_requested_state()) {
      return false;
    }

    clock_.sleep_for(std::chrono::milliseconds(250));
    return !token.is_cancelled() && matches_requested_state();
  }

  RecoveryOperation::RecoveryOperation(
    IDisplaySettings &display,
    ISnapshotStorage &storage,
    GoldenHealth &golden_health,
    RestoreState &state,
    IClock &clock)
    : display_(display),
      storage_(storage),
      golden_health_(golden_health),
      state_(state),
      clock_(clock),
      topology_transition_(display, clock) {}

  long long RecoveryOperation::steady_now_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
             clock_.now().time_since_epoch()
    )
      .count();
  }

  std::optional<codec::ParsedSnapshot> RecoveryOperation::load_filtered(SnapshotTier tier, const char *label) {
    auto loaded = storage_.load_with_metadata(tier);
    if (!loaded) {
      return std::nullopt;
    }
    const auto devices = display_.enumerate(display_device::DeviceEnumerationDetail::Minimal);
    return codec::filter_loaded_snapshot(std::move(*loaded), devices, state_.exclusions(), label ? label : tier_to_string(tier));
  }

  bool RecoveryOperation::read_stable_snapshot(
    Snapshot &out,
    std::chrono::milliseconds deadline,
    std::chrono::milliseconds interval,
    const CancellationToken &token) {
    const auto t0 = clock_.now();
    bool have_last = false;
    Snapshot last;
    while (clock_.now() - t0 < deadline) {
      if (token.is_cancelled()) {
        return false;
      }
      auto cur = display_.capture_snapshot();
      // Heuristic: treat completely empty topology+modes as transient
      const bool emptyish = cur.m_topology.empty() && cur.m_modes.empty();
      if (have_last && !emptyish && (cur == last)) {
        out = std::move(cur);
        return true;
      }
      last = std::move(cur);
      have_last = true;
      if (token.is_cancelled()) {
        return false;
      }
      clock_.sleep_for(interval);
    }
    return false;
  }

  bool RecoveryOperation::quiet_period(
    std::chrono::milliseconds duration,
    std::chrono::milliseconds interval,
    const CancellationToken &token) {
    Snapshot base;
    if (!read_stable_snapshot(base, std::chrono::milliseconds(2000), std::chrono::milliseconds(150), token)) {
      return false;
    }
    const auto t0 = clock_.now();
    while (clock_.now() - t0 < duration) {
      if (token.is_cancelled()) {
        return false;
      }
      Snapshot cur;
      if (!read_stable_snapshot(cur, std::chrono::milliseconds(2000), std::chrono::milliseconds(150), token)) {
        return false;
      }
      if (!(cur == base)) {
        // topology changed during quiet period
        return false;
      }
      if (token.is_cancelled()) {
        return false;
      }
      clock_.sleep_for(interval);
    }
    return true;
  }

  bool RecoveryOperation::wait_with_cancel(std::chrono::milliseconds duration, const CancellationToken &token) {
    constexpr auto kStep = std::chrono::milliseconds(50);
    auto remaining = duration;
    while (remaining > std::chrono::milliseconds::zero()) {
      if (token.is_cancelled()) {
        return false;
      }
      const auto slice = remaining > kStep ? kStep : remaining;
      clock_.sleep_for(slice);
      remaining -= slice;
    }
    return !token.is_cancelled();
  }

  bool RecoveryOperation::confirm_matches(const codec::ParsedSnapshot &loaded, const char *label, const CancellationToken &token) {
    Snapshot cur;
    const bool got_stable = read_stable_snapshot(cur, std::chrono::milliseconds(2000), std::chrono::milliseconds(150), token);
    if (token.is_cancelled()) {
      return false;
    }
    const bool layout_ok = !loaded.has_layout_data || display_.current_layout_matches(loaded.layout_rotations);
    const bool ok = got_stable && codec::equal_snapshots_strict(cur, loaded.snapshot) && layout_ok &&
                    quiet_period(std::chrono::milliseconds(750), std::chrono::milliseconds(150), token);
    if (ok) {
      BOOST_LOG(info) << "Restore (" << label << "): current state already matches baseline; skipping apply.";
    }
    return ok;
  }

  bool RecoveryOperation::apply_and_confirm(const codec::ParsedSnapshot &loaded, const char *label, const CancellationToken &token) {
    const auto &base = loaded.snapshot;
    const auto &layouts = loaded.layout_rotations;
    const bool require_layout_match = loaded.has_layout_data;
    if (!require_layout_match && loaded.snapshot_version < codec::kSnapshotLayoutVersionLatest) {
      BOOST_LOG(info) << label << " snapshot uses legacy schema (version "
                      << loaded.snapshot_version << "): no display layout metadata.";
    }

    const auto before_sig = codec::signature(display_.capture_snapshot());

    auto apply_once = [&]() -> bool {
      const auto topology_outcome = topology_transition_.run(
        base.m_topology,
        TopologyValidationMode::StructuralRestore,
        TopologyActivationTarget {},
        token);
      if (topology_outcome.status != ApplyStatus::Ok) {
        BOOST_LOG(warning) << "Restore (" << label << "): topology activation stage failed with status="
                           << static_cast<int>(topology_outcome.status);
        return false;
      }
      // A competing APPLY/DISARM may arrive while the staged topology waits
      // for Windows to enumerate. Do not begin a later restore stage once it
      // has cancelled this transaction.
      if (token.is_cancelled()) {
        return false;
      }
      if (!display_.apply_snapshot_settings(base)) {
        BOOST_LOG(warning) << "Restore (" << label << "): post-topology settings stage failed.";
        return false;
      }
      if (token.is_cancelled()) {
        return false;
      }
      if (require_layout_match && !layouts.empty()) {
        if (token.is_cancelled()) {
          return false;
        }
        if (!display_.apply_layout_rotations(layouts)) {
          BOOST_LOG(warning) << "Restore (" << label << "): layout rotation stage failed.";
          return false;
        }
      }
      return true;
    };

    auto verify_once = [&](const char *attempt) -> bool {
      Snapshot cur;
      const bool got_stable = read_stable_snapshot(cur, std::chrono::milliseconds(2000), std::chrono::milliseconds(150), token);
      if (token.is_cancelled()) {
        return false;
      }
      const bool layout_ok = !require_layout_match || display_.current_layout_matches(layouts);
      const bool ok = got_stable && codec::equal_snapshots_strict(cur, base) && layout_ok &&
                      quiet_period(std::chrono::milliseconds(750), std::chrono::milliseconds(150), token);
      BOOST_LOG(info) << "Restore (" << label << ") attempt " << attempt << ": before_sig=" << before_sig
                      << ", current_sig=" << codec::signature(cur)
                      << ", baseline_sig=" << codec::signature(base)
                      << ", layout_match=" << (layout_ok ? "true" : "false")
                      << ", match=" << (ok ? "true" : "false");
      return ok;
    };

    if (token.is_cancelled()) {
      return false;
    }
    if (confirm_matches(loaded, label, token)) {
      return true;
    }

    // Attempt 1
    if (token.is_cancelled()) {
      return false;
    }
    const bool first_apply_succeeded = apply_once();
    if (first_apply_succeeded && verify_once("#1")) {
      return true;
    }

    // Attempt 2 (double-check) after a short delay
    if (token.is_cancelled() || !wait_with_cancel(std::chrono::milliseconds(700), token)) {
      return false;
    }
    if (confirm_matches(loaded, label, token)) {
      return true;
    }
    const bool second_apply_succeeded = apply_once();
    return second_apply_succeeded && verify_once("#2");
  }

  std::set<std::string> RecoveryOperation::known_present_devices() {
    std::set<std::string> result;
    try {
      // Active devices (have modes)
      const auto snap = display_.capture_snapshot();
      for (const auto &kv : snap.m_modes) {
        result.insert(codec::normalize_device_id(kv.first));
      }
      // Enumerated devices (active or inactive)
      for (const auto &d : display_.enumerate(display_device::DeviceEnumerationDetail::Minimal)) {
        const auto id = d.m_device_id.empty() ? d.m_display_name : d.m_device_id;
        if (!id.empty()) {
          result.insert(codec::normalize_device_id(id));
        }
      }
      // Fallback to topology flatten if the above produced nothing
      if (result.empty()) {
        for (const auto &grp : snap.m_topology) {
          for (const auto &id : grp) {
            result.insert(codec::normalize_device_id(id));
          }
        }
      }
    } catch (...) {
    }
    return result;
  }

  bool RecoveryOperation::should_skip_golden(const Snapshot &golden) {
    const auto now_ms = steady_now_ms();
    const auto last_ok = state_.last_session_restore_success_ms.load(std::memory_order_acquire);
    if (last_ok != 0 && (now_ms - last_ok) < 60'000) {
      BOOST_LOG(info) << "Skipping golden: recent session restore success guard active.";
      return true;
    }
    // Ensure all devices in golden exist now
    std::set<std::string> golden_devices;
    for (const auto &grp : golden.m_topology) {
      for (const auto &id : grp) {
        golden_devices.insert(codec::normalize_device_id(id));
      }
    }
    if (golden_devices.empty()) {
      // be conservative if snapshot malformed
      return true;
    }
    const auto present = known_present_devices();
    for (const auto &id : golden_devices) {
      if (!present.contains(id)) {
        BOOST_LOG(info) << "Skipping golden: device not present: " << id;
        return true;
      }
    }
    return false;
  }

  void RecoveryOperation::clear_session_snapshots_after_golden() {
    const bool removed_current = storage_.remove(SnapshotTier::Current);
    const bool removed_previous = storage_.remove(SnapshotTier::Previous);
    BOOST_LOG(info) << "Golden restore cleanup: removed current=" << (removed_current ? "true" : "false")
                    << ", previous=" << (removed_previous ? "true" : "false");
  }

  RecoveryOutcome RecoveryOperation::run(const CancellationToken &token) {
    RecoveryOutcome outcome;
    if (token.is_cancelled()) {
      return outcome;
    }

    // AsyncDispatcher handles the cancellable grace period before entering
    // this operation. Once recovery begins, preserve its restore guard on a
    // later cancellation: topology/settings work may start on any branch.
    outcome.display_may_have_changed = true;
    state_.restore_attempted_unconfirmed.store(true, std::memory_order_release);

    const bool golden_first = state_.always_restore_from_golden.load(std::memory_order_acquire);
    if (!golden_first) {
      state_.golden_pending_session_fallbacks.store(0, std::memory_order_release);
    }

    std::optional<Snapshot> restored;

    auto try_golden = [&]() -> bool {
      if (token.is_cancelled()) {
        return false;
      }
      auto golden = load_filtered(SnapshotTier::Golden, "golden");
      if (!golden) {
        return false;
      }
      if (should_skip_golden(golden->snapshot)) {
        return false;
      }
      if (!display_.validate_topology(golden->snapshot.m_topology)) {
        golden_health_.note_issue("invalid_topology");
        return false;
      }
      if (apply_and_confirm(*golden, "golden", token)) {
        if (token.is_cancelled()) {
          return false;
        }
        BOOST_LOG(info) << "Golden restore confirmed; clearing session restore snapshots.";
        outcome.staged_state_reset_attempted = true;
        outcome.staged_state_reset_succeeded = display_.reset_staged_apply_state();
        if (!outcome.staged_state_reset_succeeded) {
          BOOST_LOG(warning) << "Display helper v2: failed to clear staged APPLY state after confirmed golden restore.";
        }
        clear_session_snapshots_after_golden();
        golden_health_.clear_status("restore confirmed");
        restored = golden->snapshot;
        return true;
      }
      if (!token.is_cancelled()) {
        golden_health_.note_issue("restore_not_confirmed");
      }
      return false;
    };

    auto try_session = [&](SnapshotTier tier, const char *label, bool &attempted) -> bool {
      attempted = false;
      auto loaded = load_filtered(tier, label);
      if (!loaded) {
        BOOST_LOG(info) << label << " snapshot not available.";
        return false;
      }
      attempted = true;
      if (!display_.topology_is_valid(loaded->snapshot.m_topology)) {
        BOOST_LOG(info) << label << " snapshot rejected due to invalid topology.";
        return false;
      }
      if (apply_and_confirm(*loaded, label, token)) {
        if (token.is_cancelled()) {
          return false;
        }
        outcome.staged_state_reset_attempted = true;
        outcome.staged_state_reset_succeeded = display_.reset_staged_apply_state();
        if (!outcome.staged_state_reset_succeeded) {
          BOOST_LOG(warning) << "Display helper v2: failed to clear staged APPLY state after confirmed session restore.";
        }
        state_.last_session_restore_success_ms.store(steady_now_ms(), std::memory_order_release);
        restored = loaded->snapshot;
        return true;
      }
      return false;
    };

    bool tried_golden_before_previous = false;
    auto try_session_snapshots = [&]() -> bool {
      bool attempted_current = false;
      if (try_session(SnapshotTier::Current, "current", attempted_current)) {
        (void) storage_.promote_current_to_previous();
        return true;
      }

      const bool current_snapshot_unavailable = !attempted_current;
      const bool prefer_golden_before_previous =
        state_.prefer_golden_if_current_missing.load(std::memory_order_acquire) && current_snapshot_unavailable;
      if (prefer_golden_before_previous &&
          storage_.exists(SnapshotTier::Previous) && storage_.exists(SnapshotTier::Golden)) {
        tried_golden_before_previous = true;
        BOOST_LOG(info) << "Restore: current snapshot unavailable; preferring golden snapshot over previous session snapshot.";
        if (try_golden()) {
          return true;
        }
      }

      bool attempted_previous = false;
      if (try_session(SnapshotTier::Previous, "previous", attempted_previous)) {
        if (attempted_current) {
          (void) storage_.remove(SnapshotTier::Current);
        }
        return true;
      }
      (void) attempted_previous;
      return false;
    };

    if (golden_first) {
      // Prefer golden snapshot, fallback to session snapshots
      BOOST_LOG(info) << "Restore: using golden-first strategy (always_restore_from_golden=true)";
      if (try_golden()) {
        state_.golden_pending_session_fallbacks.store(0, std::memory_order_release);
        outcome.success = true;
        outcome.snapshot = restored;
        return outcome;
      }
      // Golden failed. Session snapshots can keep the machine usable, but only
      // retry golden a few times within the same restore request before accepting
      // the confirmed session fallback.
      if (!try_session_snapshots()) {
        state_.golden_pending_session_fallbacks.store(0, std::memory_order_release);
        return outcome;
      }

      if (load_filtered(SnapshotTier::Golden, "golden-pending-check")) {
        const auto fallback_count = state_.golden_pending_session_fallbacks.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (fallback_count < kGoldenFallbackCompletionThreshold) {
          BOOST_LOG(info) << "Restore: session fallback applied while golden snapshot remains pending; continuing polling (attempt "
                          << fallback_count << '/' << kGoldenFallbackCompletionThreshold << ").";
          return outcome;
        }

        state_.golden_pending_session_fallbacks.store(0, std::memory_order_release);
        BOOST_LOG(info) << "Restore: session fallback confirmed while golden snapshot remains pending; accepting session restore after "
                        << kGoldenFallbackCompletionThreshold << " consecutive golden-first attempts.";
        outcome.success = true;
        outcome.snapshot = restored;
        return outcome;
      }

      golden_health_.register_unresolved("session fallback accepted");
      state_.golden_pending_session_fallbacks.store(0, std::memory_order_release);
      outcome.success = true;
      outcome.snapshot = restored;
      return outcome;
    }

    // Default: prefer session snapshots, fallback to golden
    if (try_session_snapshots()) {
      if (tried_golden_before_previous) {
        golden_health_.register_unresolved("session fallback accepted");
      }
      state_.golden_pending_session_fallbacks.store(0, std::memory_order_release);
      outcome.success = true;
      outcome.snapshot = restored;
      return outcome;
    }
    if (tried_golden_before_previous) {
      state_.golden_pending_session_fallbacks.store(0, std::memory_order_release);
      return outcome;
    }
    const bool restored_golden = try_golden();
    state_.golden_pending_session_fallbacks.store(0, std::memory_order_release);
    outcome.success = restored_golden;
    outcome.snapshot = restored;
    return outcome;
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
