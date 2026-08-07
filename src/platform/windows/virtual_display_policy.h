#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace VDISPLAY::policy {
  inline constexpr std::string_view ensure_display_stable_id = "sunshine-ensure";
  inline constexpr auto hdr_activation_timeout = std::chrono::seconds {3};
  inline constexpr auto enumeration_timeout = std::chrono::seconds {2};
  inline constexpr auto activation_grace = std::chrono::milliseconds {500};
  inline constexpr auto readiness_poll_interval = std::chrono::milliseconds {50};

  constexpr bool should_ensure_probe_display(const bool session_uses_virtual_display) noexcept {
    return !session_uses_virtual_display;
  }

  constexpr bool should_prepare_display_for_new_session(const bool no_active_sessions) noexcept {
    return no_active_sessions;
  }

  // The temporary output created by ensure_display() is probe-scoped. Once a
  // caller reaches any terminal probe path, it must release that output rather
  // than cache it for an unrelated stream or later retry.
  constexpr bool should_cleanup_temporary_probe(const bool tracks_temporary_for_probe) noexcept {
    return tracks_temporary_for_probe;
  }

  enum class probe_display_release_action : std::uint8_t {
    ignored,
    retained_for_other_probe,
    remove,
  };

  // Tracks in-process users of the deterministic encoder-probe display. The
  // display may be shared by overlapping startup/discovery probes, but only
  // the final user is allowed to remove it. Generations fence a late cleanup
  // from a later display that happens to reuse the same stable GUID.
  class probe_display_lifetime_t {
  public:
    [[nodiscard]] std::optional<std::uint64_t> begin_lifetime() noexcept {
      if (retained_) {
        return std::nullopt;
      }
      ++generation_;
      if (generation_ == 0) {
        ++generation_;
      }
      retained_ = true;
      removal_in_progress_ = false;
      active_probes_ = 1;
      return generation_;
    }

    [[nodiscard]] std::optional<std::uint64_t> acquire() noexcept {
      if (!retained_ || removal_in_progress_) {
        return std::nullopt;
      }
      ++active_probes_;
      return generation_;
    }

    [[nodiscard]] probe_display_release_action release(const std::uint64_t generation) noexcept {
      if (!retained_ || removal_in_progress_ || generation == 0 ||
          generation != generation_ || active_probes_ == 0) {
        return probe_display_release_action::ignored;
      }
      --active_probes_;
      if (active_probes_ != 0) {
        return probe_display_release_action::retained_for_other_probe;
      }
      removal_in_progress_ = true;
      return probe_display_release_action::remove;
    }

    [[nodiscard]] std::optional<std::uint64_t> begin_idle_removal() noexcept {
      if (!retained_ || removal_in_progress_ || active_probes_ != 0) {
        return std::nullopt;
      }
      removal_in_progress_ = true;
      return generation_;
    }

    void complete_removal(const std::uint64_t generation, const bool succeeded) noexcept {
      if (!retained_ || !removal_in_progress_ || generation == 0 || generation != generation_) {
        return;
      }
      removal_in_progress_ = false;
      if (succeeded) {
        retained_ = false;
        active_probes_ = 0;
      }
    }

    [[nodiscard]] bool retained() const noexcept {
      return retained_;
    }

    [[nodiscard]] bool removal_in_progress() const noexcept {
      return removal_in_progress_;
    }

    [[nodiscard]] std::size_t active_probes() const noexcept {
      return active_probes_;
    }

  private:
    std::uint64_t generation_ = 0;
    std::size_t active_probes_ = 0;
    bool retained_ = false;
    bool removal_in_progress_ = false;
  };

  constexpr bool allow_generic_resume_fallback() noexcept {
    return false;
  }

  constexpr char ascii_lower(const char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
  }

  constexpr bool equals_ascii_ci(const std::string_view lhs, const std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) {
      return false;
    }
    for (std::size_t index = 0; index < lhs.size(); ++index) {
      if (ascii_lower(lhs[index]) != ascii_lower(rhs[index])) {
        return false;
      }
    }
    return true;
  }

  constexpr bool is_virtual_display_selection(
    const std::string_view value,
    const bool accept_sudovda_alias
  ) noexcept {
    return equals_ascii_ci(value, "sunshine:virtual_display") ||
           (accept_sudovda_alias && equals_ascii_ci(value, "sunshine:sudovda_virtual_display"));
  }

  constexpr bool adapter_preference_allows_creation(const bool preference_applied) noexcept {
    return preference_applied;
  }

  constexpr bool passive_install_status(const bool device_present) noexcept {
    return device_present;
  }

  constexpr bool should_release_retained_probe_display(const bool ensure_display_client) noexcept {
    return !ensure_display_client;
  }

  // A retained target remains owned after the driver accepts it, even when
  // Windows has not published a monitor/tracker entry yet. Sunshine supplies
  // a lease here; SudoVDA supplies accepted render-adapter provenance.
  constexpr bool retained_target_is_owned(
    const bool tracked_by_windows,
    const bool driver_owned
  ) noexcept {
    return tracked_by_windows || driver_owned;
  }

  enum class reclaimed_display_action : std::uint8_t {
    reuse,
    recreate,
  };

  struct reclaimed_display_plan {
    reclaimed_display_action action {reclaimed_display_action::recreate};
    bool preserve_device_identity = false;
    bool activation_apply_required = true;
  };

  constexpr bool refresh_matches(
    const std::uint32_t advertised_millihz,
    const std::uint32_t requested_millihz
  ) noexcept {
    return advertised_millihz >= requested_millihz ?
             advertised_millihz - requested_millihz <= 1u :
             requested_millihz - advertised_millihz <= 1u;
  }

  constexpr bool refresh_is_advertised(
    const std::span<const std::uint32_t> advertised_refreshes_millihz,
    const std::uint32_t requested_millihz
  ) noexcept {
    if (requested_millihz == 0) {
      return false;
    }
    for (const auto advertised : advertised_refreshes_millihz) {
      if (refresh_matches(advertised, requested_millihz)) {
        return true;
      }
    }
    return false;
  }

  // Replacing an active display still requires a fresh descriptor. A retained
  // inactive display can instead be reactivated in place when its descriptor
  // already advertises the requested mode, avoiding a same-connector PnP
  // depart/arrive cycle after REVERT. Adapter provenance always wins.
  constexpr reclaimed_display_plan reclaimed_display_plan_for_session(
    const bool replace_existing,
    const bool inactive,
    const bool requested_mode_available,
    const bool render_adapter_provenance_matches
  ) noexcept {
    const bool can_resurrect =
      replace_existing && inactive && requested_mode_available;
    const auto action =
      ((replace_existing && !can_resurrect) ||
       !render_adapter_provenance_matches) ?
        reclaimed_display_action::recreate :
        reclaimed_display_action::reuse;
    return {
      .action = action,
      .preserve_device_identity = action == reclaimed_display_action::reuse,
      // A retained inactive target is intentionally not activated by the
      // driver operation. The session helper must apply its topology/mode.
      .activation_apply_required = inactive && action == reclaimed_display_action::reuse,
    };
  }

  constexpr bool accept_enumerated_target(
    const std::chrono::steady_clock::duration elapsed_since_enumeration
  ) noexcept {
    return elapsed_since_enumeration >= activation_grace;
  }

  struct advanced_color_state_t {
    bool supported = false;
    bool hdr_supported = false;
    bool hdr_enabled = false;
    bool limited_by_policy = false;
    std::uint32_t bits_per_color_channel = 0;
  };

  constexpr bool hdr_target_ready(const advanced_color_state_t &state) noexcept {
    return state.supported &&
           state.hdr_supported &&
           state.hdr_enabled &&
           !state.limited_by_policy &&
           state.bits_per_color_channel >= 10;
  }

  constexpr bool should_reset_hdr_state_for_stream(
    const bool hdr_requested,
    const bool hdr_enabled
  ) noexcept {
    return !hdr_requested && hdr_enabled;
  }

  enum class hdr_activation_failure_action : std::uint8_t {
    none,
    continue_sdr,
    defer_to_display_helper,
  };

  constexpr hdr_activation_failure_action hdr_failure_action(
    const bool hdr_requested,
    const bool hdr_enabled,
    const bool confirmed_active
  ) noexcept {
    if (!hdr_requested || hdr_enabled) {
      return hdr_activation_failure_action::none;
    }
    return confirmed_active ?
             hdr_activation_failure_action::continue_sdr :
             hdr_activation_failure_action::defer_to_display_helper;
  }

  enum class identity_match_kind : std::uint8_t {
    stable_edid,
    exact_output,
    exact_client_name,
    generic_active,
    generic_inactive,
  };

  inline constexpr std::array identity_resolution_order {
    identity_match_kind::stable_edid,
    identity_match_kind::exact_output,
    identity_match_kind::exact_client_name,
    identity_match_kind::generic_active,
    identity_match_kind::generic_inactive,
  };

  enum class driver_status_class : std::uint8_t {
    ok,
    failed,
    version_incompatible,
  };

  constexpr driver_status_class classify_protocol_query(
    const bool query_succeeded,
    const bool protocol_incompatible
  ) noexcept {
    if (query_succeeded) {
      return driver_status_class::ok;
    }
    return protocol_incompatible ?
             driver_status_class::version_incompatible :
             driver_status_class::failed;
  }

  constexpr std::uint64_t normalize_opaque_lease_id(
    const std::uint64_t generated,
    const std::uint64_t minimum
  ) noexcept {
    return generated < minimum ? generated | minimum : generated;
  }

  constexpr bool should_reopen_control_transport(
    const bool transport_valid,
    const bool transport_responsive
  ) noexcept {
    return !transport_valid || !transport_responsive;
  }
}  // namespace VDISPLAY::policy
