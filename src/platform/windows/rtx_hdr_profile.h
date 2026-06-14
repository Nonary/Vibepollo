/**
 * @file src/platform/windows/rtx_hdr_profile.h
 * @brief Read-only NVIDIA RTX HDR profile resolution helpers.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace platf::rtx_hdr {

  enum class profile_source_e {
    none,
    application,
    global,
    config
  };

  struct profile_values_t {
    std::optional<bool> enabled;
    std::optional<int> contrast;
    std::optional<int> saturation;
    std::optional<int> middle_gray;
    std::optional<int> peak_brightness;

    bool has_any() const {
      return enabled || contrast || saturation || middle_gray || peak_brightness;
    }
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
    int peak_brightness {1000};
    profile_source_e source {profile_source_e::none};
  };

  resolved_profile_t resolve_profile_for_executable(const std::string &executable);
  std::optional<int> resolve_session_peak_brightness(const std::string &executable = {});

  runtime_values_t materialize_runtime_values_for_tests(
    const resolved_profile_t &resolved,
    const runtime_values_t &config_fallback
  );
  runtime_values_t materialize_live_tuning_values(
    const resolved_profile_t &resolved,
    const runtime_values_t &config_fallback,
    bool enabled
  );

  std::optional<bool> decode_rtx_hdr_activation_for_tests(
    std::optional<std::uint32_t> driver_flags,
    std::optional<std::uint32_t> profile_enable
  );
  std::optional<int> decode_rtx_hdr_contrast_units_for_tests(std::uint32_t raw);
  std::optional<int> decode_rtx_hdr_saturation_units_for_tests(std::uint32_t raw);

  const char *source_name(profile_source_e source);

}  // namespace platf::rtx_hdr
