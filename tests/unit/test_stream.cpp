/**
 * @file tests/unit/test_stream.cpp
 * @brief Test src/stream.*
 */

#include "../tests_common.h"
#include "src/stream_protocol.h"

TEST(VideoSendBatchTests, EncryptedDefaultPacketsStayWithinWindowsBufferingLimit) {
  // packetSize=1392 plus the 16-byte RTP allowance, then a 32-byte GCM prefix.
  // The old calculation selected 46 packets: 66,240 bytes instead of <=65,536.
  EXPECT_EQ(stream::video_send_batch_size(1408, 0, 65536), 46u);
  const auto encrypted_count = stream::video_send_batch_size(1408, 32, 65536);
  EXPECT_EQ(encrypted_count, 45u);
  EXPECT_LE(encrypted_count * (1408 + 32), 65536u);
}

TEST(VideoSendBatchTests, HonorsConfiguredByteBudgetAndSegmentationLimit) {
  EXPECT_EQ(stream::video_send_batch_size(1408, 32, 16 * 1024), 11u);
  EXPECT_EQ(stream::video_send_batch_size(1408, 32, 128 * 1024), 45u);
  EXPECT_EQ(stream::video_send_batch_size(64, 32, 65536), 64u);
  EXPECT_EQ(stream::video_send_batch_size(1408, 32, 0), 1u);
  EXPECT_EQ(stream::video_send_batch_size(1408, 32, 1000), 1u);
}

TEST(VideoSendBatchTests, EncryptionPrefixNeverPushesABatchPastTheByteLimit) {
  for (std::size_t block_size = 64; block_size <= 9000; ++block_size) {
    for (const std::size_t prefix_size : {0u, 32u}) {
      for (const std::size_t budget : {16u * 1024, 64u * 1024}) {
        const auto count = stream::video_send_batch_size(block_size, prefix_size, budget);
        ASSERT_LE(count * (block_size + prefix_size), budget);
        ASSERT_GE(count, 1u);
        ASSERT_LE(count, 64u);
      }
    }
  }
}

TEST(VideoFormatNameTests, CanonicalCodecNameNormalizesKnownAliases) {
  EXPECT_EQ(stream::canonical_codec_name("h264"), "H.264");
  EXPECT_EQ(stream::canonical_codec_name("H.264"), "H.264");
  EXPECT_EQ(stream::canonical_codec_name("hevc"), "HEVC");
  EXPECT_EQ(stream::canonical_codec_name("H265"), "HEVC");
  EXPECT_EQ(stream::canonical_codec_name("av1"), "AV1");
}

TEST(VideoFormatNameTests, CanonicalCodecNamePreservesUnknownValues) {
  EXPECT_EQ(stream::canonical_codec_name("vp9"), "vp9");
  EXPECT_TRUE(stream::canonical_codec_name({}).empty());
}

TEST(ConcatAndInsertTests, ConcatNoInsertionTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(0, 2, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatLargeStrideTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(1, sizeof(b1) + sizeof(b2) + 1, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {0, 'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatSmallStrideTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(1, 1, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {0, 'a', 0, 'b', 0, 'c', 0, 'd', 0, 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ControlPacketParsing, RejectsRuntPacketsBeforeReadingType) {
  EXPECT_FALSE(stream::decode_control_packet({}));

  const char one_byte[] = {'\x34'};
  EXPECT_FALSE(stream::decode_control_packet(std::string_view {one_byte, sizeof(one_byte)}));
}

TEST(ControlPacketParsing, DecodesTypeAndPayloadSafely) {
  const char packet[] = {'\x34', '\x12', 'a', 'b'};

  const auto decoded = stream::decode_control_packet(std::string_view {packet, sizeof(packet)});

  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->type, 0x1234);
  EXPECT_EQ(decoded->payload, "ab");
}

TEST(ControlPacketParsing, AllowsTypeOnlyPacketWithoutPayloadUnderflow) {
  const char packet[] = {'\x34', '\x12'};

  const auto decoded = stream::decode_control_packet(std::string_view {packet, sizeof(packet)});

  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->type, 0x1234);
  EXPECT_TRUE(decoded->payload.empty());
}
