/**
 * @file src/platform/windows/rtx_hdr_runtime.cpp
 */

#include "rtx_hdr_runtime.h"

#include "foreground_app.h"

#include "src/config.h"

#include <algorithm>

namespace platf::rtx_hdr {
  namespace {
    constexpr auto FOREGROUND_REFRESH_INTERVAL = std::chrono::milliseconds(100);
    constexpr auto PROFILE_REFRESH_INTERVAL = std::chrono::seconds(5);

    runtime_values_t config_runtime_values() {
      runtime_values_t values;
      values.enabled = config::video.rtx_hdr.enabled;
      values.contrast = std::clamp(config::video.rtx_hdr.contrast + 100, 0, 200);
      values.saturation = std::clamp(config::video.rtx_hdr.saturation + 100, 0, 200);
      values.middle_gray = config::video.rtx_hdr.middle_gray;
      values.peak_brightness = config::video.rtx_hdr.peak_brightness;
      values.source = profile_source_e::config;
      return values;
    }
  }  // namespace

  frame_state_t runtime_t::update_for_frame(const std::optional<RECT> &capture_rect) {
    const auto now = std::chrono::steady_clock::now();
    if (now < next_foreground_refresh) {
      return cached_frame_state;
    }
    next_foreground_refresh = now + FOREGROUND_REFRESH_INTERVAL;

    frame_state_t frame;

    auto foreground = platf::foreground_app::snapshot(capture_rect);
    frame.foreground_matches = foreground.matches_active_app;
    frame.foreground_exe = foreground.foreground_exe;
    frame.active_app_exe = foreground.active_app_exe;
    frame.foreground_source = foreground.source;

    if (!foreground.matches_active_app) {
      frame.enabled = false;
      frame.source = profile_source_e::none;
      cached_frame_state = frame;
      return cached_frame_state;
    }

    const auto identity_key = foreground.active_app_exe + "\n" + foreground.foreground_exe + "\n" + foreground.active_app_name + "\n" + foreground.source;
    if (identity_key != cached_identity_key || now >= next_profile_refresh) {
      cached_identity_key = identity_key;
      cached_profile = resolve_profile_for_executable(foreground.active_app_exe.empty() ? foreground.foreground_exe : foreground.active_app_exe);
      next_profile_refresh = now + PROFILE_REFRESH_INTERVAL;
    }

    auto values = materialize_runtime_values_for_tests(cached_profile, config_runtime_values());
    frame.enabled = values.enabled;
    frame.contrast = values.contrast;
    frame.saturation = values.saturation;
    frame.middle_gray = values.middle_gray;
    frame.peak_brightness = values.peak_brightness;
    frame.source = values.source;
    frame.lookup_available = cached_profile.lookup_available;
    cached_frame_state = frame;
    return cached_frame_state;
  }

}  // namespace platf::rtx_hdr
