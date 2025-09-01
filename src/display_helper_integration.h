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
  inline bool suppress_fallback() { return true; }
}

#else

namespace display_helper_integration {
  // Non-Windows: No-op implementations that allow callers to fallback to in-process logic
  inline bool apply_from_session(const config::video_t &, const rtsp_stream::launch_session_t &) { return false; }
  inline bool revert() { return false; }
  inline bool suppress_fallback() { return false; }
}

#endif
