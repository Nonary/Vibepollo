/**
 * @file src/platform/macos/av_audio_policy.h
 * @brief CoreAudio-independent buffer and converter policy.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace platf::av_audio_policy {
  std::size_t ring_buffer_bytes(std::uint32_t channels);

  struct converter_input_t {
    float *data = nullptr;
    std::uint32_t total_frames = 0;
    std::uint32_t provided_frames = 0;
    std::uint32_t channels = 0;
  };

  struct converter_chunk_t {
    float *data = nullptr;
    std::uint32_t frames = 0;
    std::uint32_t channels = 0;
    std::size_t bytes() const;
  };

  converter_chunk_t next_converter_chunk(converter_input_t &input, std::uint32_t requested_frames);

  class buffer_t {
  public:
    void initialize(std::uint32_t channels);
    void cleanup();
    bool initialized() const;
    std::size_t capacity() const;
    std::size_t available() const;
    void append(std::span<const float> samples);
    const std::vector<float> &samples() const;

  private:
    bool _initialized = false;
    std::size_t _capacity_bytes = 0;
    std::vector<float> _samples;
  };
}  // namespace platf::av_audio_policy
