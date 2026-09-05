/**
 * @file src/platform/linux/private_display_mode_policy.h
 * @brief Mode-selection policy helpers for Linux private displays.
 */
#pragma once

#include <cmath>
#include <cstdint>
#include <string_view>

namespace platf::linux_private_display::mode_policy {
  inline constexpr double refresh_tolerance_hz = 0.2;

  struct requested_mode_t {
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t refresh_millihz {};

    [[nodiscard]] bool valid() const noexcept {
      return width >= 64 && width <= 8192 && height >= 64 && height <= 8192 &&
             refresh_millihz >= 1000 && refresh_millihz <= 1000000;
    }
  };

  [[nodiscard]] inline bool managed_connector_name(const std::string_view name) noexcept {
    return name == "Virtual-1" || name == "Virtual-2" || name == "Virtual-3" || name == "Virtual-4";
  }

  // KScreen 6.4 can report a parse/apply failure and still exit successfully.
  [[nodiscard]] inline bool doctor_reported_failure(const std::string_view output) noexcept {
    return output.find("Unable to parse arguments:") != std::string_view::npos ||
           output.find("applying config failed!") != std::string_view::npos ||
           output.find("Invalid config.") != std::string_view::npos;
  }

  /**
   * Treat fractional representations of the same nominal refresh as equal,
   * while rejecting a nearest mode that would visibly change stream pacing.
   */
  [[nodiscard]] inline bool refresh_matches(
    const double available_hz,
    const double requested_hz
  ) noexcept {
    return std::isfinite(available_hz) && std::isfinite(requested_hz) &&
           std::abs(available_hz - requested_hz) < refresh_tolerance_hz;
  }
}  // namespace platf::linux_private_display::mode_policy
