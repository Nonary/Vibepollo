/**
 * @file src/amf/amf_config_policy.h
 * @brief Driver-independent policy for native AMF configuration.
 */
#pragma once

#include <optional>

namespace amf::config_policy {

  inline bool valid_av1_tiles_override(int value) {
    return value == 0 || value == 1 || value == 2 || value == 4;
  }

  inline std::optional<int> av1_tiles_request(int host_override, int client_slices) {
    if (valid_av1_tiles_override(host_override) && host_override != 0) {
      return host_override;
    }
    // Preserve existing negotiation in auto mode, including client requests
    // larger than the host's deliberately small set of experimental overrides.
    // An explicit 1 is different from auto: it must overwrite the preset value.
    if (client_slices > 1) {
      return client_slices;
    }
    return std::nullopt;
  }

  struct slice_request_t {
    bool requested = false;  ///< A count above one was sent to the driver.
    bool applied = false;  ///< The driver reports exactly the client's count.
    long long driver_count = 0;  ///< The count the driver reports; 0 if it could not be read.
  };

  /**
   * Ask the driver for the client's H.264/HEVC slices per frame. The count is a
   * request, not a requirement: Moonlight for Nintendo Switch asks for four, and
   * a driver that refuses or rounds that count still encodes a valid stream with
   * its own. The caller logs a refusal and keeps streaming.
   *
   * `set(count)` and `get(count)` return whether the driver call succeeded.
   */
  template<class Set, class Get>
  slice_request_t request_client_slices(int client_slices, Set &&set, Get &&get) {
    slice_request_t result;
    if (client_slices <= 1) {
      return result;
    }
    result.requested = true;
    const bool set_ok = set(static_cast<long long>(client_slices));
    long long reported = 0;
    const bool get_ok = get(reported);
    result.driver_count = get_ok ? reported : 0;
    result.applied = set_ok && get_ok && reported == client_slices;
    return result;
  }

}  // namespace amf::config_policy
