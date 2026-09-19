// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include "third-party/moonlight-common-c/src/ControllerHaptics.h"

namespace platf::gamepad {
  // Preserve signed waveform samples. Never mix controller speaker channels
  // into the actuators. Packets are 5 ms and fit below the control MTU.
  class ds5_haptics_packetizer {
  public:
    template<class Emit>
    void push(const unsigned char *pcm, std::size_t bytes, Emit emit) {
      if (!pcm || bytes % 8) return;
      const auto now = std::chrono::steady_clock::now();
      if (now - last_input > std::chrono::milliseconds(20)) {
        if (frames) ++sequence;
        frames = 0;
      }
      last_input = now;
      for (std::size_t offset = 0; offset < bytes; offset += 8) {
        std::memcpy(samples.data() + frames * 4, pcm + offset + 4, 4);
        if (++frames == ML_HAPTICS_MAX_FRAMES) {
          emit(sequence++, samples);
          frames = 0;
        }
      }
    }
  private:
    std::array<std::uint8_t, ML_HAPTICS_MAX_FRAMES * ML_HAPTICS_FRAME_BYTES> samples {};
    std::size_t frames = 0;
    std::uint32_t sequence = 0;
    std::chrono::steady_clock::time_point last_input {};
  };
}
