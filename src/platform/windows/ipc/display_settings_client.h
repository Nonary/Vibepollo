/**
 * @file src/platform/windows/ipc/display_settings_client.h
 * @brief Client helper to send display apply/revert commands to the helper process.
 */
#pragma once

#ifdef _WIN32

  #include <cstdint>
  #include <chrono>
  #include <functional>
  #include <optional>
  #include <string>

namespace platf::display_helper_client {
  // Send APPLY with JSON payload (SingleDisplayConfiguration). Every request
  // carries a backward-compatible token: v2 echoes it for a later verification
  // acknowledgement, while legacy helpers reply in their original untagged
  // format and are detected from that response.
  bool send_apply_json(
    const std::string &json,
    std::uint64_t *request_id_out = nullptr,
    std::uint64_t *wait_generation_out = nullptr,
    std::uint64_t *connection_generation_out = nullptr,
    std::function<bool()> cancellation_predicate = {},
    int operation_timeout_ms = 0,
    // Shutdown-class callers (owned recovery/teardown workers) collapse the
    // connect and send caps to kShutdownIpcTimeoutMs. Ordinary applies always
    // carry a cancellation predicate now, so it cannot imply this by itself.
    bool shutdown_class_caller = false);

  // Wait for helper verification result after APPLY (v2 engine only).
  // Returns nullopt on timeout/unavailable.
  // The optional cancellation predicate is checked between short receive waits so a
  // superseded capture gate does not keep the response reader occupied until the
  // original verification deadline.
  std::optional<bool> wait_for_verification_result(
    int timeout_ms,
    std::function<bool()> cancellation_predicate = {},
    std::uint64_t expected_request_id = 0,
    std::uint64_t expected_wait_generation = 0,
    std::uint64_t expected_connection_generation = 0
  );

  // True only after this live pipe has acknowledged a tagged v2 APPLY. An
  // unknown/legacy connection must use dispatch-only snapshot semantics.
  bool uses_v2_response_protocol();

  // Change only one display's refresh rate. This does not alter session snapshots,
  // topology, resolution, HDR, or the helper's restore state.
  bool send_refresh_rate(const std::string &device_id, std::uint32_t numerator, std::uint32_t denominator);

  // Send REVERT with optional JSON payload.
  bool send_revert(const std::string &json_payload = {});

  // Bounded stream-start REVERT. Control ownership, connection setup, and
  // frame dispatch all share operation_deadline.
  bool send_revert_within(
    const std::string &json_payload,
    std::chrono::steady_clock::time_point operation_deadline,
    std::function<bool()> cancellation_predicate = {});

  // Update helper log level to match Sunshine's minimum log level (v2 engine only).
  bool send_log_level(int min_log_level);

  // Export current OS display settings as a golden restore snapshot
  bool send_export_golden(const std::string &json_payload = {});

  // Best-effort cancel of any pending restore/watchdog activity on the helper
  bool send_disarm_restore();

  // Fast, best-effort DISARM using only an already-connected cached pipe. It
  // never starts anonymous/named-pipe connection setup; lock acquisition and
  // send initiation use timeout_ms as their budget. Intended for stream-start
  // paths where helper activity must be stopped promptly.
  bool send_disarm_restore_fast(
    int timeout_ms,
    std::chrono::steady_clock::time_point operation_deadline =
      std::chrono::steady_clock::time_point::max());

  // Save the current OS display state to session_current (rotate current->previous) without applying config.
  bool send_snapshot_current(const std::string &json_payload = {});

  // Save the current OS display state and wait for the v2 helper's result.
  // Legacy helpers do not implement this acknowledgement.
  bool send_snapshot_current_and_wait(const std::string &json_payload = {}, int timeout_ms = 3000);

  // Bounded stream-start snapshot. Connection, write, and any v2 completion
  // wait all share operation_deadline; legacy/unknown helpers remain
  // dispatch-only.
  bool send_snapshot_current_within(
    const std::string &json_payload,
    std::chrono::steady_clock::time_point operation_deadline,
    std::function<bool()> cancellation_predicate = {});

  // Reset helper-side persistence/state (best-effort)
  bool send_reset();

  // Request helper process to terminate gracefully.
  bool send_stop();

  // Lightweight liveness probe; returns true if a Ping frame was sent.
  // This does not wait for a reply; it only validates a healthy send path.
  bool send_ping();

  // Cancellation-aware liveness probe. Both connection and send work are
  // bounded by timeout_ms and the predicate is observed between lock waits.
  bool send_ping_cancellable(
    int timeout_ms,
    std::function<bool()> cancellation_predicate,
    std::chrono::steady_clock::time_point operation_deadline =
      std::chrono::steady_clock::time_point::max());

  // Fast liveness probe using only an already-connected cached pipe.
  bool send_ping_fast(
    int timeout_ms,
    std::chrono::steady_clock::time_point operation_deadline =
      std::chrono::steady_clock::time_point::max());

  // Reset the cached connection so the next send will reconnect.
  void reset_connection();

  // Cancellation-aware variant used by bounded recovery paths.
  bool reset_connection_cancellable(
    std::function<bool()> cancellation_predicate,
    std::chrono::steady_clock::time_point operation_deadline =
      std::chrono::steady_clock::time_point::max());
}  // namespace platf::display_helper_client

#endif
