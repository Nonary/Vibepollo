#pragma once

#include "src/framegen_policy.h"

namespace platf {
  enum class frame_limiter_owner {
    rtsp,
    webrtc
  };
  void frame_limiter_streaming_start(frame_limiter_owner owner, const framegen::stream_start_policy_t &policy);
  void frame_limiter_streaming_stop(frame_limiter_owner owner, bool keep_running = false);
}  // namespace platf
