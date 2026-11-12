/**
 * @file src/audio_policy.h
 * @brief Platform-neutral audio stream and capture decisions.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace audio::policy {
  struct stream_layout_t {
    int channels;
    int streams;
    int coupled_streams;
    std::array<unsigned char, 8> mapping;
  };

  int stream_index(int channels, bool high_quality);
  stream_layout_t apply_custom_layout(stream_layout_t base, const std::optional<stream_layout_t> &custom);

  /**
   * @brief Convert one packet's captured float samples into a PCM audio packet payload.
   * Each sample is scaled by 32768, rounded to nearest and clamped to the int16 range (no dither),
   * then written as little-endian int16, keeping the samples' interleaved order.
   */
  std::vector<std::uint8_t> pcm_payload(std::span<const float> samples);

  /**
   * @brief The largest audio datagram, headers and encryption padding included, that every
   * Moonlight-protocol client receives intact: upstream moonlight-common-c's audio receive limit.
   */
  constexpr std::size_t DATAGRAM_BUDGET = 1400;

  /** @brief The shortest and longest packet durations, in whole milliseconds, PCM audio may use. */
  constexpr int PCM_MIN_PACKET_DURATION = 1;
  constexpr int PCM_MAX_PACKET_DURATION = 5;

  /** @brief On-the-wire sizes of one PCM packet's data datagram and of the parity datagrams sent with it. */
  struct pcm_datagrams_t {
    std::size_t data;
    std::size_t parity;
  };

  /**
   * @brief Size the datagrams carrying one PCM packet of the given packet duration.
   * Data datagrams carry the RTP header; parity datagrams also carry the audio FEC header.
   * With audio encryption, the payload grows by its AES-CBC PKCS#7 padding.
   */
  pcm_datagrams_t pcm_datagrams(int channels, int packet_duration_ms, bool encrypted);

  /** @brief Whether every data and parity datagram of PCM audio at this packet duration fits the datagram budget. */
  bool pcm_packet_duration_fits(int channels, int packet_duration_ms, bool encrypted);

  /** @brief The largest whole-millisecond packet duration from 1 to 5 that fits the datagram budget, or 0 if none does. */
  int pcm_max_packet_duration(int channels, bool encrypted);

  struct sink_catalog_t {
    std::string host;
    std::optional<std::string> stereo;
    std::optional<std::string> surround51;
    std::optional<std::string> surround71;
  };

  std::string select_sink(const sink_catalog_t &catalog,
                          const std::string &configured_sink,
                          int channels,
                          bool host_audio_enabled);
  std::string select_stream_sink(const sink_catalog_t &catalog,
                                 const std::string &configured_sink,
                                 const std::string &configured_virtual_sink,
                                 int channels,
                                 bool host_audio_enabled);

  bool capture_sink_without_routing(bool enabled, const std::string &configured_sink, const std::string &configured_virtual_sink, const std::string &selected_sink);

  enum class sample_status_e {
    ok,
    timeout,
    reinitialize,
    interrupted,
    error,
  };

  enum class sample_action_e {
    emit,
    retry,
    reacquire,
    stop,
  };

  sample_action_e sample_action(sample_status_e status);

  class sample_source_t {
  public:
    virtual ~sample_source_t() = default;
    virtual sample_status_e sample() = 0;
    virtual bool reacquire() = 0;
  };

  struct capture_summary_t {
    std::size_t emitted = 0;
    std::size_t timeouts = 0;
    std::size_t reacquisitions = 0;
    bool stopped = false;
  };

  capture_summary_t drive_capture(sample_source_t &source, std::size_t event_limit);
}  // namespace audio::policy
