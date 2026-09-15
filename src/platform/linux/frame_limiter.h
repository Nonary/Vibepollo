#pragma once

#include "src/framegen_policy.h"

namespace platf {
  enum class frame_limiter_owner {
    rtsp,
    webrtc,
    // Keeps the external Proton hook alive while a launch is handed to an
    // already-running Steam daemon, before the stream owner is established.
    application
  };
  enum class proton_color_mode {
    sdr,
    sdr10,
    hdr
  };

  struct proton_launch_environment_t {
    proton_color_mode color_mode {proton_color_mode::sdr};
    // This is resolved at stream setup, rather than rereading a global HDR
    // state when an external Proton title eventually starts.
    bool wayland_hdr_compatibility {};
  };

  void frame_limiter_streaming_start(
    frame_limiter_owner owner,
    const framegen::stream_start_policy_t &policy,
    proton_launch_environment_t environment
  );
  void frame_limiter_streaming_stop(frame_limiter_owner owner, bool keep_running = false);
}  // namespace platf
