/**
 * @file src/platform/windows/frame_limiter.cpp
 * @brief Frame limiter provider selection and orchestration.
 */

#ifdef _WIN32

  #include "frame_limiter.h"

  #include "src/config.h"
  #include "src/logging.h"
  #include "src/platform/windows/frame_limiter_nvcp.h"
  #include "src/platform/windows/misc.h"

  #include <algorithm>
  #include <array>
  #include <cctype>
  #include <string>
  #include <vector>

namespace platf {

  namespace {

    frame_limiter_provider g_active_provider = frame_limiter_provider::none;
    bool g_nvcp_started = false;

    frame_limiter_provider parse_provider(const std::string &value) {
      std::string normalized;
      normalized.reserve(value.size());
      for (char ch : value) {
        if (ch == '-' || ch == '_' || ch == ' ') {
          continue;
        }
        normalized.push_back((char) std::tolower(static_cast<unsigned char>(ch)));
      }
      if (normalized.empty() || normalized == "auto") {
        return frame_limiter_provider::auto_detect;
      }
      if (normalized == "rtss") {
        return frame_limiter_provider::rtss;
      }
      if (normalized == "nvidiacontrolpanel" || normalized == "nvidia" || normalized == "nvcp") {
        return frame_limiter_provider::nvidia_control_panel;
      }
      if (normalized == "none" || normalized == "disabled") {
        return frame_limiter_provider::none;
      }
      return frame_limiter_provider::auto_detect;
    }

    bool provider_available(frame_limiter_provider provider) {
      switch (provider) {
        case frame_limiter_provider::nvidia_control_panel:
          return frame_limiter_nvcp::is_available();
        case frame_limiter_provider::rtss:
          return rtss_is_configured();
        default:
          return false;
      }
    }

  }  // namespace

  const char *frame_limiter_provider_to_string(frame_limiter_provider provider) {
    switch (provider) {
      case frame_limiter_provider::none:
        return "none";
      case frame_limiter_provider::auto_detect:
        return "auto";
      case frame_limiter_provider::rtss:
        return "rtss";
      case frame_limiter_provider::nvidia_control_panel:
        return "nvidia-control-panel";
      default:
        return "unknown";
    }
  }

  void frame_limiter_streaming_start(int fps) {
    g_active_provider = frame_limiter_provider::none;
    g_nvcp_started = false;

    const bool frame_limit_enabled = config::frame_limiter.enable;
    const bool nvidia_gpu_present = platf::has_nvidia_gpu();
    const bool nvcp_ready = frame_limiter_nvcp::is_available();
    const bool want_nv_overrides = config::rtss.disable_vsync_ullm && nvidia_gpu_present && nvcp_ready;

    bool nvcp_already_invoked = false;

    if (frame_limit_enabled) {
      auto configured = parse_provider(config::frame_limiter.provider);
      std::vector<frame_limiter_provider> order;

      switch (configured) {
        case frame_limiter_provider::none:
          break;
        case frame_limiter_provider::auto_detect:
          order = {frame_limiter_provider::nvidia_control_panel, frame_limiter_provider::rtss};
          break;
        default:
          order = {configured};
          break;
      }

      bool applied = false;
      for (auto provider : order) {
        if (!provider_available(provider)) {
          BOOST_LOG(warning) << "Frame limiter provider '" << frame_limiter_provider_to_string(provider)
                             << "' not available";
          if (configured != frame_limiter_provider::auto_detect) {
            break;
          }
          continue;
        }

        if (provider == frame_limiter_provider::nvidia_control_panel) {
          bool ok = frame_limiter_nvcp::streaming_start(
            fps,
            true,
            want_nv_overrides,
            want_nv_overrides
          );
          if (ok) {
            g_active_provider = frame_limiter_provider::nvidia_control_panel;
            applied = true;
            nvcp_already_invoked = true;
            BOOST_LOG(info) << "Frame limiter provider 'nvidia-control-panel' applied";
            break;
          }
        } else if (provider == frame_limiter_provider::rtss) {
          bool ok = rtss_streaming_start(fps);
          if (ok) {
            g_active_provider = frame_limiter_provider::rtss;
            applied = true;
            BOOST_LOG(info) << "Frame limiter provider 'rtss' applied";
            break;
          }
        }

        BOOST_LOG(warning) << "Frame limiter provider '" << frame_limiter_provider_to_string(provider)
                           << "' failed to apply limit";
        if (configured != frame_limiter_provider::auto_detect) {
          break;
        }
      }

      if (!applied && configured != frame_limiter_provider::none) {
        BOOST_LOG(warning) << "Frame limiter enabled but no provider applied";
      }
    }

    if (want_nv_overrides && !nvcp_already_invoked) {
      frame_limiter_nvcp::streaming_start(fps, false, true, true);
      nvcp_already_invoked = true;
    }

    if (nvcp_already_invoked) {
      g_nvcp_started = true;
    }
  }

  void frame_limiter_streaming_stop() {
    if (g_active_provider == frame_limiter_provider::rtss) {
      rtss_streaming_stop();
    }

    if (g_nvcp_started || g_active_provider == frame_limiter_provider::nvidia_control_panel) {
      frame_limiter_nvcp::streaming_stop();
    }

    g_active_provider = frame_limiter_provider::none;
    g_nvcp_started = false;
  }

  frame_limiter_provider frame_limiter_active_provider() {
    return g_active_provider;
  }

  frame_limiter_status_t frame_limiter_get_status() {
    frame_limiter_status_t status {};
    status.enabled = config::frame_limiter.enable;
    status.configured_provider = parse_provider(config::frame_limiter.provider);
    status.active_provider = g_active_provider;
    status.nvidia_available = platf::has_nvidia_gpu();
    status.nvcp_ready = frame_limiter_nvcp::is_available();
    status.rtss_available = rtss_is_configured();
    status.disable_vsync_ullm = config::rtss.disable_vsync_ullm;
    status.nv_overrides_supported = status.nvidia_available && status.nvcp_ready;
    status.rtss = rtss_get_status();
    return status;
  }

}  // namespace platf

#endif  // _WIN32
