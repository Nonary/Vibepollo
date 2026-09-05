/**
 * @file src/platform/linux/gamescope_session.h
 * @brief Safe discovery of the Gamescope Wayland socket for the current user.
 */
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace platf::gamescope_session {
  struct environment_t {
    std::optional<std::string> wayland_display;
    std::optional<std::string> x11_display;
  };

  /** Parse the bounded contents of SteamOS' gamescope-environment file. */
  std::optional<environment_t> parse_environment(std::string_view contents);

  /** Validate a Wayland socket basename before passing it to libwayland. */
  bool valid_wayland_display(std::string_view display_name);

  /** Import the matching session's local X11 display after Gamescope was verified. */
  bool import_x11_display();

  /**
   * Find the Gamescope socket from GAMESCOPE_WAYLAND_DISPLAY, or from the
   * current user's $XDG_RUNTIME_DIR/gamescope-environment file.
   */
  std::optional<std::string> discover_wayland_display();
}  // namespace platf::gamescope_session
