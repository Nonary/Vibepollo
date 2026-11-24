// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file src/dualsense_mic_audio.cpp
 * @brief Client -> Host DualSense microphone audio decoder and stereo PCM jitter buffer.
 */
#include "src/dualsense_mic_audio.h"

#include <opus/opus.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace dualsense_mic_audio {
  namespace {
    std::uint16_t read16(const std::uint8_t *p) {
      return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
    }

    std::uint32_t read32(const std::uint8_t *p) {
      return static_cast<std::uint32_t>(read16(p)) | (static_cast<std::uint32_t>(read16(p + 2)) << 16);
    }

    constexpr std::size_t ring_capacity_bytes = max_buffered_frames * bytes_per_usb_frame;
    constexpr std::size_t prebuffer_bytes = prebuffer_frames * bytes_per_usb_frame;
  }  // namespace

  std::optional<parsed_packet> parse_packet(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < header_size) {
      return std::nullopt;
    }
    if (bytes[0] != version || bytes[1] != codec_opus_48k_mono) {
      return std::nullopt;
    }
    const auto controller = read16(bytes.data() + 2);
    const auto sequence = read32(bytes.data() + 4);
    const auto sample_count = read16(bytes.data() + 8);
    const auto payload_bytes = read16(bytes.data() + 10);

    if (controller >= 16 || sample_count == 0 || sample_count > max_samples) {
      return std::nullopt;
    }
    if (payload_bytes == 0 || payload_bytes > max_payload) {
      return std::nullopt;
    }
    if (bytes.size() != header_size + payload_bytes) {
      return std::nullopt;
    }

    return parsed_packet {
      controller,
      sequence,
      sample_count,
      bytes.subspan(header_size, payload_bytes),
    };
  }

  slot_buffer::slot_buffer():
      ring_(ring_capacity_bytes, 0) {
  }

  slot_buffer::~slot_buffer() {
    if (decoder_) {
      opus_decoder_destroy(decoder_);
      decoder_ = nullptr;
    }
  }

  bool slot_buffer::ensure_decoder_locked() {
    if (decoder_) {
      return true;
    }
    int err = OPUS_OK;
    decoder_ = opus_decoder_create(sample_rate, 1, &err);
    if (err != OPUS_OK || !decoder_) {
      decoder_ = nullptr;
      return false;
    }
    return true;
  }

  void slot_buffer::clear_ring_locked() {
    read_pos_ = 0;
    write_pos_ = 0;
    count_bytes_ = 0;
    primed_ = false;
  }

  void slot_buffer::append_mono_as_stereo_locked(std::span<const std::int16_t> mono) {
    if (mono.empty()) {
      return;
    }
    const std::size_t incoming_bytes = mono.size() * bytes_per_usb_frame;
    if (incoming_bytes >= ring_capacity_bytes) {
      // Keep only the newest ring_capacity_bytes worth of samples.
      const std::size_t keep_samples = ring_capacity_bytes / bytes_per_usb_frame;
      mono = mono.subspan(mono.size() - keep_samples);
      clear_ring_locked();
    } else if (count_bytes_ + incoming_bytes > ring_capacity_bytes) {
      const std::size_t overflow = (count_bytes_ + incoming_bytes) - ring_capacity_bytes;
      read_pos_ = (read_pos_ + overflow) % ring_capacity_bytes;
      count_bytes_ -= overflow;
    }

    for (std::int16_t sample : mono) {
      const std::uint8_t lo = static_cast<std::uint8_t>(static_cast<std::uint16_t>(sample) & 0xFFu);
      const std::uint8_t hi = static_cast<std::uint8_t>((static_cast<std::uint16_t>(sample) >> 8) & 0xFFu);
      // Stereo L=R little-endian S16LE
      ring_[write_pos_] = lo;
      write_pos_ = (write_pos_ + 1) % ring_capacity_bytes;
      ring_[write_pos_] = hi;
      write_pos_ = (write_pos_ + 1) % ring_capacity_bytes;
      ring_[write_pos_] = lo;
      write_pos_ = (write_pos_ + 1) % ring_capacity_bytes;
      ring_[write_pos_] = hi;
      write_pos_ = (write_pos_ + 1) % ring_capacity_bytes;
    }
    count_bytes_ += mono.size() * bytes_per_usb_frame;
    if (!primed_ && count_bytes_ >= prebuffer_bytes) {
      primed_ = true;
    }
  }

  std::size_t slot_buffer::push_opus(
    std::uint32_t sequence,
    std::uint16_t sample_count,
    std::span<const std::uint8_t> opus_payload,
    std::chrono::steady_clock::time_point now
  ) {
    if (opus_payload.empty() || opus_payload.size() > max_payload || sample_count == 0 || sample_count > max_samples) {
      return 0;
    }

    std::lock_guard lock {mutex_};
    if (!ensure_decoder_locked()) {
      return 0;
    }

    std::array<std::int16_t, max_samples> mono {};
    std::size_t total_samples = 0;

    if (has_sequence_) {
      const auto delta = static_cast<std::int32_t>(sequence - expected_sequence_);
      if (delta < 0 && delta > -32) {
        // Late or duplicate out-of-order packet: drop.
        return 0;
      }
      if (delta > 0 && delta <= 2) {
        // Short gap: synthesize packet loss concealment frames.
        const int plc_samples = std::clamp<int>(last_frame_samples_, 120, max_samples);
        for (std::int32_t i = 0; i < delta; ++i) {
          const int decoded_plc = opus_decode(decoder_, nullptr, 0, mono.data(), plc_samples, 0);
          if (decoded_plc > 0) {
            append_mono_as_stereo_locked({mono.data(), static_cast<std::size_t>(decoded_plc)});
            total_samples += static_cast<std::size_t>(decoded_plc);
          }
        }
      } else if (delta != 0) {
        // Large jump / stream restart: reset decoder and buffer.
        opus_decoder_ctl(decoder_, OPUS_RESET_STATE);
        clear_ring_locked();
      }
    }

    const int decoded = opus_decode(
      decoder_,
      opus_payload.data(),
      static_cast<opus_int32>(opus_payload.size()),
      mono.data(),
      static_cast<int>(max_samples),
      0
    );
    if (decoded <= 0) {
      return total_samples;
    }

    append_mono_as_stereo_locked({mono.data(), static_cast<std::size_t>(decoded)});
    total_samples += static_cast<std::size_t>(decoded);
    last_frame_samples_ = static_cast<std::uint16_t>(decoded);
    expected_sequence_ = sequence + 1;
    has_sequence_ = true;
    last_push_ = now;
    return total_samples;
  }

  std::size_t slot_buffer::read_stereo_pcm(
    std::span<std::uint8_t> out,
    std::chrono::steady_clock::time_point now
  ) {
    if (out.empty()) {
      return 0;
    }
    std::fill(out.begin(), out.end(), std::uint8_t {0});

    std::lock_guard lock {mutex_};
    if (last_push_ != std::chrono::steady_clock::time_point {} && now > last_push_ && (now - last_push_) > idle_timeout) {
      clear_ring_locked();
      return 0;
    }
    if (!primed_ || count_bytes_ == 0) {
      return 0;
    }

    const std::size_t to_copy = std::min(out.size(), count_bytes_);
    const std::size_t first_chunk = std::min(to_copy, ring_capacity_bytes - read_pos_);
    std::memcpy(out.data(), ring_.data() + read_pos_, first_chunk);
    if (to_copy > first_chunk) {
      std::memcpy(out.data() + first_chunk, ring_.data(), to_copy - first_chunk);
    }
    read_pos_ = (read_pos_ + to_copy) % ring_capacity_bytes;
    count_bytes_ -= to_copy;

    if (to_copy < out.size()) {
      // Underrun: re-prime on incoming packets instead of buzzing on 1 ms fragments.
      primed_ = false;
    }
    return to_copy;
  }

  void slot_buffer::reset() {
    std::lock_guard lock {mutex_};
    clear_ring_locked();
    has_sequence_ = false;
    expected_sequence_ = 0;
    last_frame_samples_ = 960;
    last_push_ = {};
    if (decoder_) {
      opus_decoder_ctl(decoder_, OPUS_RESET_STATE);
    }
  }

  std::size_t slot_buffer::buffered_frames() const {
    std::lock_guard lock {mutex_};
    return count_bytes_ / bytes_per_usb_frame;
  }
}  // namespace dualsense_mic_audio
