#pragma once

#include "remote_session.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rtsp_stream::pending_policy {
  constexpr int MAX_CAPTURE_FRAMERATE = 4000;

  struct normalized_framerate_t {
    int capture_framerate;
    int encoding_framerate;
    friend bool operator==(const normalized_framerate_t &, const normalized_framerate_t &) = default;
  };

  std::optional<normalized_framerate_t> normalize_requested_framerate(std::int64_t requested_framerate);
  std::optional<normalized_framerate_t> parse_requested_framerate(std::string_view requested_framerate);

  enum class initial_route_e { reject, plaintext, encrypted };

  struct pending_owner_t {
    remote_session::role_e role {remote_session::role_e::game};
    std::string client_uuid;
    std::uint64_t generation {};
  };

  // Selects an unbound transport route from the first four wire bytes.  This
  // small policy seam is used by rtsp.cpp so NAT-mixed plaintext/encrypted
  // routing has direct component coverage.
  initial_route_e choose_initial_route(bool plaintext_available, bool encrypted_available, const std::array<std::uint8_t, 4> &first_word);
  bool game_session_requires_shutdown(bool game_runtime_active, remote_session::role_e role);
  bool control_server_should_remain_alive(bool game_runtime_active, bool has_processless_live_session, bool has_game_session_pending_or_draining);
  bool disconnect_scope_matches(remote_session::role_e candidate_role, remote_session::role_e requested_role, bool client_matches, bool all_clients);
  std::vector<pending_owner_t> expired_remote_input_owners(const std::vector<pending_owner_t> &expired);
  std::vector<pending_owner_t> disconnect_input_owners_to_forget(const std::vector<pending_owner_t> &removed);

  // A pending launch's ping_timeout starts when /launch queues it, but the
  // client still needs it through ANNOUNCE, PLAY, and the control connection.
  // An ANNOUNCE startup worker can wait seconds for the lifecycle gate, so a
  // launch never expires while its startup runs, and a started session gets a
  // fresh window for the remaining handshake instead of what was left of the
  // original one.
  bool launch_entry_expired(bool startup_running, std::chrono::steady_clock::time_point expires_at, std::chrono::steady_clock::time_point now);

  struct loop_clear_t {
    bool all_sessions;
    bool preserve_pending_launches;
    friend bool operator==(const loop_clear_t &, const loop_clear_t &) = default;
  };

  // What the RTSP loop clears on each iteration. A raised broadcast_shutdown
  // means the current broadcast is ending, so every active session goes. A
  // pending launch belongs to the next broadcast: its session cannot attach
  // until end_broadcast() finishes and start_broadcast() resets the flag, so
  // it must survive the teardown instead of being canceled with it.
  loop_clear_t rtsp_loop_clear(bool broadcast_shutdown_raised);
  std::chrono::steady_clock::time_point launch_deadline_after_startup(std::chrono::steady_clock::time_point expires_at, std::chrono::steady_clock::time_point now, std::chrono::milliseconds ping_timeout);

  enum class announce_reply_e {
    ok,
    startup_failed,
    stopped_before_reply,
  };

  // The ANNOUNCE reply is posted after startup, and the session can be stopped
  // in between (e.g. the app ended during startup). A 200 for a stopped session
  // sends the client on to PLAY and a control connection that can only time
  // out, which Moonlight reports as a firewall problem, so fail it instead.
  announce_reply_e announce_reply(bool startup_failed, bool session_running);
}  // namespace rtsp_stream::pending_policy
