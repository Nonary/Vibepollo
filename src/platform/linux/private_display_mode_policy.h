/**
 * @file src/platform/linux/private_display_mode_policy.h
 * @brief Mode-selection policy helpers for Linux private displays.
 */
#pragma once

#include <cmath>

namespace platf::linux_private_display::mode_policy {
  inline constexpr double refresh_tolerance_hz = 0.2;

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
