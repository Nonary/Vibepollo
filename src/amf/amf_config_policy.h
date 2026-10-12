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

}  // namespace amf::config_policy
