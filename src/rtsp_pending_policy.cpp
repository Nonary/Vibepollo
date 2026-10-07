#include "rtsp_pending_policy.h"

#include <charconv>
#include <limits>

namespace rtsp_stream::pending_policy {
  std::optional<normalized_framerate_t> normalize_requested_framerate(const std::int64_t requested_framerate) {
    if (requested_framerate <= 0 || requested_framerate > std::numeric_limits<int>::max()) {
      return std::nullopt;
    }

    const auto capture_framerate = requested_framerate > 4000 ?
                                     (requested_framerate + 500) / 1000 :
                                     requested_framerate;
    if (capture_framerate <= 0 || capture_framerate > MAX_CAPTURE_FRAMERATE) {
      return std::nullopt;
    }

    const auto encoding_framerate = requested_framerate > 1000 ?
                                      requested_framerate :
                                      requested_framerate * 1000;
    return normalized_framerate_t {
      .capture_framerate = static_cast<int>(capture_framerate),
      .encoding_framerate = static_cast<int>(encoding_framerate),
    };
  }

  std::optional<normalized_framerate_t> parse_requested_framerate(const std::string_view requested_framerate) {
    std::int64_t parsed {};
    const auto [end, error] = std::from_chars(
      requested_framerate.data(),
      requested_framerate.data() + requested_framerate.size(),
      parsed
    );
    if (error != std::errc {} || end != requested_framerate.data() + requested_framerate.size()) {
      return std::nullopt;
    }
    return normalize_requested_framerate(parsed);
  }

  initial_route_e choose_initial_route(const bool plaintext_available, const bool encrypted_available, const std::array<std::uint8_t, 4> &first_word) {
    if (encrypted_available && (first_word[0] & 0x80U) != 0) return initial_route_e::encrypted;
    if (plaintext_available) return initial_route_e::plaintext;
    return initial_route_e::reject;
  }

  bool game_session_requires_shutdown(const bool game_runtime_active, const remote_session::role_e role) {
    return !game_runtime_active && role == remote_session::role_e::game;
  }

  bool control_server_should_remain_alive(
    const bool game_runtime_active,
    const bool has_processless_live_session,
    const bool has_game_session_pending_or_draining
  ) {
    return game_runtime_active || has_processless_live_session || has_game_session_pending_or_draining;
  }

  bool disconnect_scope_matches(const remote_session::role_e candidate_role, const remote_session::role_e requested_role, const bool client_matches, const bool all_clients) {
    return candidate_role == requested_role && (all_clients || client_matches);
  }

  std::vector<pending_owner_t> expired_remote_input_owners(const std::vector<pending_owner_t> &expired) {
    std::vector<pending_owner_t> result;
    for (const auto &owner : expired) if (owner.role == remote_session::role_e::input) result.push_back(owner);
    return result;
  }

  std::vector<pending_owner_t> disconnect_input_owners_to_forget(const std::vector<pending_owner_t> &removed) {
    std::vector<pending_owner_t> result;
    for (const auto &owner : removed) if (owner.role == remote_session::role_e::input) result.push_back(owner);
    return result;
  }
}  // namespace rtsp_stream::pending_policy
