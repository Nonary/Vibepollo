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
    overlay_preserved,
    desktop_window,
  };

  struct result_t {
    verdict_e verdict {verdict_e::unknown};
    source_e source {source_e::none};
    DWORD pid {0};
  };

  /**
   * Reconcile fullscreen evidence as an ordered positive-detection chain:
   *  1. A full-monitor window attributed to the launched/Playnite game.
   *  2. The Windows Shell's display-scoped "rude app" activation feed.
   *  3. The Shell's session-wide exclusive-D3D notification state.
   *  4. Generic opaque borderless geometry covering the capture monitor.
   *
   * A positive result short-circuits. A negative or unavailable provider falls
   * through, and desktop evidence is returned only after all four providers
   * decline. A missing interactive session remains a hard precondition because
   * it precludes visible game content. Borderless is fullscreen for this policy.
   */
  result_t detect(const foreground_app::state_t &foreground, const RECT &capture_rect);

  const char *source_name(source_e source);
  const char *verdict_name(verdict_e verdict);

}  // namespace platf::fullscreen_detector
