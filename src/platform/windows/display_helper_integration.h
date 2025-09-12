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

  // Request the helper to export current OS settings as golden restore snapshot.
  bool export_golden_restore();

  // Request the helper to reset its persistence/state.
  bool reset_persistence();

  // Enumerate display devices and return JSON payload for API.
  std::string enumerate_devices_json();

  // Start a lightweight watchdog during active streams that pings the helper periodically
  // and restarts/re-handshakes if it crashes. No-ops if already running.
  void start_watchdog();

  // Stop the helper watchdog when no streams are active.
  void stop_watchdog();
}  // namespace display_helper_integration
