/**
 * @file src/platform/windows/foreground_app.h
 * @brief Foreground window identity helpers for stream-scoped app matching.
 */
#pragma once

#include "game_activity_policy.h"

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
    bool tracks_active_app_window {false};
    DWORD foreground_pid {0};
    DWORD blocker_pid {0};
    RECT blocker_rect {};
    double blocker_coverage_percent {0.0};
    bool matching_window_seen {false};
    bool matching_game_fullscreen {false};
    std::uint32_t ignored_passive_window_count {0};
    std::string foreground_exe;
    std::string blocker_exe;
    std::string blocker_class;
    std::string blocker_title;
    std::string blocker_reason;
    std::string blocker_classification;
    bool blocker_opaque {false};
    bool blocker_framed {false};
    bool blocker_passive_overlay {false};
    bool blocker_desktop_ui {false};
    bool blocker_present {false};
    bool definite_desktop_blocker_present {false};
    DWORD definite_desktop_blocker_pid {0};
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
  using visible_window_evidence_t = game_activity_policy::visible_window_evidence_t;

  bool transient_shell_overlay_for_tests(
    std::string_view class_name,
    bool desktop_ui,
    bool covers_capture_display
  );

}  // namespace platf::foreground_app
