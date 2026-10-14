/**
 * @file src/client_hdr_peak.cpp
 * @brief Bounded selection of launch-time HDR peak luminance.
 */

#include "client_hdr_peak.h"

#include <algorithm>
#include <charconv>
#include <system_error>

namespace client_hdr_peak {
  std::optional<std::uint32_t> parse_nits(const std::string_view value) {
    if (value.empty()) {
      return std::nullopt;
    }

    std::uint32_t parsed {};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc {} || end != value.data() + value.size() ||
        parsed < minimum_reported_nits || parsed > maximum_reported_nits) {
      return std::nullopt;
    }
    return parsed;
  }

  std::optional<result_t> resolve(const selection_t &selection) {
    if (selection.explicit_peak_override) {
      return std::nullopt;
    }
    const auto bounded_result = [](const std::uint32_t nits, const source_e source) {
      return result_t {std::clamp(nits, minimum_host_nits, maximum_host_nits), source};
    };
    if (selection.host_profile_peak && *selection.host_profile_peak > 0) {
      return bounded_result(*selection.host_profile_peak, source_e::host_profile);
    }
    if (!selection.client_hdr_requested || !selection.effective_hdr_requested) {
      return std::nullopt;
    }
    if (const auto calibrated = parse_nits(selection.calibrated_value)) {
      return bounded_result(*calibrated, source_e::calibrated);
    }
    if (const auto reported = parse_nits(selection.display_reported_value)) {
      return bounded_result(*reported, source_e::display_reported);
    }
    return std::nullopt;
  }

  rtsp_stream::hdr_request_policy::state_t effective_request(
    const bool client_hdr_requested,
    const bool client_prefer_sdr_10bit,
    const std::optional<bool> app_prefer_sdr_10bit,
    config::video_t::dd_t::hdr_request_override_e base_override,
    const std::optional<std::string_view> requested_override
  ) {
    if (requested_override) {
      using override_e = config::video_t::dd_t::hdr_request_override_e;
      // Match config's dd_hdr_request_override conversion, including its
      // automatic fallback for an invalid configured string.
      base_override = *requested_override == "force_on" ? override_e::force_on :
                         *requested_override == "force_off" ? override_e::force_off :
                                                              override_e::automatic;
    }
    return rtsp_stream::hdr_request_policy::apply(
      {
        client_hdr_requested,
        rtsp_stream::hdr_request_policy::resolve_prefer_10bit_sdr(client_prefer_sdr_10bit, app_prefer_sdr_10bit),
        false,
      },
      base_override
    );
  }
}  // namespace client_hdr_peak
