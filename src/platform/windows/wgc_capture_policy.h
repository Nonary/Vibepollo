#pragma once

#include <cstdint>

namespace platf::dxgi::wgc_policy {
  inline constexpr std::uint32_t low_latency_initial_buffer_size = 1;
  inline constexpr std::uint32_t adaptive_max_buffer_size = 2;
  inline constexpr std::uint32_t helper_stop_timeout_ms = 3000;

  enum class capture_surface_format : std::uint8_t {
    bgra8,
    rgba16_float,
  };

  constexpr capture_surface_format select_capture_surface_format(
    const bool config_received,
    const bool force_sdr_capture,
    const bool dynamic_range,
    const bool advanced_color_capture
  ) noexcept {
    return config_received &&
             !force_sdr_capture &&
             (dynamic_range || advanced_color_capture) ?
             capture_surface_format::rgba16_float :
             capture_surface_format::bgra8;
  }

  constexpr std::uint32_t maximum_buffer_size(const bool vrr_low_latency) noexcept {
    return vrr_low_latency ? low_latency_initial_buffer_size : adaptive_max_buffer_size;
  }

  constexpr bool buffer_pool_is_quiet(
    const bool allow_decrease,
    const bool has_recent_drop,
    const bool recent_pool_pressure,
    const int peak_outstanding,
    const std::uint32_t current_buffer_size
  ) noexcept {
    return allow_decrease &&
           !has_recent_drop &&
           !recent_pool_pressure &&
           peak_outstanding <= static_cast<int>(current_buffer_size) - 1;
  }
}  // namespace platf::dxgi::wgc_policy
