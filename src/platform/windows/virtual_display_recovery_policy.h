#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace VDISPLAY::policy {
  // If the caller has an exact stable target identity, do not let a client
  // friendly-name match replace it during reuse.
  inline std::optional<std::string> select_reuse_device_id(
    bool reclaiming_owned_display,
    bool has_expected_stable_identity,
    std::optional<std::string> exact_owned_device_id,
    std::optional<std::string> client_name_candidate
  ) {
    if (reclaiming_owned_display) {
      return exact_owned_device_id;
    }
    if (has_expected_stable_identity) {
      return std::nullopt;
    }
    return client_name_candidate;
  }

  template<typename ResolveByDeviceId, typename IsGdiName>
  std::optional<std::wstring> bind_reuse_display_name_to_device_id(
    bool require_stable_identity,
    const std::optional<std::string> &device_id,
    std::optional<std::wstring> pre_readiness_name,
    ResolveByDeviceId &&resolve_by_device_id,
    IsGdiName &&is_gdi_name
  ) {
    if (!require_stable_identity) {
      return pre_readiness_name;
    }
    if (!device_id || device_id->empty()) {
      return std::nullopt;
    }

    auto exact_name = std::forward<ResolveByDeviceId>(resolve_by_device_id)(*device_id);
    if (!exact_name || exact_name->empty() || !std::forward<IsGdiName>(is_gdi_name)(*exact_name)) {
      return std::nullopt;
    }
    return exact_name;
  }

  template<typename ResolveByDeviceId, typename ResolveByClientName, typename ResolveAny>
  std::optional<std::wstring> resolve_reuse_display_name(
    bool require_exact_device_id,
    const std::optional<std::string> &exact_owned_device_id,
    bool allow_any_fallback,
    ResolveByDeviceId &&resolve_by_device_id,
    ResolveByClientName &&resolve_by_client_name,
    ResolveAny &&resolve_any
  ) {
    if (require_exact_device_id) {
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

  template<typename CandidateRange, typename IsVirtual, typename MatchesStableEdid>
  std::size_t count_stable_identity_matches(
    const CandidateRange &candidates,
    IsVirtual &&is_virtual,
    MatchesStableEdid &&matches_stable_edid
  ) {
    return static_cast<std::size_t>(std::count_if(
      candidates.begin(),
      candidates.end(),
      [&](const auto &candidate) {
        return is_virtual(candidate) && matches_stable_edid(candidate);
      }
    ));
  }

  inline bool stable_identity_candidate_is_unique(
    bool candidate_matches,
    std::size_t matching_candidate_count
  ) noexcept {
    return candidate_matches && matching_candidate_count == 1;
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

  /**
   * Keeps an exclusive stream layout when Windows swaps layouts by itself.
   *
   * Windows stores one layout for each set of connected displays and loads
   * the stored one whenever that set changes, even when the display that came
   * or went is switched off in the current layout. A sleeping monitor that
   * drops its link for a second can therefore switch another monitor on in
   * the middle of an exclusive stream, and off again when the link returns.
   *
   * The hold asks for the extra displays to be removed and the result saved
   * while the changed set is still connected. Windows then stores the stream
   * layout for that set too, and stops swapping.
   *
   * It only acts when the connected set differs from the one last seen with a
   * correct layout. A display the user switches on by hand leaves that set
   * unchanged, so it is left alone, and stays left alone through later
   * hotplugs until the layout is stream-only again.
   */
  class exclusive_layout_hold_t {
  public:
    using time_point = std::chrono::steady_clock::time_point;

    /// Windows reports a changed connected set before it finishes loading the
    /// layout stored for it. A correct layout counts as the reference only
    /// after it has outlasted that gap.
    static constexpr auto settle_time = std::chrono::seconds(3);
    static constexpr auto min_repair_interval = std::chrono::milliseconds(500);
    /// A repair that lands after the set has changed back is saved for the
    /// wrong set, so several tries may be needed. Each distinct set needs one.
    /// The count starts again once a stream-only layout has settled.
    static constexpr unsigned int max_repairs = 8;

    enum class action_e {
      none,
      repair,
    };

    /**
     * Take one reading of the desktop.
     * @param connected Every connected display, switched on or not.
     * @param target_active The stream display is switched on.
     * @param other_active Some display other than the stream display is switched on.
     */
    action_e observe(
      const std::set<std::string> &connected,
      const bool target_active,
      const bool other_active,
      const time_point now
    ) {
      if (!target_active) {
        // Reactivation and recreation own a stream display that is off.
        candidate_.reset();
        return action_e::none;
      }
      if (!other_active) {
        manual_ = false;
        if (!candidate_ || *candidate_ != connected) {
          candidate_ = connected;
          candidate_since_ = now;
        }
        if (now - candidate_since_ >= settle_time) {
          reference_ = candidate_;
          repairs_ = 0;
        }
        return action_e::none;
      }

      candidate_.reset();
      if (!reference_) {
        return action_e::none;
      }
      if (connected == *reference_) {
        // Windows only swaps layouts when the set changes, so this display
        // was switched on by the user or another tool.
        manual_ = true;
        return action_e::none;
      }
      if (manual_ || repairs_ >= max_repairs) {
        return action_e::none;
      }
      if (last_repair_ && now - *last_repair_ < min_repair_interval) {
        return action_e::none;
      }
      return action_e::repair;
    }

    /// Count a repair that was attempted, whether or not it held.
    void note_repair(const time_point now) noexcept {
      ++repairs_;
      last_repair_ = now;
    }

    /// Forget what was learned. Used while the stream display is not meant to
    /// be alone, so the hold starts from a freshly settled layout afterwards.
    void suspend() noexcept {
      reference_.reset();
      candidate_.reset();
      manual_ = false;
    }

    unsigned int repairs() const noexcept {
      return repairs_;
    }

    bool exhausted() const noexcept {
      return repairs_ >= max_repairs;
    }

  private:
    std::optional<std::set<std::string>> reference_;
    std::optional<std::set<std::string>> candidate_;
    time_point candidate_since_ {};
    std::optional<time_point> last_repair_;
    unsigned int repairs_ {0};
    bool manual_ {false};
  };
}  // namespace VDISPLAY::policy
