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
