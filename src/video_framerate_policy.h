/**
 * @file src/video_framerate_policy.h
 * @brief Helpers for normalizing the encoder's requested frame rate.
 */
#pragma once

namespace video::framerate_policy {
  [[nodiscard]] constexpr int effective_encoding_framerate_milli_fps(
    const int requested_milli_fps,
    const int stream_fps
  ) noexcept {
    if (requested_milli_fps > 0) {
      return requested_milli_fps;
    }
    return stream_fps > 0 ? stream_fps * 1000 : 60000;
  }
}  // namespace video::framerate_policy
