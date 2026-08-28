/**
 * @file src/virtual_display_scale.h
 * @brief Cross-platform virtual-display scaling policy.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace virtual_display_scale {
  inline constexpr std::array<std::uint32_t, 12> supported_percentages {
    100,
    125,
    150,
    175,
    200,
    225,
    250,
    300,
    350,
    400,
    450,
    500
  };

  /** Resolve -1 to a resolution-based recommendation and preserve other values. */
  inline std::uint32_t effective_percent(
    const int configured_percent,
    const std::uint32_t width,
    const std::uint32_t height
  ) {
    if (configured_percent >= 0) {
      return static_cast<std::uint32_t>(configured_percent);
    }

    // Keep roughly 864 logical pixels on the short edge, then choose a scale
    // exposed by both Windows and the Vibeshine UI.
    const auto short_edge = (std::min) (width, height);
    const auto ideal_percent = static_cast<double>(short_edge) * 100.0 / 864.0;
    auto closest = supported_percentages.front();
    auto closest_distance = std::abs(static_cast<double>(closest) - ideal_percent);
    for (const auto candidate : supported_percentages) {
      const auto distance = std::abs(static_cast<double>(candidate) - ideal_percent);
      if (distance < closest_distance) {
        closest = candidate;
        closest_distance = distance;
      }
    }
    return closest;
  }

  inline bool supported_config_value(const int configured_percent) {
    return configured_percent == -1 || configured_percent == 0 ||
           std::ranges::find(supported_percentages, configured_percent) != supported_percentages.end();
  }

  /**
   * Resolve a compositor scale factor. A retained value is used only for the
   * preserve setting; callers decide whether the active compositor value or a
   * retained value is the better representation of the user's current choice.
   */
  inline double effective_factor(
    const int configured_percent,
    const std::uint32_t width,
    const std::uint32_t height,
    const double current_factor,
    const std::optional<double> retained_factor = std::nullopt
  ) {
    if (configured_percent == 0) {
      return retained_factor.value_or(current_factor);
    }
    return static_cast<double>(effective_percent(configured_percent, width, height)) / 100.0;
  }
}  // namespace virtual_display_scale
