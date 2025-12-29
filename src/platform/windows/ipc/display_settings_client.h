/**
 * @file src/platform/windows/ipc/display_settings_client.h
 * @brief Client helper to send display apply/revert commands to the helper process.
 */
#pragma once

#ifdef _WIN32

  #include <cstdint>
  #include <optional>
  #include <string>

namespace platf::display_helper_client {
  // Send APPLY with JSON payload (SingleDisplayConfiguration)
  bool send_apply_json(const std::string &json);

  // Wait for helper verification result after APPLY. Returns nullopt on timeout/unavailable.
  std::optional<bool> wait_for_verification_result(int timeout_ms);

  // Send REVERT (no payload)
  bool send_revert();

  // Export current OS display settings as a golden restore snapshot
  bool send_export_golden(const std::string &json_payload = {});

  // Best-effort cancel of any pending restore/watchdog activity on the helper
  bool send_disarm_restore();

  // Fast, best-effort DISARM that will not block longer than timeout_ms for connect/send.
  // Intended for stream start paths where we must stop helper activity immediately.
  bool send_disarm_restore_fast(int timeout_ms);

  // Save the current OS display state to session_current (rotate current->previous) without applying config.
  bool send_snapshot_current(const std::string &json_payload = {});

  // Reset helper-side persistence/state (best-effort)
  bool send_reset();

  // Request helper process to terminate gracefully.
  bool send_stop();

  // Lightweight liveness probe; returns true if a Ping frame was sent.
  // This does not wait for a reply; it only validates a healthy send path.
  bool send_ping();

  // Reset the cached connection so the next send will reconnect.
  void reset_connection();
}  // namespace platf::display_helper_client

#endif
