#include "rtsp_pending_policy.h"

namespace rtsp_stream::pending_policy {
  initial_route_e choose_initial_route(const bool plaintext_available, const bool encrypted_available, const std::array<std::uint8_t, 4> &first_word) {
    if (encrypted_available && (first_word[0] & 0x80U) != 0) return initial_route_e::encrypted;
    if (plaintext_available) return initial_route_e::plaintext;
    return initial_route_e::reject;
  }

  std::vector<pending_owner_t> expired_remote_input_owners(const std::vector<pending_owner_t> &expired) {
    std::vector<pending_owner_t> result;
    for (const auto &owner : expired) if (owner.role == remote_session::role_e::input) result.push_back(owner);
    return result;
  }

  std::vector<pending_owner_t> disconnect_input_owners_to_forget(const std::vector<pending_owner_t> &, const std::optional<pending_owner_t> current_owner) {
    // Match the old DisconnectClient behaviour: its post-removal owner lookup
    // can select a generation that was admitted after RTSP removed pending
    // sessions. The red ordering test characterizes that race.
    if (current_owner && current_owner->role == remote_session::role_e::input) return {*current_owner};
    return {};
  }
}  // namespace rtsp_stream::pending_policy
