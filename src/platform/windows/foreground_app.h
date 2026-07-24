/**
 * @file src/platform/windows/foreground_app.h
 * @brief Foreground window identity helpers for stream-scoped app matching.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include <winsock2.h>
#include <windows.h>

namespace platf::foreground_app {

  struct state_t {
    bool valid_window {false};
    bool shell_window {false};
    bool fullscreen_on_capture_display {false};
    bool has_active_app {false};
    bool matches_active_app {false};
    bool uses_playnite {false};
    DWORD foreground_pid {0};
    DWORD blocker_pid {0};
    RECT blocker_rect {};
    bool matching_window_seen {false};
    std::uint32_t ignored_passive_window_count {0};
    std::string foreground_exe;
    std::string blocker_exe;
    std::string blocker_class;
    std::string blocker_title;
    std::string blocker_reason;
    std::string active_app_name;
    std::string active_app_exe;
    std::string source;
  };

  state_t snapshot(
    const std::optional<RECT> &capture_rect = std::nullopt,
    DWORD game_hint_pid = 0,
    std::string_view game_hint_exe = {}
  );

  bool path_equal_or_basename_match(std::string_view lhs, std::string_view rhs);
  bool path_is_under_directory(std::string_view path, std::string_view directory);
  bool playnite_foreground_matches_for_tests(
    std::string_view active_playnite_id,
    std::string_view status_id,
    std::string_view status_exe,
    std::string_view status_install_dir,
    std::string_view foreground_exe
  );
  bool passive_compositor_style_for_tests(
    std::uintptr_t style,
    std::uintptr_t ex_style
  );

  struct visible_window_evidence_t {
    bool belongs_to_active_app {false};
    bool desktop_ui {false};
    bool passive_host {false};
    bool fullscreen_on_capture_display {false};
  };

  // Evidence is ordered from topmost to bottommost in the composed window stack.
  // Returns true only when fullscreen game content is reached before any visible
  // desktop or unrelated application surface.
  bool visible_fullscreen_game_selected_for_tests(
    std::span<const visible_window_evidence_t> evidence,
    bool require_active_app_match
  );

}  // namespace platf::foreground_app
