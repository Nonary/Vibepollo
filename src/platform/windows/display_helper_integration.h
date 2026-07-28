/**
 * @file src/platform/windows/display_helper_integration.h
 * @brief High-level wrappers to use the display helper from Sunshine start/stop events.
 */
#pragma once

#include "src/config.h"
#include "src/display_helper_builder.h"
#include "src/platform/windows/display_helper_v2/timing.h"
#include "src/rtsp.h"

#include <display_device/types.h>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace display_helper_integration {
  /// Per-APPLY identity for the v2 capture gate. It is intentionally owned by
  /// the launching session rather than looked up through mutable global state.
  struct ApplyVerificationTicket {
    std::uint64_t generation {0};
    std::uint64_t helper_request_id {0};
    std::uint64_t client_wait_generation {0};
    std::uint64_t connection_generation {0};
    bool uses_v2_helper {false};
    std::chrono::steady_clock::time_point startup_deadline {};
  };

  // Launch the helper (if needed) and process the provided builder request.
  // Returns true if the helper accepted the command; false to allow fallback.
  // A cancellation predicate is intended for recovery workers during shutdown.
  // It interrupts helper IPC waits and disables the potentially blocking
  // in-process fallback for that caller.
  bool apply(
    const DisplayApplyRequest &request,
    ApplyVerificationTicket *verification_ticket = nullptr,
    std::function<bool()> cancellation_predicate = {});

  // Returns true if a deferred APPLY request is currently queued.
  bool has_pending_apply();

  // Retry a deferred APPLY request once a user session is available. The
  // optional predicate lets an owned shutdown worker abandon a long helper
  // operation before teardown begins.
  bool apply_pending_if_ready(std::function<bool()> cancellation_predicate = {});

  // Clear any deferred APPLY request (used when sessions end).
  void clear_pending_apply();

  // Launch the helper (if needed) and send REVERT.
  // Returns true if the helper accepted the command; false to allow fallback.
  bool revert(bool prefer_golden_if_current_missing = true);

  // Attempt to cancel any pending restore/revert requests on a running helper.
  // Returns true if a DISARM command was sent successfully.
  bool disarm_pending_restore(std::function<bool()> cancellation_predicate = {});

  // Request the helper to export current OS settings as golden restore snapshot.
  bool export_golden_restore();

  // Request the helper to reset its persistence/state.
  bool reset_persistence();

  // Ask the helper to capture the current display snapshot without applying changes.
  bool snapshot_current_display_state();

  // Enumerate display devices via helper (or return nullopt on failure).
  std::optional<display_device::EnumeratedDeviceList> enumerate_devices(
    display_device::DeviceEnumerationDetail detail = display_device::DeviceEnumerationDetail::Minimal
  );

  // Enumerate display devices and return JSON payload for API.
  std::string enumerate_devices_json(
    display_device::DeviceEnumerationDetail detail = display_device::DeviceEnumerationDetail::Minimal
  );

  // Capture the currently active topology before applying changes.
  std::optional<std::vector<std::vector<std::string>>> capture_current_topology();

#ifdef _WIN32
  enum class ApplyVerificationStatus {
    Verified,
    Failed,
    Unknown
  };

  // APPLY acknowledgement and verification share this single stream-start
  // budget; verification receives only the time left after APPLY.
  inline constexpr auto kApplyVerificationTimeout = display_helper::v2::timing::kApplyStartupBudget;
  inline constexpr auto kApplyVerificationGateWaitTimeout =
    kApplyVerificationTimeout + display_helper::v2::timing::kApplyGateConsumerSlack;

  // Wait for helper verification to finish after APPLY (v2 engine only).
  // Returns Unknown on timeout, legacy engine, or when verification is unavailable.
  ApplyVerificationStatus wait_for_apply_verification(
    const ApplyVerificationTicket &ticket,
    std::chrono::milliseconds timeout);

  // True when the most recent successful APPLY is verified and has no pending
  // HDR/topology workaround that requires the settling fallback.
  bool last_apply_is_capture_stable();

  // True when the most recent APPLY asked for HDR to be enabled. Capture start
  // uses this to wait for HDR to actually come up rather than for a fixed
  // interval, so a session never begins in SDR and transitions mid-stream.
  bool last_apply_requested_hdr();
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
    const std::vector<int> &targets_hz
  );
#endif

  // Returns milliseconds since the last successful display-helper APPLY completed.
  // Returns a very large value if no apply has ever been performed.
  int64_t ms_since_last_apply();

  // Start a lightweight watchdog during active streams that pings the helper periodically
  // and restarts/re-handshakes if it crashes. No-ops if already running.
  void start_watchdog();

  // Stop the helper watchdog when no streams are active. Forced stops are
  // reserved for process shutdown or an explicit user-requested restore.
  void stop_watchdog(bool force = false);

}  // namespace display_helper_integration
