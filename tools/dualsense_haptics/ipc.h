// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// clang-format off
#include <winsock2.h>
#include <windows.h>
// clang-format on
#include <array>
#include <cstdint>

namespace dualsense_haptics {
  inline constexpr wchar_t environment_key[] = L"VIBEPOLLO_DUALSENSE_HAPTICS_MAPPING";
  inline constexpr unsigned slot_count = 16;
  inline constexpr unsigned queue_size = 16;
  inline constexpr std::uint32_t magic = 0x35485356;

  struct packet {
    std::uint32_t sequence;
    std::uint32_t timestamp;
    LONG generation;
    std::array<std::uint8_t, 960> samples;
  };

  struct slot {
    volatile LONG active;
    volatile LONG generation;
    volatile LONG sequence;
    volatile LONG lock;
    unsigned read;
    unsigned write;
    packet packets[queue_size];
  };

  struct shared_state {
    std::uint32_t signature;
    volatile LONG alive;
    slot slots[slot_count];
  };

  inline GUID container_id(unsigned index) {
    // Matches libvirtualgamepad's per-controller ContainerID, not a physical pad.
    return {0x9a2f1c74, 0x6b83, 0x4d51, {0xa7, 0x0e, 0x35, 0x1d, 0xc6, 0x84, 0x00, static_cast<unsigned char>(index)}};
  }

  inline bool try_lock(slot &s) {
    return InterlockedCompareExchange(&s.lock, 1, 0) == 0;
  }

  inline void unlock(slot &s) {
    InterlockedExchange(&s.lock, 0);
  }
}  // namespace dualsense_haptics
