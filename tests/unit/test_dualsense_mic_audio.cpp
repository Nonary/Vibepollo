// SPDX-License-Identifier: GPL-3.0-or-later
#include "src/dualsense_mic_audio.h"

#include <gtest/gtest.h>
#include <opus/opus.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {
  std::vector<std::uint8_t> encode_sine_opus(OpusEncoder *enc, std::size_t frame_index, std::uint16_t samples = 960) {
    std::vector<std::int16_t> pcm(samples);
    constexpr double pi2 = 6.28318530717958647692;
    for (std::size_t i = 0; i < samples; ++i) {
      const double t = static_cast<double>(frame_index * samples + i) / 48000.0;
      pcm[i] = static_cast<std::int16_t>(std::sin(pi2 * 440.0 * t) * 16000.0);
    }
    std::vector<std::uint8_t> encoded(dualsense_mic_audio::max_payload);
    const int n = opus_encode(enc, pcm.data(), samples, encoded.data(), static_cast<opus_int32>(encoded.size()));
    EXPECT_GT(n, 0);
    encoded.resize(static_cast<std::size_t>(std::max(n, 0)));
    return encoded;
  }

  std::vector<std::uint8_t> build_wire_packet(
    std::uint16_t controller,
    std::uint32_t sequence,
    std::uint16_t samples,
    const std::vector<std::uint8_t> &payload
  ) {
    std::vector<std::uint8_t> out(dualsense_mic_audio::header_size + payload.size());
    out[0] = dualsense_mic_audio::version;
    out[1] = dualsense_mic_audio::codec_opus_48k_mono;
    out[2] = static_cast<std::uint8_t>(controller & 0xFF);
    out[3] = static_cast<std::uint8_t>(controller >> 8);
    out[4] = static_cast<std::uint8_t>(sequence & 0xFF);
    out[5] = static_cast<std::uint8_t>((sequence >> 8) & 0xFF);
    out[6] = static_cast<std::uint8_t>((sequence >> 16) & 0xFF);
    out[7] = static_cast<std::uint8_t>((sequence >> 24) & 0xFF);
    out[8] = static_cast<std::uint8_t>(samples & 0xFF);
    out[9] = static_cast<std::uint8_t>(samples >> 8);
    out[10] = static_cast<std::uint8_t>(payload.size() & 0xFF);
    out[11] = static_cast<std::uint8_t>(payload.size() >> 8);
    std::copy(payload.begin(), payload.end(), out.begin() + dualsense_mic_audio::header_size);
    return out;
  }
}  // namespace

TEST(DualSenseMicAudio, ParsePacketValidatesHeaderAndBounds) {
  const std::vector<std::uint8_t> payload = {0xAA, 0xBB, 0xCC, 0xDD};
  auto wire = build_wire_packet(2, 77, 960, payload);
  const auto parsed = dualsense_mic_audio::parse_packet(wire);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->controller_number, 2);
  EXPECT_EQ(parsed->sequence, 77u);
  EXPECT_EQ(parsed->sample_count, 960);
  ASSERT_EQ(parsed->opus_payload.size(), payload.size());
  EXPECT_EQ(parsed->opus_payload[0], 0xAA);

  // Bad version
  auto bad = wire;
  bad[0] = 2;
  EXPECT_FALSE(dualsense_mic_audio::parse_packet(bad));

  // Bad codec
  bad = wire;
  bad[1] = 9;
  EXPECT_FALSE(dualsense_mic_audio::parse_packet(bad));

  // Controller >= 16
  bad = wire;
  bad[2] = 16;
  EXPECT_FALSE(dualsense_mic_audio::parse_packet(bad));

  // Sample count 0
  bad = wire;
  bad[8] = 0;
  bad[9] = 0;
  EXPECT_FALSE(dualsense_mic_audio::parse_packet(bad));

  // Truncated payload
  wire.pop_back();
  EXPECT_FALSE(dualsense_mic_audio::parse_packet(wire));
}

TEST(DualSenseMicAudio, SlotBufferDecodesDuplicatesStereoPlcAndIdleSilence) {
  int err = OPUS_OK;
  OpusEncoder *enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
  ASSERT_EQ(err, OPUS_OK);
  ASSERT_NE(enc, nullptr);

  dualsense_mic_audio::slot_buffer buf;
  const auto t0 = std::chrono::steady_clock::now();

  // Push 2 frames (0 and 1), then skip 2 and push 3 (triggers PLC for 2).
  const auto f0 = encode_sine_opus(enc, 0);
  const auto f1 = encode_sine_opus(enc, 1);
  const auto f2 = encode_sine_opus(enc, 2);
  (void) f2;  // simulated drop
  const auto f3 = encode_sine_opus(enc, 3);

  EXPECT_EQ(buf.push_opus(0, 960, f0, t0), 960u);
  // Duplicate seq 0 must be ignored.
  EXPECT_EQ(buf.push_opus(0, 960, f0, t0), 0u);
  EXPECT_EQ(buf.push_opus(1, 960, f1, t0 + std::chrono::milliseconds(20)), 960u);
  // Skipping seq 2 yields 960 (PLC) + 960 (decoded) = 1920 samples.
  EXPECT_EQ(buf.push_opus(3, 960, f3, t0 + std::chrono::milliseconds(60)), 1920u);

  // Read 20 ms stereo S16LE (960 frames * 4 bytes = 3840 bytes).
  std::vector<std::uint8_t> out(960 * 4, 0);
  const std::size_t copied = buf.read_stereo_pcm(out, t0 + std::chrono::milliseconds(65));
  EXPECT_EQ(copied, out.size());

  double energy = 0.0;
  for (std::size_t i = 0; i < out.size(); i += 4) {
    const auto left = static_cast<std::int16_t>(out[i] | (static_cast<std::uint16_t>(out[i + 1]) << 8));
    const auto right = static_cast<std::int16_t>(out[i + 2] | (static_cast<std::uint16_t>(out[i + 3]) << 8));
    ASSERT_EQ(left, right);
    energy += static_cast<double>(left) * static_cast<double>(left);
  }
  const double rms = std::sqrt(energy / 960.0);
  EXPECT_GT(rms, 3000.0);

  // After idle timeout (>150 ms), read returns silence and clears buffer.
  std::vector<std::uint8_t> silent(192 * 4, 0xFF);
  EXPECT_EQ(buf.read_stereo_pcm(silent, t0 + std::chrono::milliseconds(500)), 0u);
  for (std::uint8_t b : silent) {
    EXPECT_EQ(b, 0u);
  }
  EXPECT_EQ(buf.buffered_frames(), 0u);

  opus_encoder_destroy(enc);
}
