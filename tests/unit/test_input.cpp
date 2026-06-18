/**
 * @file tests/unit/test_input.cpp
 * @brief Regression tests for input packet validation hardening.
 */
#include "../tests_common.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include <moonlight-common-c/src/Input.h>
}

#include <src/input.h>
#include <src/utility.h>

namespace {

  template<typename Packet>
  std::vector<std::uint8_t> packet_bytes(Packet packet, std::size_t actual_size = sizeof(Packet)) {
    std::vector<std::uint8_t> bytes(actual_size);
    std::memcpy(bytes.data(), &packet, std::min(actual_size, sizeof(Packet)));
    return bytes;
  }

}  // namespace

TEST(InputValidation, RejectsShortPacketHeader) {
  EXPECT_FALSE(input::validate_packet_for_tests(std::vector<std::uint8_t> {0x00, 0x01, 0x02}));
}

TEST(InputValidation, RejectsDeclaredSizeMismatch) {
  NV_KEYBOARD_PACKET packet {};
  packet.header.size = util::endian::big<std::uint32_t>(sizeof(NV_KEYBOARD_PACKET) - sizeof(packet.header.size));
  packet.header.magic = util::endian::little<std::uint32_t>(KEY_DOWN_EVENT_MAGIC);

  auto bytes = packet_bytes(packet, sizeof(packet) - 1);
  EXPECT_FALSE(input::validate_packet_for_tests(bytes));
}

TEST(InputValidation, AcceptsBoundedUnicodePayloadAndRejectsOversizedOne) {
  NV_UNICODE_PACKET packet {};
  packet.header.size = util::endian::big<std::uint32_t>(sizeof(packet.header.magic) + 5);
  packet.header.magic = util::endian::little<std::uint32_t>(UTF8_TEXT_EVENT_MAGIC);
  std::memcpy(packet.text, "hello", 5);

  auto valid = packet_bytes(packet, sizeof(packet.header) + 5);
  EXPECT_TRUE(input::validate_packet_for_tests(valid));

  packet.header.size = util::endian::big<std::uint32_t>(sizeof(packet.header.magic) + UTF8_TEXT_EVENT_MAX_COUNT + 1);
  auto invalid = packet_bytes(packet, sizeof(packet));
  EXPECT_FALSE(input::validate_packet_for_tests(invalid));
}

TEST(InputValidation, RejectsUnknownMagic) {
  NV_INPUT_HEADER packet {};
  packet.size = util::endian::big<std::uint32_t>(sizeof(packet.magic));
  packet.magic = util::endian::little<std::uint32_t>(0xDEADBEEF);

  auto bytes = packet_bytes(packet);
  EXPECT_FALSE(input::validate_packet_for_tests(bytes));
}

TEST(InputTouchMapping, NormalizesUsingPlatformOffsetContract) {
  input::touch_port_t touch_port {
    {
      1920,
      0,
      1920,
      1080,
      1920,
      1080,
    },
    3840,
    1080,
    0.0f,
    0.0f,
    1.0f,
    1.0f,
    3840,
    1080,
  };
  std::pair<float, float> coords {960.0f, 540.0f};

  auto monitor_port = input::monitor_touch_port_for_tests(touch_port, coords);

  ASSERT_TRUE(monitor_port.has_value());
  EXPECT_EQ(monitor_port->offset_x, 1920);
  EXPECT_EQ(monitor_port->offset_y, 0);
  EXPECT_EQ(monitor_port->width, 1920);
  EXPECT_EQ(monitor_port->height, 1080);
#ifdef __linux__
  EXPECT_FLOAT_EQ(coords.first, -0.5f);
#else
  EXPECT_FLOAT_EQ(coords.first, 0.5f);
#endif
  EXPECT_FLOAT_EQ(coords.second, 0.5f);
}
