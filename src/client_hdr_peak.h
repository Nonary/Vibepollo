/**
 * @file src/client_hdr_peak.h
 * @brief Bounded selection of launch-time HDR peak luminance.
 */
#pragma once

#include "hdr_request_policy.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace client_hdr_peak {
  inline constexpr std::uint32_t minimum_reported_nits = 1;
  inline constexpr std::uint32_t maximum_reported_nits = 100000;
  inline constexpr std::uint32_t minimum_host_nits = 400;
  inline constexpr std::uint32_t maximum_host_nits = 2000;

  enum class source_e {
    host_profile,
    calibrated,
    display_reported,
  };

  struct selection_t {
    bool explicit_peak_override {};
    std::optional<std::uint32_t> host_profile_peak;
    bool client_hdr_requested {};
    bool effective_hdr_requested {};
    std::string_view calibrated_value;
    std::string_view display_reported_value;
  };

  struct result_t {
    std::uint32_t peak_nits {};
    source_e source {source_e::display_reported};
  };

  std::optional<std::uint32_t> parse_nits(std::string_view value);
  // A missing result leaves the explicit override or configured fallback intact.
  std::optional<result_t> resolve(const selection_t &selection);

  // Adapt the merged, not-yet-applied runtime layer to the existing HDR policy.
  rtsp_stream::hdr_request_policy::state_t effective_request(
    bool client_hdr_requested,
    bool client_prefer_sdr_10bit,
    std::optional<bool> app_prefer_sdr_10bit,
    config::video_t::dd_t::hdr_request_override_e base_override,
    std::optional<std::string_view> requested_override = std::nullopt
  );
}  // namespace client_hdr_peak
