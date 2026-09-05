/**
 * @file src/platform/linux/pipewire_format.h
 * @brief PipeWire video format negotiation shared with focused SPA tests.
 */
#pragma once

#include <cstdint>
#include <limits>
#include <spa/param/video/format.h>
#include <spa/pod/builder.h>

namespace pipewire {
  inline void add_framerate_parameters(spa_pod_builder *builder, bool negotiate_maxframerate) {
    const auto variable_rate = SPA_FRACTION(0, 1);
    spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&variable_rate), 0);
    if (negotiate_maxframerate) {
      // Prefer an unpaced stream, but accept compositors such as KWin whose
      // maximum-rate range starts at 1 fps. A zero-only range has no overlap
      // with those producers and rejects every otherwise compatible format.
      const auto maximum_rate = SPA_FRACTION(std::numeric_limits<int32_t>::max(), 1);
      spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_maxFramerate,
                          SPA_POD_CHOICE_RANGE_Fraction(&variable_rate, &variable_rate, &maximum_rate), 0);
    }
  }
}  // namespace pipewire
