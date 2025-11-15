/**
 * @file src/platform/windows/display_helper_integration.h
 * @brief High-level wrappers to use the display helper from Sunshine start/stop events.
 */
#pragma once

#include "src/config.h"
#include "src/display_helper_builder.h"
#include "src/rtsp.h"

#include <display_device/types.h>
#include <optional>
#include <vector>

namespace display_helper_integration {
  // Launch the helper (if needed) and process the provided builder request.
  // Returns true if the helper accepted the command; false to allow fallback.
  bool apply(const DisplayApplyRequest &request);

  // Launch the helper (if needed) and send REVERT.
  // Returns true if the helper accepted the command; false to allow fallback.
  bool revert();

  // Request the helper to export current OS settings as golden restore snapshot.
  bool export_golden_restore();

  // Request the helper to reset its persistence/state.
  bool reset_persistence();

  // Enumerate display devices via helper (or return nullopt on failure).
  std::optional<display_device::EnumeratedDeviceList> enumerate_devices();

  // Enumerate display devices and return JSON payload for API.
  std::string enumerate_devices_json();

  // Capture the currently active topology before applying changes.
  std::optional<std::vector<std::vector<std::string>>> capture_current_topology();

  // Start a lightweight watchdog during active streams that pings the helper periodically
  // and restarts/re-handshakes if it crashes. No-ops if already running.
  void start_watchdog();

  // Stop the helper watchdog when no streams are active.
  void stop_watchdog();

}  // namespace display_helper_integration
