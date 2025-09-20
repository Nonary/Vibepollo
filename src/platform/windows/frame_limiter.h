/**
 * @file src/platform/windows/frame_limiter.h
 * @brief Frame limiter provider selection and lifecycle management.
 */
#pragma once

#ifdef _WIN32

  #include "src/platform/windows/rtss_integration.h"

  #include <string>

namespace platf {

  enum class frame_limiter_provider {
    none,
    auto_detect,
    rtss,
    nvidia_control_panel
  };

  const char *frame_limiter_provider_to_string(frame_limiter_provider provider);

  struct frame_limiter_status_t {
    bool enabled;
    frame_limiter_provider configured_provider;
    frame_limiter_provider active_provider;
    bool nvidia_available;
    bool nvcp_ready;
    bool rtss_available;
    bool disable_vsync_ullm;
    bool nv_overrides_supported;
    rtss_status_t rtss;
  };

  void frame_limiter_streaming_start(int fps);
  void frame_limiter_streaming_stop();

  frame_limiter_provider frame_limiter_active_provider();
  frame_limiter_status_t frame_limiter_get_status();

}  // namespace platf

#endif  // _WIN32
