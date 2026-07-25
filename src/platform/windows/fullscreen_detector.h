/**
 * @file src/platform/windows/fullscreen_detector.h
 * @brief Ordered, non-invasive Windows fullscreen detection middleware.
 */
#pragma once

#include "foreground_app.h"

#include <cstdint>

namespace platf::fullscreen_detector {

  enum class verdict_e : std::uint8_t {
    unknown = 0,
    desktop,
    fullscreen,
  };

  enum class source_e : std::uint8_t {
    none = 0,
    tracked_window,
    shell_hook,
    notification_state,
    borderless_window,
    desktop_window,
  };

  struct result_t {
    verdict_e verdict {verdict_e::unknown};
    source_e source {source_e::none};
    DWORD pid {0};
  };

  /**
   * Try increasingly broad fullscreen providers:
   *  1. A full-monitor window attributed to the launched/Playnite game.
   *  2. The Windows Shell's non-invasive "rude app" activation feed.
   *  3. The Shell's exact exclusive-D3D notification state.
   *  4. A generic borderless window covering the capture monitor.
   *
   * A borderless window is fullscreen for this policy. Providers that cannot
   * distinguish a game from notification/presentation policy return unknown
   * rather than issuing a false desktop or game verdict.
   */
  result_t detect(const foreground_app::state_t &foreground, const RECT &capture_rect);

  const char *source_name(source_e source);
  const char *verdict_name(verdict_e verdict);

}  // namespace platf::fullscreen_detector
