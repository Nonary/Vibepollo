#include "rtsp_pending_policy.h"

namespace rtsp_stream::pending_policy {
  initial_route_e choose_initial_route(const bool plaintext_available, const bool encrypted_available, const std::array<std::uint8_t, 4> &first_word) {
    if (encrypted_available && (first_word[0] & 0x80U) != 0) return initial_route_e::encrypted;
    if (plaintext_available) return initial_route_e::plaintext;
    return initial_route_e::reject;
  }

  bool control_server_should_remain_alive(const bool process_running, const bool has_live_session, const bool launch_or_startup_pending) {
    return process_running || has_live_session || launch_or_startup_pending;
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
