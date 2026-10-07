#pragma once

#include "remote_session.h"

#include <array>
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
}  // namespace rtsp_stream::pending_policy
