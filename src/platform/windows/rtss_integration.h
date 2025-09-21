/**
 * @file src/platform/windows/rtss_integration.h
 * @brief Windows-only RTSS frame limit integration via RTSSHooks.dll and Sunshine-managed profile.
 */
#pragma once

#ifdef _WIN32

  #include <string>

namespace platf {
  struct rtss_status_t {
    bool enabled;  // frame limiter toggle
    bool path_configured;  // install_path not empty
    std::string configured_path;  // raw config value (may be relative)
    std::string resolved_path;  // absolute resolved path we will use
    bool path_exists;  // resolved path exists on disk
    bool hooks_found;  // RTSSHooks64.dll or RTSSHooks.dll exists
    bool profile_found;  // Sunshine-managed profile exists
    bool can_bootstrap_profile;  // Sunshine can create its profile automatically
    bool process_running;  // RTSS process currently running
  };

  // Apply RTSS frame limit and related settings at stream start.
  // fps is the integer client framerate.
  bool rtss_streaming_start(int fps);

  // Restore any RTSS settings modified at stream start.
  void rtss_streaming_stop();

  bool rtss_is_configured();

  // Query RTSS availability and installation status (no side effects).
  rtss_status_t rtss_get_status();
}  // namespace platf

#endif  // _WIN32
