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

#include <src/input_validation_policy.h>

namespace {

  template<typename Packet>
  std::vector<std::uint8_t> packet_bytes(Packet packet, std::size_t actual_size = sizeof(Packet)) {
    std::vector<std::uint8_t> bytes(actual_size);
    std::memcpy(bytes.data(), &packet, std::min(actual_size, sizeof(Packet)));
    return bytes;
  }

  void write_u32_be(std::vector<std::uint8_t> &bytes, const std::size_t offset, const std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value >> 24);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 3] = static_cast<std::uint8_t>(value);
  }

  void write_u32_le(std::vector<std::uint8_t> &bytes, const std::size_t offset, const std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
  }

}  // namespace

TEST(InputValidation, RejectsShortPacketHeader) {
  EXPECT_FALSE(input::validation::validate_packet(std::vector<std::uint8_t> {0x00, 0x01, 0x02}));
}

TEST(InputValidation, RejectsDeclaredSizeMismatch) {
  NV_KEYBOARD_PACKET packet {};
  auto bytes = packet_bytes(packet, sizeof(packet) - 1);
  write_u32_be(bytes, 0, sizeof(NV_KEYBOARD_PACKET) - sizeof(packet.header.size));
  write_u32_le(bytes, sizeof(packet.header.size), KEY_DOWN_EVENT_MAGIC);
  EXPECT_FALSE(input::validation::validate_packet(bytes));
}

TEST(InputValidation, AcceptsBoundedUnicodePayloadAndRejectsOversizedOne) {
  NV_UNICODE_PACKET packet {};
  auto valid = packet_bytes(packet, sizeof(packet.header) + 5);
  write_u32_be(valid, 0, sizeof(packet.header.magic) + 5);
  write_u32_le(valid, sizeof(packet.header.size), UTF8_TEXT_EVENT_MAGIC);
  std::memcpy(valid.data() + sizeof(packet.header), "hello", 5);
  EXPECT_TRUE(input::validation::validate_packet(valid));

  auto invalid = packet_bytes(packet, sizeof(packet));
  write_u32_be(invalid, 0, sizeof(packet.header.magic) + UTF8_TEXT_EVENT_MAX_COUNT + 1);
  write_u32_le(invalid, sizeof(packet.header.size), UTF8_TEXT_EVENT_MAGIC);
  EXPECT_FALSE(input::validation::validate_packet(invalid));
}

TEST(InputValidation, RejectsUnknownMagic) {
  NV_INPUT_HEADER packet {};
  auto bytes = packet_bytes(packet);
  write_u32_be(bytes, 0, sizeof(packet.magic));
  write_u32_le(bytes, sizeof(packet.size), 0xDEADBEEF);
  EXPECT_FALSE(input::validation::validate_packet(bytes));
}

TEST(InputTouchMapping, NormalizesUsingPlatformOffsetContract) {
  input::validation::touch_port_t touch_port {
    1920,
    0,
    1920,
    1080,
    1.0f,
    1.0f,
  };
  std::pair<float, float> coords {960.0f, 540.0f};

  auto monitor_port = input::validation::normalize_touch_port(touch_port, coords);

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
