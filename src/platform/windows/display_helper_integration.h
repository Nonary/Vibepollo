/**
 * @file src/platform/windows/display_helper_integration.h
 * @brief High-level wrappers to use the display helper from Sunshine start/stop events.
 */
#pragma once

#include "src/config.h"
#include "src/display_helper_builder.h"
#include "src/rtsp.h"

#include <display_device/types.h>
#include <chrono>
#include <optional>
#include <vector>

namespace display_helper_integration {
  // Launch the helper (if needed) and process the provided builder request.
  // Returns true if the helper accepted the command; false to allow fallback.
  bool apply(const DisplayApplyRequest &request);

  // Retry a deferred APPLY request once a user session is available.
  bool apply_pending_if_ready();

  // Clear any deferred APPLY request (used when sessions end).
  void clear_pending_apply();

  // Launch the helper (if needed) and send REVERT.
  // Returns true if the helper accepted the command; false to allow fallback.
  bool revert();

  // Attempt to cancel any pending restore/revert requests on a running helper.
  // Returns true if a DISARM command was sent successfully.
  bool disarm_pending_restore();

  // Request the helper to export current OS settings as golden restore snapshot.
  bool export_golden_restore();

  // Request the helper to reset its persistence/state.
  bool reset_persistence();

  // Ask the helper to capture the current display snapshot without applying changes.
  bool snapshot_current_display_state();

  // Enumerate display devices via helper (or return nullopt on failure).
  std::optional<display_device::EnumeratedDeviceList> enumerate_devices(
    display_device::DeviceEnumerationDetail detail = display_device::DeviceEnumerationDetail::Minimal);

  // Enumerate display devices and return JSON payload for API.
  std::string enumerate_devices_json(
    display_device::DeviceEnumerationDetail detail = display_device::DeviceEnumerationDetail::Minimal);

  // Capture the currently active topology before applying changes.
  std::optional<std::vector<std::vector<std::string>>> capture_current_topology();

#ifdef _WIN32
  enum class ApplyVerificationStatus {
    Verified,
    Failed,
    Unknown
  };

  // Wait for helper verification to finish after APPLY. Returns Unknown on timeout/unavailable.
  ApplyVerificationStatus wait_for_apply_verification(std::chrono::milliseconds timeout);
#endif

#ifdef _WIN32
  struct FramegenEdidTargetSupport {
    int hz {0};
    std::optional<bool> supported;
    std::string method;
  };

  struct FramegenEdidSupportResult {
    std::string device_id;
    std::string device_label;
    bool edid_present {false};
    std::optional<int> max_vertical_hz;
    std::optional<double> max_timing_hz;
    std::vector<FramegenEdidTargetSupport> targets;
  };

  // Read EDID for a specific device and evaluate refresh support for requested targets.
  std::optional<FramegenEdidSupportResult> framegen_edid_refresh_support(
    const std::string &device_hint,
    const std::vector<int> &targets_hz);
#endif

  // Start a lightweight watchdog during active streams that pings the helper periodically
  // and restarts/re-handshakes if it crashes. No-ops if already running.
  void start_watchdog();

  // Stop the helper watchdog when no streams are active.
  void stop_watchdog();

}  // namespace display_helper_integration
