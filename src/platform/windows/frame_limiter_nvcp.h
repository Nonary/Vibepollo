/**
 * @file src/platform/windows/frame_limiter_nvcp.h
 * @brief NVIDIA Control Panel frame limiter provider.
 */
#pragma once

#ifdef _WIN32

namespace platf::frame_limiter_nvcp {

  bool is_available();
  bool streaming_start(int fps, bool apply_frame_limit, bool force_vsync_off, bool force_low_latency_off);
  void streaming_stop();

}  // namespace platf::frame_limiter_nvcp

#endif  // _WIN32
