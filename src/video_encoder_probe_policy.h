/**
 * @file src/video_encoder_probe_policy.h
 * @brief Value-level ownership rules for encoder capability probes.
 */
#pragma once

#include <optional>
#include <string>

namespace video::encoder_probe_policy {

  struct cache_key_t {
    std::string encoder_configuration;
    std::string adapter_identity;
    bool adapter_identity_resolved = false;

    bool operator==(const cache_key_t &other) const {
      return encoder_configuration == other.encoder_configuration &&
             adapter_identity == other.adapter_identity &&
             adapter_identity_resolved == other.adapter_identity_resolved;
    }
  };

  struct probe_observation_t {
    std::optional<std::string> required_adapter;
    std::optional<std::string> observed_adapter;
  };

  /**
   * A positive result belongs to the adapter returned by the initialized probe
   * display. A pending or selected adapter may reject a mismatched result, but
   * it cannot replace a missing observation or take ownership of the result.
   */
  inline std::optional<cache_key_t> own_successful_cache_key(
    const cache_key_t &requested_key,
    const probe_observation_t &observation
  ) {
    if (!observation.observed_adapter || observation.observed_adapter->empty()) {
      return std::nullopt;
    }
    if (observation.required_adapter &&
        (*observation.required_adapter != *observation.observed_adapter)) {
      return std::nullopt;
    }

    auto owned_key = requested_key;
    owned_key.adapter_identity = *observation.observed_adapter;
    owned_key.adapter_identity_resolved = true;
    return owned_key;
  }

  inline bool cache_key_matches(
    const cache_key_t &requested_key,
    const std::optional<cache_key_t> &cached_key
  ) {
    return requested_key.adapter_identity_resolved &&
           cached_key && cached_key->adapter_identity_resolved &&
           requested_key == *cached_key;
  }

}  // namespace video::encoder_probe_policy
