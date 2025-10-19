/**
 * @file src/platform/windows/ipc/display_settings_client.h
 * @brief Client helper to send display apply/revert commands to the helper process.
 */
#pragma once

#ifdef _WIN32

  #include <string>

namespace platf::display_helper_client {
  // Send APPLY with JSON payload (SingleDisplayConfiguration)
  bool send_apply_json(const std::string &json);

  // Send REVERT (no payload)
  bool send_revert();

  // Export current OS display settings as a golden restore snapshot
  bool send_export_golden();

  // Blacklist a display device_id from topology exports (prevents virtual display from being saved in topology)
  bool send_blacklist(const std::string &device_id);
  
  // Reset helper-side persistence/state (best-effort)
  bool send_reset();

  // Lightweight liveness probe; returns true if a Ping frame was sent.
  // This does not wait for a reply; it only validates a healthy send path.
  bool send_ping();

  // Reset the cached connection so the next send will reconnect.
  void reset_connection();
}  // namespace platf::display_helper_client

#endif