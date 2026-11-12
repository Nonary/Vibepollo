/**
 * @file src/audio_policy.cpp
 * @brief Platform-neutral audio stream and capture decisions.
 */
#include "audio_policy.h"

#include <algorithm>
#include <cmath>

namespace audio::policy {
  int stream_index(int channels, bool high_quality) {
    const int quality_offset = high_quality ? 1 : 0;
    switch (channels) {
      case 2:
        return quality_offset;
      case 6:
        return 2 + quality_offset;
      case 8:
        return 4 + quality_offset;
      default:
        return 0;
    }
  }

  stream_layout_t apply_custom_layout(stream_layout_t base, const std::optional<stream_layout_t> &custom) {
    return custom.value_or(base);
  }

  std::vector<std::uint8_t> pcm_payload(std::span<const float> samples) {
    std::vector<std::uint8_t> payload;
    payload.reserve(samples.size() * sizeof(std::int16_t));
    for (const auto sample : samples) {
      // Clamping before rounding gives the same result and keeps lround() in range for any input.
      const auto scaled = std::clamp(sample * 32768.0f, -32768.0f, 32767.0f);
      const auto pcm = static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(scaled)));
      payload.push_back(static_cast<std::uint8_t>(pcm & 0xFF));
      payload.push_back(static_cast<std::uint8_t>(pcm >> 8));
    }
    return payload;
  }

  namespace {
    // 48 kHz, 16-bit interleaved samples.
    constexpr std::size_t PCM_BYTES_PER_CHANNEL_MS = 48 * sizeof(std::int16_t);
    constexpr std::size_t RTP_HEADER_SIZE = 12;
    constexpr std::size_t AUDIO_FEC_HEADER_SIZE = 12;
    constexpr std::size_t AES_BLOCK_SIZE = 16;
  }  // namespace

  pcm_datagrams_t pcm_datagrams(int channels, int packet_duration_ms, bool encrypted) {
    auto payload = PCM_BYTES_PER_CHANNEL_MS * static_cast<std::size_t>(channels) * static_cast<std::size_t>(packet_duration_ms);
    if (encrypted) {
      // PKCS#7 always pads, adding a whole block when the payload is already block-aligned.
      payload = (payload / AES_BLOCK_SIZE + 1) * AES_BLOCK_SIZE;
    }
    return {RTP_HEADER_SIZE + payload, RTP_HEADER_SIZE + AUDIO_FEC_HEADER_SIZE + payload};
  }

  bool pcm_packet_duration_fits(int channels, int packet_duration_ms, bool encrypted) {
    if (channels < 1 || packet_duration_ms < PCM_MIN_PACKET_DURATION || packet_duration_ms > PCM_MAX_PACKET_DURATION) {
      return false;
    }
    const auto datagrams = pcm_datagrams(channels, packet_duration_ms, encrypted);
    return datagrams.data <= DATAGRAM_BUDGET && datagrams.parity <= DATAGRAM_BUDGET;
  }

  int pcm_max_packet_duration(int channels, bool encrypted) {
    for (int duration = PCM_MAX_PACKET_DURATION; duration >= PCM_MIN_PACKET_DURATION; --duration) {
      if (pcm_packet_duration_fits(channels, duration, encrypted)) {
        return duration;
      }
    }
    return 0;
  }

  std::string select_sink(const sink_catalog_t &catalog,
                          const std::string &configured_sink,
                          int channels,
                          bool host_audio_enabled) {
    std::string selected = configured_sink.empty() ? catalog.host : configured_sink;
    if (!host_audio_enabled || selected.empty()) {
      const std::optional<std::string> *virtual_sink = nullptr;
      switch (channels) {
        case 2:
          virtual_sink = &catalog.stereo;
          break;
        case 6:
          virtual_sink = &catalog.surround51;
          break;
        case 8:
          virtual_sink = &catalog.surround71;
          break;
      }
      if (virtual_sink && *virtual_sink) {
        selected = **virtual_sink;
      }
    }
    return selected;
  }

  std::string select_stream_sink(const sink_catalog_t &catalog,
                                 const std::string &configured_sink,
                                 const std::string &configured_virtual_sink,
                                 int channels,
                                 bool host_audio_enabled) {
    const bool managed_virtual_sink = !configured_virtual_sink.empty();
    return select_sink(
      catalog,
      managed_virtual_sink ? configured_virtual_sink : configured_sink,
      channels,
      host_audio_enabled && !managed_virtual_sink
    );
  }

  bool capture_sink_without_routing(bool enabled, const std::string &configured_sink, const std::string &configured_virtual_sink, const std::string &selected_sink) {
    return enabled && !configured_sink.empty() && configured_virtual_sink.empty() &&
           selected_sink == configured_sink;
  }

  sample_action_e sample_action(sample_status_e status) {
    switch (status) {
      case sample_status_e::ok:
        return sample_action_e::emit;
      case sample_status_e::timeout:
        return sample_action_e::retry;
      case sample_status_e::reinitialize:
        return sample_action_e::reacquire;
      case sample_status_e::interrupted:
      case sample_status_e::error:
        return sample_action_e::stop;
    }
    return sample_action_e::stop;
  }

  capture_summary_t drive_capture(sample_source_t &source, std::size_t event_limit) {
    capture_summary_t summary;
    for (std::size_t event = 0; event < event_limit; ++event) {
      switch (sample_action(source.sample())) {
        case sample_action_e::emit:
          ++summary.emitted;
          break;
        case sample_action_e::retry:
          ++summary.timeouts;
          break;
        case sample_action_e::reacquire:
          ++summary.reacquisitions;
          if (!source.reacquire()) {
            summary.stopped = true;
            return summary;
          }
          break;
        case sample_action_e::stop:
          summary.stopped = true;
          return summary;
      }
    }
    return summary;
  }
}  // namespace audio::policy
