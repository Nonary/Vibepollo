#include "src/platform/windows/display_helper_v2/operations.h"

#include <algorithm>
#include <sstream>

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
  }  // namespace

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

  ApplyOperation::ApplyOperation(IDisplaySettings &display, IClock &clock)
    : display_(display),
      clock_(clock) {}

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

    // SDC_VALIDATE soft-test with one display-stack recovery retry (legacy handle_apply).
    bool validated = display_.soft_test(*request.configuration, request.topology);
    if (!validated) {
      BOOST_LOG(warning) << "Display helper: configuration failed SDC_VALIDATE soft-test; attempting display stack recovery and retrying once.";
      if (display_.recover_display_stack()) {
        clock_.sleep_for(std::chrono::milliseconds(500));
        if (token.is_cancelled()) {
          outcome.status = ApplyStatus::Fatal;
          return outcome;
        }
        validated = display_.soft_test(*request.configuration, request.topology);
      }
    }
    if (!validated) {
      BOOST_LOG(error) << "Display helper: configuration failed SDC_VALIDATE soft-test; not applying.";
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

    // Best-effort base topology pre-set (legacy apply(cfg, base_topology)).
    if (request.topology && display_.topology_is_valid(*request.topology)) {
      (void) display_.apply_topology(*request.topology);
    }

    outcome.status = display_.apply(*request.configuration);

    if (outcome.status == ApplyStatus::Ok) {
      apply_monitor_positions(request, token);
      apply_refresh_rate_overrides(request, token);
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

      // Sticky verification (legacy verify_last_configuration_sticky): Windows can
      // revert a mode/HDR change moments after it reports success, so re-check
      // after a settle delay before declaring victory.
      clock_.sleep_for(std::chrono::milliseconds(250));
      if (token.is_cancelled()) {
        return false;
      }
      if (!display_.configuration_matches(*request.configuration)) {
        return false;
      }
    }

    return true;
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
      clock_(clock) {}

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

    auto apply_once = [&]() {
      (void) display_.apply_snapshot(base);
      if (require_layout_match && !layouts.empty()) {
        (void) display_.apply_layout_rotations(layouts);
      }
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
    apply_once();
    if (verify_once("#1")) {
      return true;
    }

    // Attempt 2 (double-check) after a short delay
    if (token.is_cancelled() || !wait_with_cancel(std::chrono::milliseconds(700), token)) {
      return false;
    }
    if (confirm_matches(loaded, label, token)) {
      return true;
    }
    apply_once();
    return verify_once("#2");
  }

  std::set<std::string> RecoveryOperation::known_present_devices() {
    std::set<std::string> result;
    try {
      // Active devices (have modes)
      const auto snap = display_.capture_snapshot();
      for (const auto &kv : snap.m_modes) {
        result.insert(kv.first);
      }
      // Enumerated devices (active or inactive)
      for (const auto &d : display_.enumerate(display_device::DeviceEnumerationDetail::Minimal)) {
        const auto id = d.m_device_id.empty() ? d.m_display_name : d.m_device_id;
        if (!id.empty()) {
          result.insert(id);
        }
      }
      // Fallback to topology flatten if the above produced nothing
      if (result.empty()) {
        for (const auto &grp : snap.m_topology) {
          result.insert(grp.begin(), grp.end());
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
        golden_devices.insert(id);
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
        BOOST_LOG(info) << "Golden restore confirmed; clearing session restore snapshots.";
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
