// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file src/dualsense_mic_audio.h
 * @brief Client -> Host DualSense microphone audio protocol (0x5603) and Opus jitter buffer.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

struct OpusDecoder;

namespace dualsense_mic_audio {
  // Session feature flag advertised in x-ml-general.featureFlags when the client
  // enables controller microphone streaming.
  inline constexpr std::uint32_t session_feature = 0x20u;

  // Client -> Host encrypted control stream packet type.
  inline constexpr std::uint16_t packet_type = 0x5603u;
  inline constexpr std::uint8_t version = 1;
  inline constexpr std::uint8_t codec_opus_48k_mono = 1;
  inline constexpr std::size_t header_size = 12;
  inline constexpr std::size_t max_payload = 512;
  inline constexpr std::size_t max_packet_size = header_size + max_payload;
  inline constexpr std::uint16_t max_samples = 2880;  // 60 ms at 48 kHz

  inline constexpr int sample_rate = 48000;
  inline constexpr std::size_t usb_channels = 2;
  inline constexpr std::size_t bytes_per_usb_frame = usb_channels * sizeof(std::int16_t);  // 4 bytes
  inline constexpr std::size_t prebuffer_frames = 960;      // 20 ms
  inline constexpr std::size_t max_buffered_frames = 4800;  // 100 ms
  inline constexpr auto idle_timeout = std::chrono::milliseconds(150);

  struct parsed_packet {
    std::uint16_t controller_number = 0;
    std::uint32_t sequence = 0;
    std::uint16_t sample_count = 0;
    std::span<const std::uint8_t> opus_payload;
  };

  /**
   * @brief Parse and validate a 0x5603 controller mic packet payload.
   */
  std::optional<parsed_packet> parse_packet(std::span<const std::uint8_t> bytes);

  /**
   * @brief Per-controller Opus decoder + bounded stereo S16LE ring buffer feeding USB/IP EP 0x82.
   *
   * Thread-safe: push_opus() may run on the input worker while read_stereo_pcm() runs on the
   * USB/IP transport thread. Decoded 48 kHz mono samples are duplicated to L=R stereo S16LE
   * to match the DualSense USB audio capture descriptor (48 kHz, 2 ch, 16-bit LE).
   */
  class slot_buffer {
  public:
    slot_buffer();
    ~slot_buffer();
    slot_buffer(const slot_buffer &) = delete;
    slot_buffer &operator=(const slot_buffer &) = delete;

    /**
     * @brief Decode one incoming Opus frame (with PLC on 1-2 lost sequences) and enqueue stereo PCM.
     * @return Number of decoded mono samples appended (0 on invalid input or late/duplicate packet).
     */
    std::size_t push_opus(
      std::uint32_t sequence,
      std::uint16_t sample_count,
      std::span<const std::uint8_t> opus_payload,
      std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()
    );

    /**
     * @brief Fill an outgoing USB/IP ISO IN buffer with 48 kHz stereo S16LE PCM (or silence).
     * @return Number of non-zero-padded PCM bytes copied from the jitter buffer.
     */
    std::size_t read_stereo_pcm(
      std::span<std::uint8_t> out,
      std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()
    );

    void set_muted(bool muted);
    bool is_muted() const;
    void reset();
    std::size_t buffered_frames() const;

  private:
    bool ensure_decoder_locked();
    void append_mono_as_stereo_locked(std::span<const std::int16_t> mono);
    void clear_ring_locked();
    void reset_stream_state_locked();

    mutable std::mutex mutex_;
    OpusDecoder *decoder_ = nullptr;
    std::vector<std::uint8_t> ring_;
    std::size_t read_pos_ = 0;
    std::size_t write_pos_ = 0;
    std::size_t count_bytes_ = 0;
    bool muted_ = false;
    bool has_sequence_ = false;
    std::uint32_t expected_sequence_ = 0;
    std::uint16_t last_frame_samples_ = 960;
    bool primed_ = false;
    std::chrono::steady_clock::time_point last_push_ {};
  };
}  // namespace dualsense_mic_audio
