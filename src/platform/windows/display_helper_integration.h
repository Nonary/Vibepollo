/**
 * @file src/platform/windows/display_helper_integration.h
 * @brief High-level wrappers to use the display helper from Sunshine start/stop events.
 */
#pragma once

#include "src/config.h"
#include "src/rtsp.h"

namespace display_helper_integration {
  // Launch the helper (if needed) and send APPLY derived from (video_config, session).
  // Returns true if the helper accepted the command; false to allow fallback.
  bool apply_from_session(const config::video_t &video_config, const rtsp_stream::launch_session_t &session);

  // Launch the helper (if needed) and send REVERT.
  // Returns true if the helper accepted the command; false to allow fallback.
  bool revert();
}  // namespace display_helper_integration
