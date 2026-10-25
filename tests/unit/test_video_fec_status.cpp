/**
 * @file tests/unit/test_video_fec_status.cpp
 * @brief Test src/video_fec_status.h
 */

#include "../tests_common.h"
#include "src/video_fec_status.h"

#include <array>
#include <string>

namespace {
  using namespace stream::pacing;
  using report_bytes_t = std::array<std::uint8_t, fec_status_size>;

  // frame, highest, next contiguous, missing, data, parity, rx data, rx parity, pct, block, blocks
  constexpr report_bytes_t sample_bytes {0x12, 0x34, 0x56, 0x78, 0, 4, 0xff, 0xfe, 0, 1, 0, 10, 0, 2, 0, 9, 0, 1, 20, 1, 3};

  std::string text(const report_bytes_t &bytes) {
    return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
  }

  // Build a report for one block of a frame.
  std::string report(std::uint32_t frame, std::uint16_t data, std::uint16_t parity, std::uint16_t received_data, std::uint16_t received_parity, std::uint8_t percentage, std::uint8_t block, std::uint8_t blocks) {
    report_bytes_t bytes {};
    const auto put16 = [&](std::size_t at, std::uint16_t value) {
      bytes[at] = std::uint8_t(value >> 8);
      bytes[at + 1] = std::uint8_t(value);
    };
    for (int i = 0; i < 4; ++i) {
      bytes[i] = std::uint8_t(frame >> (24 - 8 * i));
    }
    put16(10, data);
    put16(12, parity);
    put16(14, received_data);
    put16(16, received_parity);
    bytes[18] = percentage;
    bytes[19] = block;
    bytes[20] = blocks;
    return text(bytes);
  }
}  // namespace

TEST(VideoFecStatus, ParsesBigEndianFields) {
  const auto status = parse_fec_status(text(sample_bytes));
  ASSERT_TRUE(status);
  EXPECT_EQ(status->frame, 0x12345678u);
  EXPECT_EQ(status->highest_received, 4);
  EXPECT_EQ(status->next_contiguous, 0xfffe);
  EXPECT_EQ(status->missing_before_highest, 1);
  EXPECT_EQ(status->data, 10);
  EXPECT_EQ(status->parity, 2);
  EXPECT_EQ(status->received_data, 9);
  EXPECT_EQ(status->received_parity, 1);
  EXPECT_EQ(status->percentage, 20);
  EXPECT_EQ(status->block, 1);
  EXPECT_EQ(status->blocks, 3);
}

TEST(VideoFecStatus, RejectsWrongSizes) {
  const auto bytes = text(sample_bytes);
  EXPECT_FALSE(parse_fec_status({}));
  EXPECT_FALSE(parse_fec_status(std::string_view {bytes}.substr(0, fec_status_size - 1)));
  EXPECT_FALSE(parse_fec_status(bytes + '\0'));
}

TEST(VideoFecStatus, RejectsInvalidReports) {
  const auto mutated = [](std::size_t at, std::uint8_t value) {
    auto bytes = sample_bytes;
    bytes[at] = value;
    return text(bytes);
  };
  EXPECT_FALSE(parse_fec_status(mutated(20, 0)));  // No blocks.
  EXPECT_FALSE(parse_fec_status(mutated(20, 5)));  // More than four blocks.
  EXPECT_FALSE(parse_fec_status(mutated(19, 3)));  // Block index beyond the count.
  EXPECT_FALSE(parse_fec_status(mutated(11, 0)));  // No data packets.
  EXPECT_FALSE(parse_fec_status(mutated(10, 4)));  // 1034 data packets.
  EXPECT_FALSE(parse_fec_status(mutated(13, 3)));  // Parity does not match the percentage.
  EXPECT_FALSE(parse_fec_status(mutated(15, 11)));  // More data received than sent.
  EXPECT_FALSE(parse_fec_status(mutated(17, 3)));  // More parity received than sent.
  EXPECT_FALSE(parse_fec_status(mutated(9, 3)));  // Missing more than could still be absent.
  EXPECT_FALSE(parse_fec_status(mutated(5, 40)));  // A span wider than the block.
}

TEST(VideoFecStatus, ShardLimitAppliesOnlyWithParity) {
  // 200 data + 100% parity exceeds one Reed-Solomon block.
  EXPECT_FALSE(parse_fec_status(report(1, 200, 200, 200, 200, 100, 0, 1)));
  EXPECT_TRUE(parse_fec_status(report(1, 127, 127, 127, 127, 100, 0, 1)));
  // Without FEC the block may be larger, and a PyroWave percentage may exceed 100.
  EXPECT_TRUE(parse_fec_status(report(1, 1000, 0, 1000, 0, 0, 0, 1)));
  EXPECT_FALSE(parse_fec_status(report(1, 1000, 100, 1000, 100, 10, 0, 1)));
}

TEST(VideoFecStatusTracker, ClassifiesBlocks) {
  fec_report_tracker_t tracker;
  const auto clean = tracker.record(report(100, 10, 2, 10, 2, 20, 0, 4), 100);
  ASSERT_TRUE(clean);
  EXPECT_EQ(clean->frame, 100u);
  EXPECT_EQ(clean->block, fec_block_t::clean_block());

  const auto recovered = tracker.record(report(100, 10, 2, 8, 2, 20, 1, 4), 100);
  ASSERT_TRUE(recovered);
  EXPECT_EQ(recovered->block, fec_block_t::recovered_block(2, 10));

  const auto lost = tracker.record(report(100, 10, 2, 7, 2, 20, 2, 4), 100);
  ASSERT_TRUE(lost);
  EXPECT_EQ(lost->block, fec_block_t::unrecoverable_block());

  // Parity alone can stand in for data once enough packets arrived.
  const auto parity = tracker.record(report(100, 10, 2, 8, 2, 20, 3, 4), 100);
  ASSERT_TRUE(parity);
  EXPECT_EQ(parity->block, fec_block_t::recovered_block(2, 10));
  EXPECT_EQ(tracker.reports(), 4u);
  EXPECT_EQ(tracker.invalid_reports(), 0u);
  EXPECT_EQ(tracker.duplicate_reports(), 0u);
}

TEST(VideoFecStatusTracker, CountsEachBlockOnce) {
  fec_report_tracker_t tracker;
  const auto first = report(7, 10, 2, 9, 2, 20, 0, 2);
  EXPECT_TRUE(tracker.record(first, 7));
  EXPECT_FALSE(tracker.record(first, 7));
  EXPECT_EQ(tracker.duplicate_reports(), 1u);
  // The other block of the same frame is still new.
  EXPECT_TRUE(tracker.record(report(7, 10, 2, 9, 2, 20, 1, 2), 7));
  // A frame cannot change its block count midway.
  EXPECT_FALSE(tracker.record(report(7, 10, 2, 9, 2, 20, 0, 3), 7));
  EXPECT_EQ(tracker.invalid_reports(), 1u);
}

TEST(VideoFecStatusTracker, RejectsUnsentStaleAndInvalidReports) {
  fec_report_tracker_t tracker;
  const auto block = report(500, 10, 2, 9, 2, 20, 0, 1);
  EXPECT_FALSE(tracker.record(block, std::nullopt));  // Nothing sent yet.
  EXPECT_FALSE(tracker.record(block, 499));  // From the future.
  EXPECT_FALSE(tracker.record(block, 500 + fec_report_tracker_t::window));  // Too old.
  EXPECT_FALSE(tracker.record("short", 500));
  EXPECT_EQ(tracker.invalid_reports(), 4u);
  EXPECT_TRUE(tracker.record(block, 500 + fec_report_tracker_t::window - 1));
}

TEST(VideoFecStatusTracker, WorksAcrossFrameCounterWrap) {
  fec_report_tracker_t tracker;
  EXPECT_TRUE(tracker.record(report(0xfffffffe, 10, 2, 9, 2, 20, 0, 1), 2));
  EXPECT_TRUE(tracker.record(report(1, 10, 2, 9, 2, 20, 0, 1), 2));
  EXPECT_FALSE(tracker.record(report(3, 10, 2, 9, 2, 20, 0, 1), 2));
}

TEST(VideoFecStatusTracker, ForgetsFramesOutsideTheWindow) {
  fec_report_tracker_t tracker;
  const auto old_block = report(10, 10, 2, 9, 2, 20, 0, 1);
  EXPECT_TRUE(tracker.record(old_block, 10));
  // Once the frame leaves the window its entry is dropped; a repeat is stale.
  EXPECT_TRUE(tracker.record(report(2000, 10, 2, 9, 2, 20, 0, 1), 2000));
  EXPECT_FALSE(tracker.record(old_block, 2000));
  EXPECT_EQ(tracker.duplicate_reports(), 0u);
}
