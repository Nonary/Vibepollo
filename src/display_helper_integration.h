/**
 * @file src/display_helper_integration.h
 * @brief Cross-platform wrapper for display helper integration. On Windows, routes to the IPC helper; on other platforms, no-ops.
 */
#pragma once

#include "src/config.h"
#include "src/rtsp.h"

#ifdef _WIN32
  // Bring in the Windows implementation in the correct namespace
  #include "src/platform/windows/display_helper_integration.h"

namespace display_helper_integration {
  // On Windows, we exclusively use the helper and suppress in-process fallback.
  inline bool suppress_fallback() {
    return true;
  }

  // Enumerate display devices as a JSON string suitable for API responses.
  // Implemented in the Windows backend.
  std::string enumerate_devices_json();
}  // namespace display_helper_integration

#else

namespace display_helper_integration {
  // Non-Windows: No-op implementations that allow callers to fallback to in-process logic
  inline bool apply_from_session(const config::video_t &, const rtsp_stream::launch_session_t &) {
    return false;
  }

  inline bool revert() {
    return false;
  }

  inline bool export_golden_restore() {
    return false;
  }

  inline bool reset_persistence() {
    return false;
  }

  inline bool suppress_fallback() {
    return false;
  }

  inline std::string enumerate_devices_json() {
    return "[]";
  }
}  // namespace display_helper_integration

#endif
