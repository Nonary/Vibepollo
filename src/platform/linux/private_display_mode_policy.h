/**
 * @file src/platform/linux/private_display_mode_policy.h
 * @brief Mode-selection policy helpers for Linux private displays.
 */
#pragma once

#include <cmath>
#include <charconv>
#include <cstdint>
#include <limits>
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
    constexpr std::string_view prefix = "Virtual-";
    if (!name.starts_with(prefix) || name.size() <= prefix.size()) {
      return false;
    }
    const auto suffix = name.substr(prefix.size());
    if (suffix.size() > 10 || suffix.front() < '1' || suffix.front() > '9') {
      return false;
    }
    std::uint32_t connector_type_id = 0;
    const auto [end, error] = std::from_chars(suffix.data(), suffix.data() + suffix.size(), connector_type_id);
    return error == std::errc {} && end == suffix.data() + suffix.size() &&
           connector_type_id <= static_cast<std::uint32_t>(std::numeric_limits<int>::max());
  }

  [[nodiscard]] inline bool should_admit_requested_mode(
    const bool has_resolution,
    const bool has_refresh,
    const bool prefer_highest,
    const bool managed_output
  ) noexcept {
    return has_resolution && has_refresh && !prefer_highest && managed_output;
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
