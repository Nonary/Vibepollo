// SPDX-License-Identifier: GPL-3.0-or-later
#include "src/platform/linux/input/ds5_haptics.h"
#include <cstdio>
#include <vector>

int main() {
  platf::gamepad::ds5_haptics_packetizer packetizer;
  std::vector<uint8_t> usb(240 * 8);
  for (unsigned i = 0; i < 240; ++i) {
    MlHapticsWrite16(usb.data() + i * 8, 0x5555); // speaker must not leak
    MlHapticsWrite16(usb.data() + i * 8 + 2, 0x7777);
    MlHapticsWrite16(usb.data() + i * 8 + 4, uint16_t(int(i) - 120));
    MlHapticsWrite16(usb.data() + i * 8 + 6, uint16_t(120 - int(i)));
  }
  unsigned count = 0;
  bool valid = true;
  auto emit = [&](uint32_t sequence, const auto &samples) {
    valid &= sequence == count++;
    for (unsigned i = 0; i < 240; ++i) {
      valid &= MlHapticsRead16(samples.data() + i * 4) == uint16_t(int(i) - 120);
      valid &= MlHapticsRead16(samples.data() + i * 4 + 2) == uint16_t(120 - int(i));
    }
  };
  packetizer.push(usb.data(), 7, emit); // reject partial USB frames
  for (unsigned pass = 0; pass < 3; ++pass) {
    for (unsigned offset = 0; offset < usb.size(); offset += 384)
      packetizer.push(usb.data() + offset, 384, emit);
  }
  if (!valid || count != 3) return 1;

  std::array<uint8_t, ML_HAPTICS_MAX_PAYLOAD> packet {};
  packet[0] = packet[1] = 1;
  MlHapticsWrite16(packet.data() + 2, 15);
  MlHapticsWrite32(packet.data() + 4, UINT32_MAX);
  MlHapticsWrite16(packet.data() + 8, ML_HAPTICS_MAX_FRAMES);
  if (!MlHapticsValidate(packet.data(), packet.size())) return 2;
  for (size_t length = 0; length < packet.size(); ++length)
    if (MlHapticsValidate(packet.data(), length)) return 3;
  packet[10] = 1;
  if (MlHapticsValidate(packet.data(), packet.size())) return 4;
  packet[10] = 0; packet[2] = 16;
  if (MlHapticsValidate(packet.data(), packet.size())) return 5;
  packet[2] = 0; packet[1] = 2;
  if (MlHapticsValidate(packet.data(), packet.size())) return 6;
  std::puts("DS5 haptics: signed channels, framing, sequence and malformed packets passed");
}
