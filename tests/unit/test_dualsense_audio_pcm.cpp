// SPDX-License-Identifier: GPL-3.0-or-later
#include "tools/dualsense_haptics/pcm.h"

#include <gtest/gtest.h>
#include <limits>

TEST(DualSenseAudio, ExtractsActuatorsAndPreservesSign) {
  std::array<float, 240 * 4> pcm {};
  for (unsigned i = 0; i < 240; ++i) {
    pcm[i * 4] = 1;
    pcm[i * 4 + 1] = -1;
    pcm[i * 4 + 2] = -0.5;
    pcm[i * 4 + 3] = 0.25;
  }
  dualsense_haptics::packetizer converter;
  unsigned packets = 0;
  converter.push(pcm.data(), 240, 48000, true, false, [&](auto, const auto &samples) {
    ++packets;
    for (unsigned i = 0; i < 240; ++i) {
      EXPECT_EQ(samples[i * 4], 0);
      EXPECT_EQ(samples[i * 4 + 1], 0xc0);
      EXPECT_EQ(samples[i * 4 + 2], 0);
      EXPECT_EQ(samples[i * 4 + 3], 0x20);
    }
  });
  EXPECT_EQ(packets, 1);
}

TEST(DualSenseAudio, ResamplesAcrossBlockBoundariesAndSilence) {
  for (unsigned rate : {44100u, 48000u, 96000u}) {
    dualsense_haptics::packetizer converter;
    std::array<std::int16_t, 100 * 4> pcm {};
    unsigned packets = 0;
    const auto emit = [&](auto sequence, const auto &samples) {
      EXPECT_EQ(sequence, packets++);
      for (auto sample : samples) {
        EXPECT_EQ(sample, 0);
      }
    };
    for (unsigned frames = 0; frames < rate; frames += 100) {
      converter.push(pcm.data(), std::min(100u, rate - frames), rate, false, false, emit);
    }
    EXPECT_EQ(packets, 200);
    converter.push(nullptr, rate / 200, rate, false, true, emit);
    EXPECT_EQ(packets, 201);
  }
}

TEST(DualSenseAudio, BoundsInvalidFloatAndDropsPartialPacketOnReset) {
  dualsense_haptics::packetizer converter;
  std::array<float, 240 * 4> pcm {};
  for (unsigned i = 0; i < 240; ++i) {
    pcm[i * 4 + 2] = std::numeric_limits<float>::quiet_NaN();
    pcm[i * 4 + 3] = 2;
  }
  unsigned packets = 0;
  converter.push(pcm.data(), 100, 48000, true, false, [&](auto, const auto &) {
    ++packets;
  });
  converter.reset();
  converter.push(pcm.data(), 240, 48000, true, false, [&](auto sequence, const auto &samples) {
    ++packets;
    EXPECT_EQ(sequence, 1);
    EXPECT_EQ(samples[0], 0);
    EXPECT_EQ(samples[1], 0);
    EXPECT_EQ(samples[2], 0xff);
    EXPECT_EQ(samples[3], 0x7f);
  });
  EXPECT_EQ(packets, 1);
}
