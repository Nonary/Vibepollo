/**
 * @file src/hdr_request_policy.h
 * @brief Platform-neutral launch-time HDR request override policy.
 */
#pragma once

#include "config.h"

namespace rtsp_stream::hdr_request_policy {

  struct state_t {
    bool enable_hdr {};
    bool prefer_sdr_10bit {};
    bool force_sdr {};

    constexpr bool operator==(const state_t &) const = default;
  };

  [[nodiscard]] constexpr state_t apply(
    state_t state,
    const config::video_t::dd_t::hdr_request_override_e override
  ) {
    using override_e = config::video_t::dd_t::hdr_request_override_e;
    switch (override) {
      case override_e::force_on:
        state.enable_hdr = true;
        state.prefer_sdr_10bit = false;
        state.force_sdr = false;
        break;
      case override_e::force_off:
        state.enable_hdr = false;
        state.force_sdr = true;
        break;
      case override_e::automatic:
        break;
    }
    return state;
  }

}  // namespace rtsp_stream::hdr_request_policy
