// SPDX-License-Identifier: GPL-3.0-or-later
#include "src/controller_speaker_pcm.h"

#include <gtest/gtest.h>

TEST(ControllerSpeakerPcm, CapabilityIsPlayStationOnlyAndSessionNegotiated) {
  using namespace controller_speaker_pcm;
  EXPECT_TRUE(capable(2, 0x4000));
  for (const auto type : {0, 1, 3, 4, 255}) {
    EXPECT_FALSE(capable(type, 0xffff));
  }
  EXPECT_FALSE(capable(2, 0x8000));
  block pcm {};
  pcm.generation = 7;
  const auto epoch = std::make_shared<std::atomic<std::uint32_t>>(7);
  EXPECT_FALSE(encode(0, 13, 0, pcm, epoch));
  EXPECT_FALSE(encode(0x04, 13, 0, pcm, epoch));
  EXPECT_TRUE(encode(0x08, 13, 0, pcm, epoch));
  EXPECT_FALSE(encode(0x08, 12, 0, pcm, epoch));
}

TEST(ControllerSpeakerPcm, ExactLittleEndianWireAndSamples) {
  controller_speaker_pcm::block pcm {};
  pcm.generation = 0x12345678;
  pcm.sequence = 0x9abcdef0;
  pcm.sample_position = 0x1122334455667788;
  pcm.flags = 1;
  for (unsigned i = 0; i < pcm.samples.size(); ++i) {
    pcm.samples[i] = std::uint8_t(i);
  }
  const auto epoch = std::make_shared<std::atomic<std::uint32_t>>(pcm.generation);
  const auto packet = controller_speaker_pcm::encode(8, 13, 15, pcm, epoch);
  ASSERT_TRUE(packet);
  EXPECT_EQ(packet->size(), 988u);
  const std::array<std::uint8_t, 28> expected {2, 0x56, 0xd8, 3, 1, 1, 15, 0, 0x78, 0x56, 0x34, 0x12, 0xf0, 0xde, 0xbc, 0x9a, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xf0, 0, 1, 0};
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), packet->begin()));
  EXPECT_TRUE(std::equal(pcm.samples.begin(), pcm.samples.end(), packet->begin() + 28));
}

TEST(ControllerSpeakerPcm, RejectsMalformedAndExpiredOrResetProducer) {
  controller_speaker_pcm::block pcm {};
  pcm.generation = 9;
  auto epoch = std::make_shared<std::atomic<std::uint32_t>>(9);
  std::weak_ptr<std::atomic<std::uint32_t>> weak = epoch;
  EXPECT_FALSE(controller_speaker_pcm::encode(8, 13, 16, pcm, weak));
  pcm.flags = 2;
  EXPECT_FALSE(controller_speaker_pcm::encode(8, 13, 0, pcm, weak));
  pcm.flags = 0;
  epoch->store(10);
  EXPECT_FALSE(controller_speaker_pcm::encode(8, 13, 0, pcm, weak));
  pcm.generation = 10;
  EXPECT_TRUE(controller_speaker_pcm::encode(8, 13, 0, pcm, weak));
  pcm.generation = 0;
  epoch->store(0);
  EXPECT_FALSE(controller_speaker_pcm::encode(8, 13, 0, pcm, weak));
  pcm.generation = 10;
  epoch.reset();
  EXPECT_FALSE(controller_speaker_pcm::encode(8, 13, 0, pcm, weak));
}
