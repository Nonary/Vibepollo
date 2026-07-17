/**
 * @file tests/unit/test_stream.cpp
 * @brief Test src/stream.*
 */

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace stream {
  struct control_packet_view_t {
    std::uint16_t type = 0;
    std::string_view payload;
  };

  std::vector<uint8_t> concat_and_insert(uint64_t insert_size, uint64_t slice_size, const std::string_view &data1, const std::string_view &data2);
  std::string canonical_codec_name(std::string_view codec);
  std::optional<control_packet_view_t> decode_control_packet_for_tests(std::string_view packet_bytes);
}

#include "../tests_common.h"

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
  EXPECT_FALSE(stream::decode_control_packet_for_tests({}));

  const char one_byte[] = {'\x34'};
  EXPECT_FALSE(stream::decode_control_packet_for_tests(std::string_view {one_byte, sizeof(one_byte)}));
}

TEST(ControlPacketParsing, DecodesTypeAndPayloadSafely) {
  const char packet[] = {'\x34', '\x12', 'a', 'b'};

  const auto decoded = stream::decode_control_packet_for_tests(std::string_view {packet, sizeof(packet)});

  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->type, 0x1234);
  EXPECT_EQ(decoded->payload, "ab");
}

TEST(ControlPacketParsing, AllowsTypeOnlyPacketWithoutPayloadUnderflow) {
  const char packet[] = {'\x34', '\x12'};

  const auto decoded = stream::decode_control_packet_for_tests(std::string_view {packet, sizeof(packet)});

  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->type, 0x1234);
  EXPECT_TRUE(decoded->payload.empty());
}
