/**
 * @file src/platform/linux/private_display_resume_policy.h
 * @brief Resume policy for Linux private streaming displays.
 */
#pragma once

namespace platf::linux_private_display::resume_policy {
  /** A newly hot-plugged output must be configured even if KWin auto-enabled it. */
  constexpr bool requires_apply(const bool newly_connected, const bool enabled) {
    return newly_connected || !enabled;
  }
}  // namespace platf::linux_private_display::resume_policy
