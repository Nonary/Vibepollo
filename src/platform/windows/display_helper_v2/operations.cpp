#include "src/platform/windows/display_helper_v2/operations.h"
#include "src/platform/windows/display_helper_v2/topology_policy.h"

#include <algorithm>
#include <sstream>

#include <boost/algorithm/string/predicate.hpp>

#include "src/platform/windows/display_helper_v2/diagnostics.h"

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
      const ActiveTopology &planned_topology) {
      if (activation_target.kind == DeviceTargetKind::None ||
          activation_target.acceptable_device_ids.empty()) {
        return std::nullopt;
      }

      for (const auto &group : planned_topology) {
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

  }  // namespace

  TopologyTransition::TopologyTransition(IDisplaySettings &display, IClock &clock)
    : display_(display),
      clock_(clock) {}

  std::optional<ActiveTopology> TopologyTransition::topology_ready(
    const ActiveTopology &requested_topology) {
    const auto current = display_.capture_topology();
    if (!display_.topology_is_valid(current)) {
      return std::nullopt;
    }

    if (!display_.is_topology_same(requested_topology, current)) {
      return std::nullopt;
    }

    std::vector<std::string> required_device_ids;
    for (const auto &group : current) {
      required_device_ids.insert(
        required_device_ids.end(),
        group.begin(),
        group.end());
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
    const CancellationToken &token) {
    const auto deadline = clock_.now() + kActivationTimeout;
    while (!token.is_cancelled()) {
      if (auto ready = topology_ready(requested_topology)) {
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

    // Capture can expose a requested path before Windows makes it enumerable.
    // Readiness therefore belongs after SetDisplayConfig, not in an APPLY
    // preflight that would turn a valid delayed-publication transition into a
    // recovery path.
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
      // Snapshot restoration requires the exact saved topology and every
      // restored path to become enumerable before settings are replayed.
      if (auto applied_topology = wait_until_ready(topology, token)) {
        BOOST_LOG(info) << "Display helper v2: restored topology is active and all devices are enumerable.";
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

  ApplyOperation::ApplyOperation(
    IDisplaySettings &display,
    IClock &clock,
    MutationBoundary mutation_boundary)
    : display_(display),
      mutation_boundary_(std::move(mutation_boundary)) {
    (void) clock;
  }

  bool ApplyOperation::arm_durable_recovery_boundary() {
    return mutation_boundary_ && mutation_boundary_();
  }

  ApplyOutcome ApplyOperation::run(
    const ApplyRequest &request,
    const CancellationToken &token,
    bool durable_recovery_already_armed,
    bool durable_recovery_already_attempted) {
    ApplyOutcome outcome;
    outcome.virtual_display_requested = request.virtual_layout.has_value();
    bool durable_recovery_armed = durable_recovery_already_armed;
    bool durable_recovery_attempted = durable_recovery_already_armed || durable_recovery_already_attempted;
    outcome.durable_recovery_armed = durable_recovery_armed;
    outcome.durable_recovery_attempted = durable_recovery_attempted;
    const auto ensure_durable_recovery = [this, &durable_recovery_armed, &durable_recovery_attempted]() {
      if (durable_recovery_armed) {
        return true;
      }
      if (durable_recovery_attempted) {
        return false;
      }
      durable_recovery_attempted = true;
      durable_recovery_armed = arm_durable_recovery_boundary();
      return durable_recovery_armed;
    };
    if (token.is_cancelled()) {
      outcome.status = ApplyStatus::Fatal;
      return outcome;
    }

    if (!request.configuration) {
      outcome.status = ApplyStatus::InvalidRequest;
      return outcome;
    }

    if (request.settings_only_repair) {
      // Match v1's best_effort_apply_last_cfg(): the full transaction already
      // established topology, recovery ownership, and target scope. A repair
      // repeats only SettingsManager; it must not re-enumerate, validate/set
      // topology, or replay position and physical-refresh adjuncts.
      outcome.resolved_target = request.repair_target;
      if (!outcome.resolved_target && !request.configuration->m_device_id.empty()) {
        outcome.resolved_target = ResolvedConfigurationTarget {
          .kind = DeviceTargetKind::ExplicitDevice,
          .representative_device_id = request.configuration->m_device_id,
          .duplicate_device_ids = {request.configuration->m_device_id},
        };
      }
      outcome.display_may_have_changed = true;
      outcome.durable_recovery_attempted = true;
      outcome.staged_state_prepared = true;
      outcome.status = display_.apply(*request.configuration);
      if (token.is_cancelled()) {
        outcome.status = ApplyStatus::Fatal;
      }
      return outcome;
    }

    // Match the legacy helper's ordinary APPLY lane: one cheap preflight
    // against the supplied staging topology and no global display-stack
    // recovery. Recovery belongs to REVERT, where a broad topology jog is
    // intentional; doing it here destabilizes DWM immediately before capture.
    auto preflight = display_.preflight_apply(*request.configuration, request.topology);
    if (preflight.status != ApplyStatus::Ok || !preflight.plan) {
      BOOST_LOG(error) << "Display helper v2: configuration failed the non-mutating APPLY preflight.";
      outcome.status = preflight.status == ApplyStatus::Ok ? ApplyStatus::Fatal : preflight.status;
      return outcome;
    }
    auto plan = std::move(*preflight.plan);

    auto activation_target = std::move(plan.activation_target);
    if (!plan.topology.empty()) {
      outcome.resolved_target = resolve_configuration_target(activation_target, plan.topology);
    }
    if (!outcome.resolved_target && !request.configuration->m_device_id.empty()) {
      outcome.resolved_target = ResolvedConfigurationTarget {
        .kind = DeviceTargetKind::ExplicitDevice,
        .representative_device_id = request.configuration->m_device_id,
        .duplicate_device_ids = {request.configuration->m_device_id},
      };
    }

    if (token.is_cancelled()) {
      outcome.status = ApplyStatus::Fatal;
      return outcome;
    }

    // Arm the durable restore lease once, immediately before the first possible
    // mutation. The preflight above is deliberately read-only.
    if (mutation_boundary_) {
      outcome.durable_recovery_armed = ensure_durable_recovery();
      outcome.durable_recovery_attempted = durable_recovery_attempted;
      if (!outcome.durable_recovery_armed) {
        BOOST_LOG(warning) << "Display helper v2: could not arm durable recovery at APPLY mutation boundary.";
      }
    }

    if (token.is_cancelled()) {
      outcome.status = ApplyStatus::Fatal;
      return outcome;
    }

    // The supplied topology is a base, not an exact post-apply contract. Set it
    // once as v1 does, then let one original SettingsManager request own
    // topology, primary, mode, HDR, and its transactional rollback guards.
    if (request.topology && display_.topology_is_valid(*request.topology)) {
      outcome.display_may_have_changed = true;
      (void) display_.apply_topology(*request.topology);
    }
    if (token.is_cancelled()) {
      outcome.status = ApplyStatus::Fatal;
      return outcome;
    }

    outcome.display_may_have_changed = true;
    // SettingsManager may retain its original-state transaction even when a
    // later mode/HDR step fails, so recovery must reset it after this call.
    outcome.staged_state_prepared = true;
    outcome.status = display_.apply(*request.configuration);

    if (token.is_cancelled()) {
      outcome.status = ApplyStatus::Fatal;
      return outcome;
    }

    if (outcome.status == ApplyStatus::Ok) {
      apply_monitor_positions(request, token);
      apply_refresh_rate_overrides(request, token);
    }

    if (token.is_cancelled()) {
      outcome.status = ApplyStatus::Fatal;
    }

    return outcome;
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
    // Positioning is best effort and must not turn display enumeration into a
    // three-second stream-start gate. A later explicit refresh or APPLY can
    // retry a target that is not active yet.
    constexpr int kMaxRepositionAttempts = 1;

    auto pending_overrides = request.monitor_positions;
    int retry_attempt = 0;

    while (!pending_overrides.empty()) {
      if (token.is_cancelled()) {
        return;
      }
      ++retry_attempt;
      std::vector<std::pair<std::string, display_device::Point>> next_pending;
      next_pending.reserve(pending_overrides.size());

      std::set<std::string> pending_device_ids;
      for (const auto &[device_id, _] : pending_overrides) {
        if (!device_id.empty()) {
          pending_device_ids.insert(device_id);
        }
      }
      const auto repositionable_displays = display_.get_repositionable_display_resolutions(pending_device_ids);

      for (const auto &[device_id, origin] : pending_overrides) {
        if (token.is_cancelled()) {
          return;
        }
        if (device_id.empty()) {
          continue;
        }
        const auto display = repositionable_displays.find(device_id);
        if (display == repositionable_displays.end()) {
          next_pending.emplace_back(device_id, origin);
          continue;
        }
        int max_origin_x = kMaxDisplayOrigin;
        int max_origin_y = kMaxDisplayOrigin;
        if (const auto &res = display->second) {
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
    std::vector<std::pair<std::string, std::pair<unsigned int, unsigned int>>> valid_overrides;
    valid_overrides.reserve(request.refresh_rate_overrides.size());
    for (const auto &[device_id, rate] : request.refresh_rate_overrides) {
      if (token.is_cancelled()) {
        return;
      }
      if (device_id.empty() || rate.first == 0 || rate.second == 0) {
        continue;
      }
      // Skip the virtual display device itself.
      if (request.configuration && device_id == request.configuration->m_device_id) {
        continue;
      }
      valid_overrides.emplace_back(device_id, rate);
    }

    if (token.is_cancelled()) {
      return;
    }
    const auto applied = display_.set_device_refresh_rates(valid_overrides);
    bool rate_result = true;
    for (const auto &[device_id, rate] : valid_overrides) {
      const bool ok = applied.contains(device_id);
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
    if (success) {
      BOOST_LOG(info) << "Display helper: refresh-only request device=" << device_id
                      << " rate=" << numerator << '/' << denominator
                      << " result=true";
    } else {
      BOOST_LOG(warning) << "Display helper: refresh-only request device=" << device_id
                         << " rate=" << numerator << '/' << denominator
                         << " result=false";
    }
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

    // Observe immediately, then confirm the same target 250 ms later. This is
    // the legacy helper's capture-readiness boundary; sleeping before the first
    // sample only serialized another quarter second onto every stable start.
    // Sticky verification re-evaluates any explicit exact topology contract
    // supplied by a future caller, plus the resolved target and requested
    // settings. Normal APPLY treats `sunshine_topology` as a staging/base
    // topology, matching v1, so unrelated paths may be adjusted while a
    // sleeping monitor comes online.
    const auto matches_requested_state = [&]() {
      std::optional<ActiveTopology> current_topology;
      if (expected_topology) {
        current_topology = display_.capture_topology();
      }
      if (expected_topology) {
        if (!current_topology || !display_.is_topology_same(*expected_topology, *current_topology)) {
          return false;
        }
      }

      if (!request.configuration) {
        return true;
      }
      if (resolved_target ?
            !display_.configuration_matches(*request.configuration, *resolved_target) :
            !display_.configuration_matches(*request.configuration)) {
        return false;
      }
      return true;
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
      if (have_last && !emptyish && topology::equal_snapshot(cur, last)) {
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
      if (!topology::equal_snapshot(cur, base)) {
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
        token);
      if (topology_outcome.status != ApplyStatus::Ok) {
        BOOST_LOG(warning) << "Restore (" << label << "): topology activation stage failed with status="
                           << static_cast<int>(topology_outcome.status);
        return false;
      }
      // A competing APPLY/DISARM may arrive while the restore topology waits
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

  bool RecoveryOperation::golden_restore_is_pending() {
    auto loaded = storage_.load_with_metadata(SnapshotTier::Golden);
    if (!loaded) {
      return false;
    }

    const auto devices = display_.enumerate(display_device::DeviceEnumerationDetail::Minimal);
    if (codec::filter_loaded_snapshot(
          *loaded,
          devices,
          state_.exclusions(),
          "golden-pending-check")) {
      return true;
    }

    // A filtered load can fail because a required monitor is temporarily
    // absent. Keep the restore pending in that case, matching the legacy
    // helper's raw golden-file pending check. Explicit exclusions remain an
    // intentional exception and must not keep recovery armed forever.
    const auto available = known_present_devices();
    const auto snapshot_devices = codec::snapshot_device_set(loaded->snapshot);
    const auto exclusions = state_.exclusions();
    return std::any_of(snapshot_devices.begin(), snapshot_devices.end(), [&](const auto &device_id) {
      const auto normalized = codec::normalize_device_id(device_id);
      const bool excluded = std::any_of(exclusions.begin(), exclusions.end(), [&](const auto &excluded_id) {
        return codec::normalize_device_id(excluded_id) == normalized;
      });
      return !excluded && !available.contains(normalized);
    });
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

      if (golden_restore_is_pending()) {
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
