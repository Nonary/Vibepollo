// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <span>

namespace stream {
  class haptics_gain_t {
  public:
    void reset() {
      active_ = false;
      lift_ = 0.0;
      peak_envelope_ = 0.0;
    }

    // Measure each actuator's peak over a 5 ms S16LE stereo packet. Add the
    // same amount to both active peak envelopes, then scale each channel's
    // whole waveform linearly. This preserves the difference between their
    // peak strengths without adding DC, mixing channels or flattening peaks.
    void apply(std::span<std::uint8_t> samples, double strength, std::uint32_t sequence, std::chrono::steady_clock::time_point now) {
      if (samples.empty() || samples.size() % 4 != 0 || !std::isfinite(strength) || strength < 0.0 || strength > 4.0) {
        reset();
        return;
      }
      if (strength <= 1.0) {
        reset();
        if (strength != 1.0) {
          for (std::size_t offset = 0; offset < samples.size(); offset += 2) {
            write(samples, offset, read(samples, offset) * strength);
          }
        }
        return;
      }

      std::array<double, 2> peaks {};
      for (std::size_t offset = 0; offset < samples.size(); offset += 2) {
        const int sample = read(samples, offset);
        const double limit = sample < 0 ? 32768.0 : 32767.0;
        auto &peak = peaks[(offset / 2) % 2];
        peak = std::max(peak, std::abs(sample) / limit);
      }
      const double strongest = std::max(peaks[0], peaks[1]);
      const bool discontinuity = !active_ || sequence != next_sequence_ || strength != strength_ || now < last_packet_ || now - last_packet_ > std::chrono::milliseconds {50};
      if (strongest == 0.0) {
        // Brief silent gaps must not forget the strong effect immediately
        // preceding them. Keep the shared lift without emitting any vibration.
        if (discontinuity || now - last_active_packet_ >= std::chrono::milliseconds {100}) {
          reset();
        } else {
          next_sequence_ = sequence + 1;
          last_packet_ = now;
        }
        return;
      }
      // A silent actuator stays silent; it must not prevent boosting the other.
      const double weakest = peaks[0] == 0.0 ? peaks[1] :
                             peaks[1] == 0.0 ? peaks[0] : std::min(peaks[0], peaks[1]);
      const double elapsed_samples = static_cast<double>(samples.size() / 4);
      if (discontinuity || strongest >= peak_envelope_) {
        peak_envelope_ = strongest;
        peak_time_ = now;
      } else if (now - peak_time_ > std::chrono::milliseconds {100}) {
        // Keep recent strong effects as the headroom reference. Otherwise a
        // following weak effect would get a bigger lift and lose its contrast.
        // After the hold, release the reference gradually over 200 ms.
        const double retained = std::exp(-elapsed_samples / (48000.0 * 0.200));
        peak_envelope_ = strongest + (peak_envelope_ - strongest) * retained;
      }
      const double target = std::min(weakest * (strength - 1.0), 1.0 - peak_envelope_);
      if (discontinuity) {
        lift_ = target;
      } else {
        // Ease increases over 20 ms. Back off immediately when the next
        // packet needs more headroom, so no sample can overflow or hard-clip.
        const double retained = std::exp(-elapsed_samples / (48000.0 * 0.020));
        lift_ = std::min(target, target + (lift_ - target) * retained);
      }
      active_ = true;
      strength_ = strength;
      next_sequence_ = sequence + 1;
      last_packet_ = now;
      last_active_packet_ = now;

      std::array<double, 2> gains {1.0, 1.0};
      for (std::size_t channel = 0; channel < peaks.size(); ++channel) {
        if (peaks[channel] != 0.0) {
          gains[channel] += lift_ / peaks[channel];
        }
      }
      for (std::size_t offset = 0; offset < samples.size(); offset += 2) {
        write(samples, offset, read(samples, offset) * gains[(offset / 2) % 2]);
      }
    }

  private:
    static int read(std::span<const std::uint8_t> samples, std::size_t offset) {
      const int raw = samples[offset] | (int(samples[offset + 1]) << 8);
      return raw >= 0x8000 ? raw - 0x10000 : raw;
    }

    static void write(std::span<std::uint8_t> samples, std::size_t offset, double sample) {
      const auto encoded = static_cast<std::uint16_t>(static_cast<int>(std::round(sample)));
      samples[offset] = static_cast<std::uint8_t>(encoded);
      samples[offset + 1] = static_cast<std::uint8_t>(encoded >> 8);
    }

    bool active_ {};
    double lift_ {};
    double peak_envelope_ {};
    double strength_ {};
    std::uint32_t next_sequence_ {};
    std::chrono::steady_clock::time_point last_packet_ {};
    std::chrono::steady_clock::time_point last_active_packet_ {};
    std::chrono::steady_clock::time_point peak_time_ {};
  };
}  // namespace stream
