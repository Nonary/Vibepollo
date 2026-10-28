/**
 * @file src/audio_steam_toggle_policy.cpp
 * @brief Implementation of the Steam audio device auto-toggle policy.
 */
#include "audio_steam_toggle_policy.h"

namespace audio::steam_toggle {
  toggle_result_t evaluate_toggle(session_toggle_state_t &state, session_event_e event, bool config_enabled) {
    state.config_enabled = config_enabled;
    toggle_result_t result;

    switch (event) {
      case session_event_e::CONNECT:
        if (!config_enabled || state.devices_enabled) {
          return result;
        }
        result.action = toggle_action_e::enable;
        result.should_snapshot = true;
        state.devices_enabled = true;
        state.has_snapshot = true;
        return result;

      case session_event_e::DISCONNECT:
        if (!state.devices_enabled) {
          return result;
        }
        result.action = toggle_action_e::disable;
        result.should_restore = state.has_snapshot;
        state.devices_enabled = false;
        state.has_snapshot = false;
        return result;

      case session_event_e::CONFIG_CHANGED:
        if (state.devices_enabled && !config_enabled) {
          result.action = toggle_action_e::disable;
          result.should_restore = state.has_snapshot;
          state.devices_enabled = false;
          state.has_snapshot = false;
        }
        return result;
    }

    return result;
  }
}  // namespace audio::steam_toggle