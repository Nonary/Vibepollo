/**
 * @file src/audio_lifecycle_policy.h
 * @brief Pure decisions for shared audio ownership across app lifecycle edges.
 */
#pragma once

namespace audio::lifecycle {
  enum class release_action_e {
    restore,
    retain,
    ignore,
  };

  constexpr release_action_e final_owner_release_action(
    const bool restore_sink,
    const bool app_active,
    const bool app_termination_requested
  ) {
    if (!restore_sink) {
      return release_action_e::ignore;
    }
    if (app_active && !app_termination_requested) {
      return release_action_e::retain;
    }
    return release_action_e::restore;
  }

  constexpr bool can_reclaim_retained_audio(
    const bool retained,
    const bool app_active,
    const bool app_termination_requested
  ) {
    return retained && app_active && !app_termination_requested;
  }
}  // namespace audio::lifecycle
