#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace VDISPLAY::policy {
  inline constexpr std::string_view ensure_display_stable_id = "sunshine-ensure";
  inline constexpr auto hdr_activation_timeout = std::chrono::seconds {3};
  inline constexpr auto enumeration_timeout = std::chrono::seconds {2};
  inline constexpr auto activation_grace = std::chrono::milliseconds {500};
  inline constexpr auto readiness_poll_interval = std::chrono::milliseconds {50};

  enum class display_identity_role : std::uint8_t {
    persistent_shared,
    per_client,
    encoder_probe,
  };

  constexpr bool persists_identity(const display_identity_role role) noexcept {
    return role == display_identity_role::persistent_shared;
  }

  constexpr bool should_ensure_probe_display(const bool session_uses_virtual_display) noexcept {
    return !session_uses_virtual_display;
  }

  constexpr bool should_prepare_display_for_new_session(const bool no_active_sessions) noexcept {
    return no_active_sessions;
  }

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
