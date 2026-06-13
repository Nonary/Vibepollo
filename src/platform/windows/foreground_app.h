/**
 * @file src/platform/windows/foreground_app.h
 * @brief Foreground window identity helpers for stream-scoped app matching.
 */
#pragma once

#include <optional>
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
    std::string foreground_exe;
    std::string active_app_name;
    std::string active_app_exe;
    std::string source;
  };

  state_t snapshot(const std::optional<RECT> &capture_rect = std::nullopt);

  bool path_equal_or_basename_match(std::string_view lhs, std::string_view rhs);
  bool path_is_under_directory(std::string_view path, std::string_view directory);
  bool playnite_foreground_matches_for_tests(
    std::string_view active_playnite_id,
    std::string_view status_id,
    std::string_view status_exe,
    std::string_view status_install_dir,
    std::string_view foreground_exe
  );

}  // namespace platf::foreground_app
