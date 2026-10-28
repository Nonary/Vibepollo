/**
 * @file src/audio_steam_toggle_policy.h
 * @brief Platform-neutral policy for auto-enabling/disabling Steam Streaming audio devices.
 */
#pragma once

#include <cstdint>

namespace audio::steam_toggle {
  enum class session_event_e : int {
    CONNECT,        ///< Streaming session connected
    DISCONNECT,     ///< Streaming session disconnected
    CONFIG_CHANGED, ///< Config was modified (enable flag changed)
  };
  using session_event_t = session_event_e;

  enum class toggle_action_e : int {
    none,    ///< No action needed
    enable,  ///< Enable Steam devices and set Steam Microphone as default capture
    disable, ///< Disable Steam devices and restore
  };

  struct session_toggle_state_t {
    bool devices_enabled {false};   ///< Steam devices are currently enabled by us
    bool has_snapshot {false};      ///< We have a snapshot of pre-session defaults
    bool config_enabled {false};    ///< The auto-toggle config flag is enabled
  };

  struct toggle_result_t {
    toggle_action_e action {toggle_action_e::none};
    bool should_snapshot {false};   ///< Take a snapshot before enabling
    bool should_restore {false};    ///< Restore snapshot after disabling
  };

  toggle_result_t evaluate_toggle(session_toggle_state_t &state, session_event_e event, bool config_enabled);
}  // namespace audio::steam_toggle