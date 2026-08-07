/**
 * @file src/platform/windows/rtx_hdr_policy.h
 * @brief Dependency-free RTX HDR eligibility and profile precedence policy.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace platf::rtx_hdr {

  enum class profile_source_e { none, application, global, config };

  struct profile_values_t {
    std::optional<bool> enabled;
    std::optional<int> contrast;
    std::optional<int> saturation;
    std::optional<int> middle_gray;
    std::optional<int> peak_brightness;
    bool has_any() const { return enabled || contrast || saturation || middle_gray || peak_brightness; }
  };

  struct resolved_profile_t {
    bool lookup_available {false};
    profile_source_e source {profile_source_e::none};
    profile_values_t application;
    profile_values_t global;
    std::string executable;
    std::string profile_name;
  };

  struct runtime_values_t {
    bool enabled {false};
    int contrast {100};
    int saturation {100};
    int middle_gray {50};
    int sdr_brightness {0};
    int peak_brightness {1000};
    profile_source_e source {profile_source_e::none};
  };

  namespace policy {
    struct overrides_t {
      bool enable {false};
      bool contrast {false};
      bool saturation {false};
      bool middle_gray {false};
      bool peak_brightness {false};
    };

    runtime_values_t materialize(const resolved_profile_t &resolved, const runtime_values_t &config, const overrides_t &overrides);
    runtime_values_t desktop_values(const runtime_values_t &config, bool runtime_enabled);
    std::optional<bool> decode_activation(std::optional<std::uint32_t> driver_flags, std::optional<std::uint32_t> profile_enable);
    std::optional<int> decode_percent_units(std::uint32_t raw);
    float sdr_brightness_to_white_nits(int brightness);
    bool playnite_foreground_matches(std::string_view active_playnite_id, std::string_view status_id, std::string_view status_exe, std::string_view status_install_dir, std::string_view foreground_exe);
  }
}
