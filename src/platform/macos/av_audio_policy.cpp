/**
 * @file src/platform/macos/av_audio_policy.cpp
 * @brief CoreAudio-independent buffer and converter policy.
 */
#include "av_audio_policy.h"

#include <algorithm>

namespace platf::av_audio_policy {
  std::size_t ring_buffer_bytes(std::uint32_t channels) {
    return static_cast<std::size_t>(6) * 240 * channels * sizeof(float);
  }

  std::size_t converter_chunk_t::bytes() const {
    return static_cast<std::size_t>(frames) * channels * sizeof(float);
  }

  converter_chunk_t next_converter_chunk(converter_input_t &input, std::uint32_t requested_frames) {
    if (!input.data || input.channels == 0 || input.provided_frames >= input.total_frames) {
      return {};
    }
    const auto frames = std::min(requested_frames, input.total_frames - input.provided_frames);
    auto *data = input.data + static_cast<std::size_t>(input.provided_frames) * input.channels;
    input.provided_frames += frames;
    return {data, frames, input.channels};
  }

  void buffer_t::initialize(std::uint32_t channels) {
    cleanup();
    _capacity_bytes = ring_buffer_bytes(channels);
    _samples.reserve(_capacity_bytes / sizeof(float));
    _initialized = true;
  }

  void buffer_t::cleanup() {
    _samples.clear();
    _samples.shrink_to_fit();
    _capacity_bytes = 0;
    _initialized = false;
  }

  bool buffer_t::initialized() const {
    return _initialized;
  }

  std::size_t buffer_t::capacity() const {
    return _capacity_bytes;
  }

  std::size_t buffer_t::available() const {
    return _samples.size() * sizeof(float);
  }

  void buffer_t::append(std::span<const float> samples) {
    if (!_initialized) {
      return;
    }
    const auto remaining = (_capacity_bytes / sizeof(float)) - _samples.size();
    const auto count = std::min(remaining, samples.size());
    _samples.insert(_samples.end(), samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(count));
  }

  const std::vector<float> &buffer_t::samples() const {
    return _samples;
  }
}  // namespace platf::av_audio_policy
