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
#include <src/controller_speaker_pcm.h>

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

TEST(InputValidation, SpeakerBindingIsExactBoundedAndLegacyArrivalUnchanged) {
  std::vector<std::uint8_t> binding {0,0,0,16,9,0,0,0x55,15,0,1,0,8,7,6,5,4,3,2,1};
  ASSERT_TRUE(input::validation::validate_packet(binding));
  auto parsed = controller_speaker_pcm::parse_binding(binding);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->first, 15);
  EXPECT_EQ(parsed->second, 0x0102030405060708ULL);
  auto truncated = binding;
  truncated.pop_back();
  write_u32_be(truncated, 0, truncated.size() - 4);
  EXPECT_FALSE(input::validation::validate_packet(truncated));
  binding.push_back(0);
  write_u32_be(binding, 0, binding.size() - 4);
  EXPECT_FALSE(input::validation::validate_packet(binding));
  SS_CONTROLLER_ARRIVAL_PACKET legacy {};
  auto arrival = packet_bytes(legacy);
  write_u32_be(arrival, 0, arrival.size() - 4);
  write_u32_le(arrival, 4, SS_CONTROLLER_ARRIVAL_MAGIC);
  EXPECT_TRUE(input::validation::validate_packet(arrival));
}

TEST(InputValidation, SpeakerBindingOwnerIsSingleUseSessionScopedAndFailClosed) {
  std::vector<std::uint8_t> binding {0,0,0,16,9,0,0,0x55,15,0,1,0,8,7,6,5,4,3,2,1};
  controller_speaker_pcm::owner_bindings pending;
  EXPECT_EQ(pending.take(15), 0u); // Legacy/unbound arrival.
  EXPECT_FALSE(pending.bind(binding, [](auto) { return false; })); // Already allocated.
  EXPECT_EQ(pending.take(15), 0u);
  EXPECT_TRUE(pending.bind(binding, [](auto) { return true; }));
  EXPECT_EQ(pending.take(15), 0x0102030405060708ULL);
  EXPECT_EQ(pending.take(15), 0u); // Cannot leak into a later legacy arrival.
  EXPECT_TRUE(pending.bind(binding, [](auto) { return true; }));
  controller_speaker_pcm::owner_bindings reconnect;
  EXPECT_EQ(reconnect.take(15), 0u);
  for (auto offset : {8, 10, 12}) {
    auto malformed = binding;
    if (offset == 8) malformed[8] = 16;
    else if (offset == 10) malformed[10] = 2;
    else std::fill(malformed.begin() + 12, malformed.end(), 0);
    EXPECT_FALSE(controller_speaker_pcm::parse_binding(malformed));
  }
}
