// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
// Standalone cross-platform regression test for native DS4 host initialization.
#include "inputtino/ds4_usb.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <tuple>

namespace {
int failures = 0;
void check(bool value, const char *message) {
  if (!value) { std::printf("FAIL: %s\n", message); ++failures; }
}
int le16(const std::uint8_t *p) {
  return static_cast<std::int16_t>(p[0] | (p[1] << 8));
}
}

int main() {
  using namespace inputtino::ds4_usb;
  // Parse the descriptor as a host does: feature page/usage/count are part of
  // transport detection, not just the total number of bits in each report.
  std::map<unsigned, std::tuple<unsigned, unsigned, unsigned>> features;
  unsigned page = 0, usage = 0, id = 0, size = 0, count = 0;
  std::map<unsigned, unsigned> input_bits, output_bits, feature_bits;
  const auto *bytes = reinterpret_cast<const unsigned char *>(report_descriptor);
  for (std::size_t i = 0; i < report_descriptor_size;) {
    unsigned prefix = bytes[i++];
    unsigned length = (prefix & 3) == 3 ? 4 : (prefix & 3);
    if (prefix == 0xfe || i + length > report_descriptor_size) return 2;
    unsigned value = 0;
    for (unsigned j = 0; j < length; ++j) value |= bytes[i++] << (8 * j);
    switch (prefix & 0xfc) {
      case 0x04: page = value; break;
      case 0x08: usage = value; break;
      case 0x84: id = value; break;
      case 0x74: size = value; break;
      case 0x94: count = value; break;
      case 0x80: input_bits[id] += size * count; break;
      case 0x90: output_bits[id] += size * count; break;
      case 0xb0:
        features[id] = {page, usage, count};
        feature_bits[id] += size * count;
        break;
    }
    if ((prefix & 0x0c) == 0) usage = 0;
  }
  unsigned max_feature = 0;
  for (auto [report, bits] : feature_bits) max_feature = std::max(max_feature, bits / 8 + 1);
  check(input_bits[1] / 8 + 1 == 64, "USB input is 64 bytes");
  check(output_bits[5] / 8 + 1 == 32, "USB output is 32 bytes");
  check(max_feature == 64 && max_feature >= 53, "native DS4 minimum feature capacity");
  check(features[0xa3] == std::tuple{0xff80u, 0x43u, 48u}, "USB firmware feature transport identity");
  check(features[0x12] == std::tuple{0xff02u, 0x21u, 15u}, "USB pairing feature identity");
  check(features[0x14] == std::tuple{0xff05u, 0x20u, 16u}, "USB control feature identity");
  check(!features.contains(6), "USB profile must not select Bluetooth initialization");

  feature_state state;
  std::array<std::uint8_t, 64> buffer;
  for (auto [report, length] : {std::pair{2u, 37u}, {0x12u, 16u}, {0x14u, 17u}, {0xa3u, 49u}}) {
    buffer.fill(0xcd);
    check(get_feature(report, buffer.data(), length - 1, state) == 0, "short feature request rejected");
    check(buffer.front() == 0xcd && buffer.back() == 0xcd, "failed request leaves buffer alone");
    check(get_feature(report, buffer.data(), length, state) == length, "exact Linux/native USB request length");
    check(buffer[length] == 0xcd, "exact request does not overwrite caller boundary");
    check(get_feature(report, buffer.data(), buffer.size(), state) == length, "maximum-size HID request returns actual length");
    check(buffer[0] == report, "response report id");
    check(std::all_of(buffer.begin() + length, buffer.end(), [](auto v) { return v == 0; }), "successful response has no stale tail");
    check(feature_bits[report] / 8 + 1 == length, "response length matches descriptor");
  }
  get_feature(0xa3, buffer.data(), buffer.size(), state);
  check((buffer[35] | (buffer[36] << 8)) >= 0x3100, "native USB firmware revision accepted");
  check(get_feature(0x7c, buffer.data(), buffer.size(), state) == 0, "unknown features are not fabricated");
  check(get_feature(2, nullptr, 64, state) == 0, "null feature destination rejected");

  // Reproduce the native game's set-feature command after firmware validation.
  buffer.fill(0); buffer[0] = 0x14; buffer[1] = 2;
  state.sensors_enabled = false;
  check(set_feature(0x14, buffer.data(), 17, state) && state.sensors_enabled, "USB sensor initialization succeeds");
  check(set_feature(0x14, buffer.data(), 64, state), "zero-padded HID control write succeeds");
  check(!set_feature(0x14, buffer.data(), 16, state), "short control write fails");
  buffer[1] = 0xff;
  check(!set_feature(0x14, buffer.data(), 17, state), "unsupported command fails");
  check(!set_feature(0x13, buffer.data(), 17, state), "unsupported feature write fails");

  // Kernel/SDL calibration calculations must reproduce the emitted units.
  get_feature(2, buffer.data(), buffer.size(), state);
  const int speed = le16(buffer.data() + 19) + le16(buffer.data() + 21);
  for (int axis = 0; axis < 3; ++axis) {
    int bias = le16(buffer.data() + 1 + axis * 2);
    int range = le16(buffer.data() + 7 + axis * 4) - le16(buffer.data() + 9 + axis * 4);
    check(bias == 0 && range > 0 && 16 * speed * 1024 / range == 1024,
          "16 gyro counts calibrate to one degree/s");
    int plus = le16(buffer.data() + 23 + axis * 4);
    int minus = le16(buffer.data() + 25 + axis * 4);
    check(plus == -minus && 8192 * 16384 / (plus - minus) == 8192,
          "8192 accelerometer counts calibrate to one g");
  }
  std::printf("DS4 USB contract: %s\n", failures ? "FAILED" : "PASS");
  return failures ? 1 : 0;
}
