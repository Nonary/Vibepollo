#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <utility>

namespace VDISPLAY::policy {
  // A reclaimed driver lease is already owned by one stable display ID. Do
  // not let a client friendly-name match replace that identity during reuse.
  inline std::optional<std::string> select_reuse_device_id(
    bool reclaiming_owned_display,
    std::optional<std::string> exact_owned_device_id,
    std::optional<std::string> client_name_candidate
  ) {
    return reclaiming_owned_display ? std::move(exact_owned_device_id) : std::move(client_name_candidate);
  }

  template<typename ResolveByDeviceId, typename ResolveByClientName, typename ResolveAny>
  std::optional<std::wstring> resolve_reuse_display_name(
    bool reclaiming_owned_display,
    const std::optional<std::string> &exact_owned_device_id,
    bool allow_any_fallback,
    ResolveByDeviceId &&resolve_by_device_id,
    ResolveByClientName &&resolve_by_client_name,
    ResolveAny &&resolve_any
  ) {
    if (reclaiming_owned_display) {
      if (!exact_owned_device_id || exact_owned_device_id->empty()) {
        return std::nullopt;
      }
      return std::forward<ResolveByDeviceId>(resolve_by_device_id)(*exact_owned_device_id);
    }

    if (auto name = std::forward<ResolveByClientName>(resolve_by_client_name)()) {
      return name;
    }
    if (allow_any_fallback) {
      return std::forward<ResolveAny>(resolve_any)();
    }
    return std::nullopt;
  }

  template<typename ResolveByDeviceId, typename ResolveByName>
  std::optional<std::wstring> resolve_monitor_device_path(
    const std::optional<std::string> &device_id,
    ResolveByDeviceId &&resolve_by_device_id,
    ResolveByName &&resolve_by_name
  ) {
    if (device_id && !device_id->empty()) {
      return std::forward<ResolveByDeviceId>(resolve_by_device_id)(*device_id);
    }
    return std::forward<ResolveByName>(resolve_by_name)();
  }

  inline bool readiness_has_dynamic_identity_hint(
    bool expected_display_id,
    bool device_id,
    bool display_name,
    bool monitor_path,
    bool gdi_name,
    bool friendly_name
  ) noexcept {
    return expected_display_id || device_id || display_name || monitor_path || gdi_name || friendly_name;
  }

  inline bool readiness_has_strong_identity_hint(
    bool expected_display_id,
    bool device_id,
    bool monitor_path,
    bool gdi_name
  ) noexcept {
    return expected_display_id || device_id || monitor_path || gdi_name;
  }

  inline bool readiness_rejects_weak_identity_candidate(
    bool has_strong_identity_hint,
    bool is_virtual_display
  ) noexcept {
    return has_strong_identity_hint || !is_virtual_display;
  }

  inline bool readiness_candidate_is_exact_target(
    bool require_stable_display_id,
    bool strong_identity_match,
    bool stable_edid_match
  ) noexcept {
    // During creation, Windows can reassign names and monitor paths while it
    // publishes topology changes. If the caller knows the requested EDID
    // identity, only that stable identity may prove the created target.
    if (require_stable_display_id) {
      return stable_edid_match;
    }
    return strong_identity_match || stable_edid_match;
  }

  inline bool defer_unidentified_readiness_candidate(
    bool exact_target,
    bool has_dynamic_identity_hint
  ) noexcept {
    return !exact_target && !has_dynamic_identity_hint;
  }

  class inactive_reactivation_budget_t {
  public:
    using duration = std::chrono::steady_clock::duration;

    static constexpr auto attempt_interval = std::chrono::milliseconds(750);
    static constexpr auto fallback_grace = std::chrono::seconds(12);
    static constexpr unsigned int max_attempts = 3;

    bool consume_if_due(duration inactive_for) noexcept {
      const auto next_attempt_age = attempt_interval * static_cast<int>(attempts_ + 1);
      if (attempts_ >= max_attempts || inactive_for < next_attempt_age) {
        return false;
      }
      ++attempts_;
      return true;
    }

    void reset() noexcept {
      attempts_ = 0;
    }

    unsigned int attempts() const noexcept {
      return attempts_;
    }

  private:
    unsigned int attempts_ {0};
  };
}  // namespace VDISPLAY::policy
