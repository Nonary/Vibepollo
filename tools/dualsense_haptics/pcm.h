// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace dualsense_haptics {
  // WASAPI channels 0/1 are speakers; only 2/3 drive the actuators.
  // The wire protocol always carries 240 stereo S16LE frames at 48 kHz.
  class packetizer {
  public:
    void reset() {
      frames = 0;
      position = 0;
      have_previous = false;
      ++sequence;
    }

    template<class Emit>
    void push(const void *data, unsigned count, unsigned rate, bool floating, bool silent, Emit emit) {
      if (!rate || (!silent && !data)) {
        return;
      }
      if (silent) {
        previous = {};
      }
      for (unsigned i = 0; i < count; ++i) {
        std::array<float, 2> current {};
        if (!silent) {
          for (unsigned ch = 0; ch < 2; ++ch) {
            if (floating) {
              std::memcpy(&current[ch], static_cast<const std::uint8_t *>(data) + (i * 4 + ch + 2) * 4, 4);
            } else {
              std::int16_t sample;
              std::memcpy(&sample, static_cast<const std::uint8_t *>(data) + (i * 4 + ch + 2) * 2, 2);
              current[ch] = sample / 32768.0f;
            }
            if (!std::isfinite(current[ch])) {
              current[ch] = 0;
            }
            current[ch] = std::clamp(current[ch], -1.0f, 1.0f);
          }
        }
        if (!have_previous) {
          previous = current;
          have_previous = true;
        }
        while (position < 1.0) {
          for (unsigned ch = 0; ch < 2; ++ch) {
            const auto value = previous[ch] + (current[ch] - previous[ch]) * position;
            const auto sample = static_cast<std::int16_t>(std::clamp(std::lround(value * 32768), -32768L, 32767L));
            const auto bits = static_cast<std::uint16_t>(sample);
            samples[frames * 4 + ch * 2] = bits & 255;
            samples[frames * 4 + ch * 2 + 1] = bits >> 8;
          }
          if (++frames == 240) {
            emit(sequence++, samples);
            frames = 0;
          }
          position += rate / 48000.0;
        }
        position -= 1.0;
        previous = current;
      }
    }

  private:
    std::array<std::uint8_t, 960> samples {};
    std::array<float, 2> previous {};
    unsigned frames {};
    std::uint32_t sequence {};
    double position {};
    bool have_previous {};
  };
}  // namespace dualsense_haptics
