#include "rtsp_pending_policy.h"

namespace rtsp_stream::pending_policy {
  initial_route_e choose_initial_route(const bool plaintext_available, const bool encrypted_available, const std::array<std::uint8_t, 4> &) {
    // Preserve the legacy precedence until the RTSP reader consumes the
    // framing word. The focused red test documents why this is unsafe.
    if (plaintext_available) return initial_route_e::plaintext;
    return encrypted_available ? initial_route_e::encrypted : initial_route_e::reject;
  }

  std::vector<pending_owner_t> expired_remote_input_owners(const std::vector<pending_owner_t> &) {
    // Legacy pending expiry erased every owner without reporting exact input
    // transport loss. The implementation commit supplies that report.
    return {};
  }
}  // namespace rtsp_stream::pending_policy
