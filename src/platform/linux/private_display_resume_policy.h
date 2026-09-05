/**
 * @file src/platform/linux/private_display_resume_policy.h
 * @brief Resume policy for Linux private streaming displays.
 */
#pragma once

#include <cstdint>
#include <string_view>

namespace platf::linux_private_display::resume_policy {
  /** A resume attaches to the app's display lease, not a new client's identity. */
  constexpr std::string_view reservation_owner(
    const std::string_view requesting_client,
    const std::string_view app_client,
    const std::uint64_t app_display_token
  ) {
    return app_display_token != 0 && !app_client.empty() ? app_client : requesting_client;
  }

  /** A newly hot-plugged output must be configured even if KWin auto-enabled it. */
  constexpr bool requires_apply(const bool newly_connected, const bool enabled) {
    return newly_connected || !enabled;
  }

  /**
   * Recompose only when the normal-game identity or its output is new.
   * Replaying a retained topology can transiently modeset an already-fullscreen
   * game to the raw client mode before the session's remapped mode is applied.
   */
  constexpr bool requires_topology_reapply(
    const bool newly_reserved_identity,
    const bool display_needs_apply
  ) {
    return newly_reserved_identity || display_needs_apply;
  }

  /** Physical displays retain the legacy apply gate; ready private outputs do not need a modeset on resume. */
  constexpr bool requires_session_apply(
    const bool virtual_display,
    const bool allow_display_changes,
    const bool newly_reserved_identity,
    const bool display_needs_apply
  ) {
    return virtual_display ?
             requires_topology_reapply(newly_reserved_identity, display_needs_apply) :
             allow_display_changes;
  }
}  // namespace platf::linux_private_display::resume_policy
