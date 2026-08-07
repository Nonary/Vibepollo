/**
 * @file src/platform/windows/game_activity_policy.cpp
 */

#include "game_activity_policy.h"

namespace platf::game_activity_policy {
  visible_stack_decision_e evaluate_visible_window(
      const visible_window_evidence_t &evidence,
      const bool require_active_app_match
    ) {
      if (evidence.transient_shell_overlay || evidence.passive_host) {
        return visible_stack_decision_e::continue_scan;
      }
      if (evidence.desktop_ui) {
        return visible_stack_decision_e::block;
      }
      if (require_active_app_match) {
        if (evidence.belongs_to_active_app) {
          return evidence.fullscreen_on_capture_display && evidence.opaque ?
                   visible_stack_decision_e::select_game :
                   visible_stack_decision_e::continue_scan;
        }
        return evidence.opaque ?
                 visible_stack_decision_e::block :
                 visible_stack_decision_e::continue_scan;
      }
      if (!evidence.opaque) {
        return visible_stack_decision_e::continue_scan;
      }
      return evidence.fullscreen_on_capture_display ?
               visible_stack_decision_e::select_game :
               visible_stack_decision_e::block;
    }

  state_t reduce_signals(const std::span<const signal_t> signals) {
    state_t result;
    for (const auto &signal : signals) {
      if (!signal.active || signal.source <= result.source) {
        continue;
      }
      result.active = true;
      result.source = signal.source;
      result.pid = signal.pid;
      result.executable = signal.executable;
    }
    return result;
  }

  bool passive_compositor_style(const std::uintptr_t style, const std::uintptr_t ex_style) {
    constexpr std::uintptr_t caption = 0x00C00000L;
    constexpr std::uintptr_t thick_frame = 0x00040000L;
    constexpr std::uintptr_t no_activate = 0x08000000L;
    constexpr std::uintptr_t transparent = 0x00000020L;
    constexpr std::uintptr_t layered = 0x00080000L;
    constexpr std::uintptr_t tool_window = 0x00000080L;

    if ((style & (caption | thick_frame)) != 0) {
      return false;
    }
    if ((ex_style & (no_activate | transparent)) != 0) {
      return true;
    }
    return (ex_style & (layered | tool_window)) == (layered | tool_window);
  }

  bool visible_fullscreen_game_selected(
    const std::span<const visible_window_evidence_t> evidence,
    const bool require_active_app_match
  ) {
    for (const auto &window : evidence) {
      switch (evaluate_visible_window(window, require_active_app_match)) {
        case visible_stack_decision_e::continue_scan:
          continue;
        case visible_stack_decision_e::select_game:
          return true;
        case visible_stack_decision_e::block:
          return false;
      }
    }
    return false;
  }

  bool preserve_confirmed_game_during_display_transition(
    const foreground_sample_t &sample,
    const foreground_sample_t &last_confirmed,
    const bool transition_settling,
    const bool minimum_hold_active
  ) {
    if (!last_confirmed.fullscreen_on_capture_display) {
      return false;
    }
    if (minimum_hold_active &&
        (sample.source == "desktop-visible" || sample.source == "visibility-unknown")) {
      return true;
    }
    if (!transition_settling) {
      return false;
    }
    if (sample.source == "visibility-unknown") {
      return true;
    }
    return sample.source == "desktop-visible" && sample.matching_window_seen;
  }
}  // namespace platf::game_activity_policy
