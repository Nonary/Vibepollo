/**
 * @file src/platform/windows/rtx_hdr_runtime.h
 * @brief Per-frame RTX HDR foreground/profile runtime state.
 */
#pragma once

#include "rtx_hdr_profile.h"

#include <chrono>
#include <optional>
#include <string>

#include <winsock2.h>
#include <windows.h>

namespace platf::rtx_hdr {

  struct frame_state_t: runtime_values_t {
    bool foreground_matches {false};
    bool lookup_available {false};
    std::string foreground_exe;
    std::string active_app_exe;
    std::string foreground_source;
  };

  class runtime_t {
  public:
    frame_state_t update_for_frame(const std::optional<RECT> &capture_rect);

  private:
    std::string cached_identity_key;
    resolved_profile_t cached_profile;
    frame_state_t cached_frame_state;
    std::chrono::steady_clock::time_point next_foreground_refresh {};
    std::chrono::steady_clock::time_point next_profile_refresh {};
  };

}  // namespace platf::rtx_hdr
