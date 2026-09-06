#include "rtsp_pending_policy.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>

namespace rtsp_stream::pending_policy {
  int select_encoding_framerate(const normalized_framerate_t requested, const int launch_fps, const bool limit_framerate) {
    const std::int64_t launch_millifps = launch_fps > 0 && launch_fps < 1000 ?
                                         static_cast<std::int64_t>(launch_fps) * 1000 : launch_fps;
    if (limit_framerate && launch_millifps > 0 && launch_millifps <= MAX_CAPTURE_FRAMERATE * 1000LL) {
      return static_cast<int>(launch_millifps);
    }
    return requested.encoding_framerate;
  }

  negotiated_bitrate_t negotiate_bitrate(
    const std::int64_t configured_kbps, const int announced_kbps, const int host_ceiling_kbps,
    const normalized_framerate_t framerate, const bool limit_framerate,
    const int fec_percentage, const int audio_channels, const bool high_quality_audio
  ) {
    // Encoder rate-control fields convert Kbps to bits in signed 32-bit integers.
    constexpr std::int64_t maximum_encoder_kbps = std::numeric_limits<int>::max() / 1000;
    const auto requested = std::clamp<std::int64_t>(
      configured_kbps > 0 ? configured_kbps : announced_kbps, 1, std::numeric_limits<int>::max()
    );
    auto budget = requested;
    if (configured_kbps > 0 && limit_framerate && framerate.encoding_framerate > 0) {
      const auto warp_factor = std::llround(static_cast<double>(framerate.capture_framerate) * 1000 / framerate.encoding_framerate);
      if (warp_factor >= 2) {
        // Bound multiplication before applying the host ceiling, including for
        // malformed or extreme rate requests. Warp must never defeat that ceiling.
        budget = warp_factor > maximum_encoder_kbps / budget ? maximum_encoder_kbps : budget * warp_factor;
      }
    }
    const auto ceiling = host_ceiling_kbps > 0 ?
                           std::min<std::int64_t>(host_ceiling_kbps, maximum_encoder_kbps) : maximum_encoder_kbps;
    budget = std::min(budget, ceiling);
    if (configured_kbps > 0) {
      if (fec_percentage >= 0 && fec_percentage <= 80) {
        budget /= 100.f / (100 - fec_percentage);
      }
      const auto audio_kbps = static_cast<std::int64_t>(high_quality_audio ? 256 : 96) * std::max(0, audio_channels);
      budget -= std::min(audio_kbps, budget / 5);
      budget -= std::min<std::int64_t>(500, budget / 10);
    }
    // Legacy clients without a configured wire budget keep their announced
    // encoder rate (subject to the ceiling), without a new overhead deduction.
    return {static_cast<int>(requested), static_cast<int>(std::max<std::int64_t>(1, budget))};
  }

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
