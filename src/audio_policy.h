/**
 * @file src/audio_policy.h
 * @brief Platform-neutral audio stream and capture decisions.
 */
#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>

namespace audio::policy {
  struct stream_layout_t {
    int channels;
    int streams;
    int coupled_streams;
    std::array<unsigned char, 8> mapping;
  };

  int stream_index(int channels, bool high_quality);
  stream_layout_t apply_custom_layout(stream_layout_t base, const std::optional<stream_layout_t> &custom);

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
