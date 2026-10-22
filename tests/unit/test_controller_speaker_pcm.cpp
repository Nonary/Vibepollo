// SPDX-License-Identifier: GPL-3.0-or-later
#include "src/controller_speaker_pcm.h"

#include <gtest/gtest.h>
#include <future>
#include <chrono>

TEST(ControllerSpeakerPcm, CapabilityIsPlayStationOnlyAndSessionNegotiated) {
  using namespace controller_speaker_pcm;
  EXPECT_TRUE(capable(2, 0x4000));
  for (const auto type : {0, 1, 3, 4, 255}) {
    EXPECT_FALSE(capable(type, 0xffff));
  }
  EXPECT_FALSE(capable(2, 0x8000));
  block pcm {};
  pcm.owner = 0x0102030405060708ULL;
  pcm.generation = 7;
  const auto epoch = std::make_shared<controller_speaker_pcm::producer_epoch>(7);
  EXPECT_FALSE(encode(0, 13, 0, pcm, epoch));
  EXPECT_FALSE(encode(0x04, 13, 0, pcm, epoch));
  EXPECT_TRUE(encode(0x08, 13, 0, pcm, epoch));
  EXPECT_FALSE(encode(0x08, 12, 0, pcm, epoch));
}

TEST(ControllerSpeakerPcm, ExactLittleEndianWireAndSamples) {
  controller_speaker_pcm::block pcm {};
  pcm.owner = 0x0102030405060708ULL;
  pcm.generation = 0x12345678;
  pcm.sequence = 0x9abcdef0;
  pcm.sample_position = 0x1122334455667788;
  pcm.flags = 1;
  for (unsigned i = 0; i < pcm.samples.size(); ++i) {
    pcm.samples[i] = std::uint8_t(i);
  }
  const auto epoch = std::make_shared<controller_speaker_pcm::producer_epoch>(pcm.generation);
  const auto packet = controller_speaker_pcm::encode(8, 13, 15, pcm, epoch);
  ASSERT_TRUE(packet);
  EXPECT_EQ(packet->size(), 996u);
  const std::array<std::uint8_t, 36> expected {2, 0x56, 0xe0, 3, 2, 1, 15, 0, 0x78, 0x56, 0x34, 0x12, 0xf0, 0xde, 0xbc, 0x9a, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xf0, 0, 1, 0, 8, 7, 6, 5, 4, 3, 2, 1};
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), packet->begin()));
  EXPECT_TRUE(std::equal(pcm.samples.begin(), pcm.samples.end(), packet->begin() + 36));
}

TEST(ControllerSpeakerPcm, RejectsMalformedAndExpiredOrResetProducer) {
  controller_speaker_pcm::block pcm {};
  pcm.owner = 0x0102030405060708ULL;
  pcm.generation = 9;
  auto epoch = std::make_shared<controller_speaker_pcm::producer_epoch>(9);
  std::weak_ptr<controller_speaker_pcm::producer_epoch> weak = epoch;
  EXPECT_FALSE(controller_speaker_pcm::encode(8, 13, 16, pcm, weak));
  pcm.owner = 0;
  EXPECT_FALSE(controller_speaker_pcm::encode(8, 13, 0, pcm, weak));
  pcm.owner = 1;
  pcm.sample_position = UINT64_MAX;
  EXPECT_FALSE(controller_speaker_pcm::encode(8, 13, 0, pcm, weak));
  pcm.sample_position = 0;
  pcm.flags = 2;
  EXPECT_FALSE(controller_speaker_pcm::encode(8, 13, 0, pcm, weak));
  pcm.flags = 0;
  epoch->reset(10);
  EXPECT_FALSE(controller_speaker_pcm::encode(8, 13, 0, pcm, weak));
  pcm.generation = 10;
  EXPECT_TRUE(controller_speaker_pcm::encode(8, 13, 0, pcm, weak));
  pcm.generation = 0;
  epoch->reset(0);
  EXPECT_FALSE(controller_speaker_pcm::encode(8, 13, 0, pcm, weak));
  pcm.generation = 10;
  epoch.reset();
  EXPECT_FALSE(controller_speaker_pcm::encode(8, 13, 0, pcm, weak));
}
TEST(ControllerSpeakerPcm, ResetAfterSerializationPreventsStaleQueueAdmission) {
  using namespace controller_speaker_pcm;
  auto epoch = std::make_shared<producer_epoch>(7);
  block pcm {};
  pcm.owner = 0x0102030405060708ULL;
  pcm.generation = 7;
  ASSERT_TRUE(encode(8, 13, 0, pcm, epoch));
  // Force the original race: reset after serialization, before queue admission.
  epoch->reset(8);
  unsigned queued = 0;
  EXPECT_EQ(enqueue_if_current(epoch, 7, [&] { ++queued; return 0; }), 0);
  EXPECT_EQ(queued, 0u);
  EXPECT_EQ(enqueue_if_current(epoch, 8, [&] { ++queued; return -1; }), -1);
  EXPECT_EQ(queued, 1u);
  std::weak_ptr<producer_epoch> expired = epoch;
  epoch.reset();
  EXPECT_EQ(enqueue_if_current(expired, 8, [&] { ++queued; return 0; }), 0);
  EXPECT_EQ(queued, 1u);
}

TEST(ControllerSpeakerPcm, ResetAndFinalQueueAdmissionAreSerialized) {
  using namespace controller_speaker_pcm;
  auto epoch = std::make_shared<producer_epoch>(7);
  std::promise<void> entered, release, resetting;
  auto release_future = release.get_future();
  auto send = std::async(std::launch::async, [&] {
    return enqueue_if_current(epoch, 7, [&] {
      entered.set_value();
      release_future.wait();
      EXPECT_EQ(epoch->load(), 7u);
      return 0;
    });
  });
  entered.get_future().wait();
  auto reset = std::async(std::launch::async, [&] {
    resetting.set_value();
    epoch->reset(8);
  });
  resetting.get_future().wait();
  // Reset may not complete while the old generation is being enqueued.
  EXPECT_EQ(reset.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
  release.set_value();
  EXPECT_EQ(send.get(), 0);
  reset.get();
  EXPECT_EQ(epoch->load(), 8u);
}
