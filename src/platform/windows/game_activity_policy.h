/**
 * @file src/platform/windows/game_activity_policy.h
 * @brief Value-only decisions shared by the Windows game-activity adapters.
 */
#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace platf::game_activity_policy {
  enum class signal_source_e : std::uint8_t {
    none = 0,
    shell_fullscreen = 5,
    fullscreen_foreground = 10,
    tracked_process = 20,
    playnite = 30,
  };

  struct signal_t {
    signal_source_e source {signal_source_e::none};
    bool active {false};
    std::uint32_t pid {0};
    std::string executable;
  };

  struct state_t {
    bool active {false};
    signal_source_e source {signal_source_e::none};
    std::uint32_t pid {0};
    std::string executable;
  };

  struct visible_window_evidence_t {
    bool belongs_to_active_app {false};
    bool desktop_ui {false};
    bool passive_host {false};
    bool fullscreen_on_capture_display {false};
    bool opaque {true};
    bool transient_shell_overlay {false};
  };

  struct foreground_sample_t {
    bool fullscreen_on_capture_display {false};
    bool matching_window_seen {false};
    std::string source;
  };

  enum class visible_stack_decision_e {
    continue_scan,
    select_game,
    block,
  };

  [[nodiscard]] state_t reduce_signals(std::span<const signal_t> signals);
  [[nodiscard]] bool passive_compositor_style(std::uintptr_t style, std::uintptr_t ex_style);
  [[nodiscard]] visible_stack_decision_e evaluate_visible_window(
    const visible_window_evidence_t &evidence,
    bool require_active_app_match
  );
  [[nodiscard]] bool visible_fullscreen_game_selected(
    std::span<const visible_window_evidence_t> evidence,
    bool require_active_app_match
  );
  [[nodiscard]] bool preserve_confirmed_game_during_display_transition(
    const foreground_sample_t &sample,
    const foreground_sample_t &last_confirmed,
    bool transition_settling,
    bool minimum_hold_active = false
  );
}  // namespace platf::game_activity_policy
