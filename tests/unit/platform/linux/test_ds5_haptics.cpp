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

  // The same four-channel USB frames must independently yield the first two
  // speaker channels, with an owner and epoch that prevent stale playback.
  std::weak_ptr<controller_speaker_pcm::producer_epoch> retired_epoch;
  std::uint32_t retired_generation = 0;
  {
    using speaker = platf::gamepad::ds5_speaker_packetizer;
    speaker output(0x1234);
    auto now = speaker::clock::now();
    std::vector<controller_speaker_pcm::block> blocks;
    std::weak_ptr<controller_speaker_pcm::producer_epoch> epoch;
    auto collect = [&](const auto &block, const auto &token) { blocks.push_back(block); epoch = token; };
    output.push(usb.data(), 120 * 8, now, collect);
    output.push(usb.data() + 120 * 8, 120 * 8, now, collect);
    if (blocks.size() != 1 || blocks[0].owner != 0x1234 || blocks[0].generation != 1 ||
        blocks[0].sequence != 0 || blocks[0].sample_position != 0 || blocks[0].flags != 1) return 7;
    for (unsigned i = 0; i < 240; ++i) {
      if (MlHapticsRead16(blocks[0].samples.data() + i * 4) != 0x5555 ||
          MlHapticsRead16(blocks[0].samples.data() + i * 4 + 2) != 0x7777) return 8;
    }
    if (output.take_first_nonzero(controller_speaker_pcm::block {}) ||
        !output.take_first_nonzero(blocks[0]) ||
        output.take_first_nonzero(blocks[0])) return 15;
    const auto stale = blocks[0];
    output.push(usb.data(), usb.size(), now + std::chrono::milliseconds(60), collect);
    if (blocks.size() != 2 || blocks[1].generation != 2 ||
        controller_speaker_pcm::encode(8, 13, 0, stale, epoch)) return 9;
    if (!controller_speaker_pcm::encode(8, 13, 0, blocks[1], epoch)) return 10;
    if (output.take_first_nonzero(blocks[1])) return 16;
    output.push(usb.data(), 7, now, collect);
    if (controller_speaker_pcm::encode(8, 13, 0, blocks[1], epoch)) return 11;
    retired_epoch = epoch;
    retired_generation = blocks[1].generation;
  }
  if (!retired_epoch.expired()) return 12;
  // A recreated host slot can keep the same client owner. Its generation must
  // advance so already-sent packets cannot masquerade as the new producer.
  {
    platf::gamepad::ds5_speaker_packetizer replacement(0x1234);
    bool advanced = false;
    replacement.push(usb.data(), usb.size(), decltype(replacement)::clock::now(), [&](const auto &block, const auto &) {
      advanced = block.generation > retired_generation;
    });
    if (!advanced) return 13;
  }
  {
    platf::gamepad::ds5_speaker_packetizer partial(0x1234);
    auto now = decltype(partial)::clock::now();
    unsigned emitted = 0;
    auto fresh = usb;
    for (unsigned i = 0; i < 240; ++i) fresh[i * 8] = 0x99;
    const auto check = [&](const auto &block, const auto &) {
      ++emitted;
      for (unsigned i = 0; i < 240; ++i) if (block.samples[i * 4] != 0x99) valid = false;
    };
    partial.push(usb.data(), 120 * 8, now, check);
    partial.push(fresh.data(), fresh.size(), now + std::chrono::milliseconds(60), check);
    if (emitted != 1 || !valid) return 14;
  }

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
