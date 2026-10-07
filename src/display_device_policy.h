/**
 * @file src/display_device_policy.h
 * @brief Data-only display configuration policy.
 */
#pragma once

#include <cstdint>
#include <compare>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace display_device::policy {
  struct resolution_t {
    unsigned int m_width {};
    unsigned int m_height {};
    auto operator<=>(const resolution_t &) const = default;
  };

  struct rational_t {
    unsigned int m_numerator {};
    unsigned int m_denominator {};

    // Refresh rates may reach this policy as either Hz or millihertz.  Keep
    // the representation supplied by the caller, but compare the numeric
    // value so 60/1 and 60000/1000 describe the same requested mode.
    [[nodiscard]] bool operator==(const rational_t &other) const {
      if (m_denominator == 0 || other.m_denominator == 0) {
        return m_numerator == other.m_numerator && m_denominator == other.m_denominator;
      }
      return static_cast<std::uint64_t>(m_numerator) * other.m_denominator ==
             static_cast<std::uint64_t>(other.m_numerator) * m_denominator;
    }
  };

  enum class device_preparation_e { VerifyOnly, EnsureActive, EnsurePrimary, EnsureOnlyDisplay };
  enum class hdr_state_e { Disabled, Enabled };

  struct configuration_t {
    std::string m_device_id;
    device_preparation_e m_device_prep {};
    std::optional<resolution_t> m_resolution;
    std::optional<rational_t> m_refresh_rate;
    std::optional<hdr_state_e> m_hdr_state;
  };

  struct failed_to_parse_tag_t {};
  struct configuration_disabled_tag_t {};

  struct video_config_t {
    struct dd_t {
      enum class config_option_e { disabled, verify_only, ensure_active, ensure_primary, ensure_only_display };
      enum class resolution_option_e { disabled, automatic, manual };
      enum class refresh_rate_option_e { disabled, automatic, manual, prefer_highest };
      enum class hdr_option_e { disabled, automatic };
      struct mode_remapping_entry_t {
        std::string requested_resolution;
        std::string requested_fps;
        std::string final_resolution;
        std::string final_refresh_rate;
      };
      struct mode_remapping_t {
        std::vector<mode_remapping_entry_t> mixed;
        std::vector<mode_remapping_entry_t> resolution_only;
        std::vector<mode_remapping_entry_t> refresh_rate_only;
      };
      struct workarounds_t { bool dummy_plug_hdr10 {}; } wa;
      config_option_e configuration_option {};
      resolution_option_e resolution_option {};
      std::string manual_resolution;
      refresh_rate_option_e refresh_rate_option {};
      std::string manual_refresh_rate;
      hdr_option_e hdr_option {};
      mode_remapping_t mode_remapping;
    } dd;
    std::string output_name;
    bool rtx_hdr_enabled {};
  };

  struct session_t {
    struct resolution_override_t { int width; int height; };
    int width {};
    int height {};
    int fps {};
    bool enable_sops {};
    bool enable_hdr {};
    bool prefer_sdr_10bit {};
    bool force_sdr {};
    bool client_display_mode_override {};
    std::uint32_t client_display_refresh_millihz {};
    std::optional<resolution_override_t> resolution_override;
    std::optional<int> framegen_refresh_rate;
    std::optional<std::uint32_t> framegen_refresh_millihz;
  };

  [[nodiscard]] bool effective_hdr_requested(const session_t &session);
  [[nodiscard]] bool effective_10bit_sdr_requested(const session_t &session);
  [[nodiscard]] std::variant<failed_to_parse_tag_t, configuration_disabled_tag_t, configuration_t> parse_configuration(const video_config_t &video_config, const session_t &session);
  [[nodiscard]] bool refresh_rate_override_active(const video_config_t &video_config, const session_t &session);
}  // namespace display_device::policy
