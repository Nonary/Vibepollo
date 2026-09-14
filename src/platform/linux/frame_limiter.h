#pragma once

#include "src/framegen_policy.h"

namespace platf {
  enum class frame_limiter_owner {
    rtsp,
    webrtc
  };
  enum class proton_color_mode {
    sdr,
    sdr10,
    hdr
  };
  void frame_limiter_streaming_start(
    frame_limiter_owner owner,
    const framegen::stream_start_policy_t &policy,
    proton_color_mode color_mode
  );
  void frame_limiter_streaming_stop(frame_limiter_owner owner, bool keep_running = false);
}  // namespace platf
