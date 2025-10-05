/**
 * @file tests/unit/test_zwpad.cpp
 */

#include "../tests_common.h"

#include <src/zwpad.h>

TEST(Zwpad, PadWidthHandlesSmallCounts) {
  EXPECT_EQ(zwpad::pad_width_for_count(1), 1u);
  EXPECT_EQ(zwpad::pad_width_for_count(2), 1u);
  EXPECT_EQ(zwpad::pad_width_for_count(3), 2u);
}

TEST(Zwpad, PadForOrderingAllowsZeroPadBits) {
  EXPECT_EQ(zwpad::pad_for_ordering("Example", 0, 0), std::string {"Example"});
  EXPECT_THROW(zwpad::pad_for_ordering("Example", 0, 1), std::out_of_range);
}

TEST(Zwpad, PadForOrderingEncodesBits) {
  const std::string bit0 = "\xE2\x80\x8B";
  const std::string bit1 = "\xE2\x80\x8C";
  EXPECT_EQ(zwpad::pad_for_ordering("Alpha", 2, 0), bit0 + bit0 + "Alpha");
  EXPECT_EQ(zwpad::pad_for_ordering("Bravo", 2, 1), bit0 + bit1 + "Bravo");
  EXPECT_EQ(zwpad::pad_for_ordering("Charlie", 2, 2), bit1 + bit0 + "Charlie");
}
