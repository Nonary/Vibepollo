/** @brief Decode oversized hexadecimal strings without writing beyond the target. */
#include "../tests_common.h"
#include "src/utility.h"
#include <array>
#include <cstdint>

TEST(UtilityHex, OversizedInputKeepsLowOrderBytesInEitherDirection) {
  EXPECT_EQ(util::from_hex<std::uint32_t>("123456789A"), util::endian::little(std::uint32_t {0x3456789A}));
  EXPECT_EQ(util::from_hex<std::uint32_t>("123456789A", true), util::endian::big(std::uint32_t {0x3456789A}));
  EXPECT_EQ(util::from_hex<std::uint8_t>("123456789A"), 0x9A);
}
TEST(UtilityHex, ShortAndSeparatedInputRemainSupported) {
  EXPECT_EQ(util::from_hex<std::uint32_t>("ABCD"), util::endian::little(std::uint32_t {0xABCD}));
  EXPECT_EQ((util::from_hex<std::array<std::uint8_t, 4>>("12:34:56:78", true)),
            (std::array<std::uint8_t, 4> {0x12, 0x34, 0x56, 0x78}));
}

TEST(UtilityHex, EmptyVectorsAreSafeInEitherDirection) {
  std::vector<std::uint8_t> empty;
  EXPECT_EQ(util::hex_vec(empty), "");
  EXPECT_EQ(util::hex_vec(empty, true), "");
}
TEST(UtilityHex, AppendingObjectPreservesItsByteRepresentation) {
  const std::array<std::uint8_t, 3> value {0, 0x7f, 0xff};
  std::vector<std::uint8_t> bytes {0x42};
  util::append_struct(bytes, value);
  EXPECT_EQ(bytes, (std::vector<std::uint8_t> {0x42, 0, 0x7f, 0xff}));
}
