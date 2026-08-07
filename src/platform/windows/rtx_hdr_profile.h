/**
 * @file src/platform/windows/rtx_hdr_profile.h
 * @brief Read-only NVIDIA RTX HDR profile resolution helpers.
 */
#pragma once

#include "rtx_hdr_policy.h"

namespace platf::rtx_hdr {

  resolved_profile_t resolve_profile_for_executable(const std::string &executable);
  std::optional<int> resolve_session_peak_brightness(const std::string &executable = {});

  runtime_values_t materialize_runtime_values_for_tests(
    const resolved_profile_t &resolved,
    const runtime_values_t &config_fallback
  );
  runtime_values_t materialize_live_values(
    const resolved_profile_t &resolved,
    const runtime_values_t &config_fallback
  );

  std::optional<bool> decode_rtx_hdr_activation_for_tests(
    std::optional<std::uint32_t> driver_flags,
    std::optional<std::uint32_t> profile_enable
  );
  std::optional<int> decode_rtx_hdr_contrast_units_for_tests(std::uint32_t raw);
  std::optional<int> decode_rtx_hdr_saturation_units_for_tests(std::uint32_t raw);

  const char *source_name(profile_source_e source);

}  // namespace platf::rtx_hdr
