// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

namespace controller_speaker_pcm {
  // The 16-bit controller capability space is exhausted. This alias is
  // explicitly type-scoped: 0x4000 means speaker PCM only for PlayStation
  // controllers; the pending Steam extension uses it for Steam attributes.
  constexpr std::uint16_t playstation_capability = 0x4000;
  constexpr std::uint32_t session_feature = 0x08;
  constexpr std::uint16_t packet_type = 0x5602;
  constexpr std::uint16_t frames_per_block = 240;
  constexpr std::size_t payload_size = 24 + frames_per_block * 4;

  inline bool capable(std::uint8_t type, std::uint16_t capabilities) {
    return type == 0x02 && (capabilities & playstation_capability) != 0;
  }

  struct block {
    std::uint32_t generation;
    std::uint32_t sequence;
    std::uint64_t sample_position;
    std::uint16_t flags;
    std::array<std::uint8_t, frames_per_block * 4> samples;
  };

  // Includes the four-byte control header. Encryption remains in the existing
  // session-owned encoder, exactly as for actuator PCM.
  inline std::optional<std::array<std::uint8_t, payload_size + 4>> encode(
    std::uint32_t session_flags,
    unsigned control_protocol,
    std::uint16_t controller,
    const block &pcm,
    const std::weak_ptr<std::atomic<std::uint32_t>> &epoch
  ) {
    const auto live = epoch.lock();
    if (control_protocol != 13 || !(session_flags & session_feature) || controller >= 16 || !pcm.generation ||
        (pcm.flags & ~1u) || !live || live->load(std::memory_order_acquire) != pcm.generation) {
      return std::nullopt;
    }
    std::array<std::uint8_t, payload_size + 4> result {};
    const auto write = [&](std::size_t offset, std::uint64_t value, unsigned bytes) {
      for (unsigned i = 0; i < bytes; ++i) {
        result[offset + i] = std::uint8_t(value >> (8 * i));
      }
    };
    write(0, packet_type, 2);
    write(2, payload_size, 2);
    result[4] = result[5] = 1;
    write(6, controller, 2);
    write(8, pcm.generation, 4);
    write(12, pcm.sequence, 4);
    write(16, pcm.sample_position, 8);
    write(24, frames_per_block, 2);
    write(26, pcm.flags, 2);
    std::copy(pcm.samples.begin(), pcm.samples.end(), result.begin() + 28);
    return result;
  }
}  // namespace controller_speaker_pcm
