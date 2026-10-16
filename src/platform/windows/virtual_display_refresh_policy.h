/**
 * @file src/platform/windows/virtual_display_refresh_policy.h
 * @brief Refresh limits for the active-area timings advertised by Windows virtual displays.
 */
#pragma once

#include "src/framegen_policy.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>

namespace VDISPLAY::policy {
  struct creation_refresh_t {
    std::uint32_t requested_millihz;
    std::uint32_t descriptor_millihz;
  };
  inline std::uint32_t maximum_refresh_millihz(
    std::uint32_t total_height,
    std::optional<std::uint32_t> windows_build
  ) {
    if (windows_build && *windows_build >= 26100) {
      return std::numeric_limits<std::uint32_t>::max();
    }
    // Pre-24H2 IddCx rejects horizontal scan frequencies above 1 MHz.
    // Our driver uses total_height == active height. Floor the millihertz
    // result so fractional maximum modes never cross the validation limit.
    // Unknown OS versions use the older limit.
    return 1'000'000'000u / std::max(total_height, 1u);
  }

  inline std::uint32_t limit_refresh_millihz(
    std::uint32_t requested,
    std::uint32_t total_height,
    std::optional<std::uint32_t> windows_build
  ) {
    return std::min(requested, maximum_refresh_millihz(total_height, windows_build));
  }

  inline creation_refresh_t resolve_creation_refresh(
    std::uint32_t requested,
    std::uint32_t base,
    int multiplier,
    std::uint32_t total_height,
    std::optional<std::uint32_t> windows_build
  ) {
    requested = std::max(requested, framegen::saturating_refresh_millihz(base, multiplier));
    const auto limited = limit_refresh_millihz(requested, total_height, windows_build);
    // A capped maximum is not necessarily a multiple of the client rate.
    // Advertise that exact rate so APPLY does not fall back to a static mode.
    return {limited, limited != requested || base == 0 ? limited : base};
  }
}  // namespace VDISPLAY::policy
