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
