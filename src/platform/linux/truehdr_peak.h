#pragma once

#include "truehdr.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace platf::linux_truehdr {
  // The pinned Linux RGB10A2 runtime accepts 2000 but saturates near 1000 nits.
  // Run its native tone curve at 1000, then restore the requested linear peak.
  inline settings_t native_settings(settings_t requested) {
    if (requested.peak > 1000) {
      requested.middle_gray = std::clamp(int(std::lround(requested.middle_gray * 1000.0 / requested.peak)), 10, 100);
      requested.peak = 1000;
    }
    return requested;
  }

  inline std::array<unsigned, 1024> peak_lut(const settings_t &requested) {
    const auto native = native_settings(requested);
    std::array<unsigned, 1024> result {};
    for (unsigned code = 0; code < result.size(); ++code) {
      const double p = std::pow(code / 1023.0, 32.0 / 2523.0);
      const double nits = 10000.0 * std::pow(std::max(p - 3424.0 / 4096.0, 0.0) / (2413.0 / 128.0 - 2392.0 / 128.0 * p), 16384.0 / 2610.0);
      const double scale = requested.peak / 1000.0;
      double corrected = nits * scale;
      // The SDK's gray floor is 10. At low gray settings, uniform gain would
      // brighten midtones too. Anchor those at the requested gray and expand
      // only the remaining range; both segments are continuous and monotonic.
      if (requested.middle_gray < 10.0 * scale) {
        corrected = nits <= native.middle_gray ? nits * requested.middle_gray / native.middle_gray :
          requested.middle_gray + (nits - native.middle_gray) * (requested.peak - requested.middle_gray) / (1000.0 - native.middle_gray);
      }
      const double x = std::pow(std::clamp(corrected / 10000.0, 0.0, 1.0), 2610.0 / 16384.0);
      const double pq = std::pow((3424.0 / 4096.0 + 2413.0 / 128.0 * x) / (1.0 + 2392.0 / 128.0 * x), 2523.0 / 32.0);
      result[code] = std::clamp(int(std::lround(pq * 1023.0)), 0, 1023);
    }
    return result;
  }

  // Tiny GPU lookup pass on the effect stream. No frame transfer or CPU wait.
  int create_peak_lut(void **device_lut);
  void destroy_peak_lut(void *device_lut);
  int upload_peak_lut(void *device_lut, const unsigned *lut);
  int apply_peak_lut(void *pixels, unsigned pitch, unsigned width, unsigned height, const void *lut, void *stream);
}  // namespace platf::linux_truehdr
