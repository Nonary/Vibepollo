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
  #include <optional>
  #include <string>
  #include <vector>

namespace platf {

  namespace {

    frame_limiter_provider g_active_provider = frame_limiter_provider::none;
    bool g_nvcp_started = false;
    bool g_gen1_framegen_fix_active = false;
    bool g_gen2_framegen_fix_active = false;
    bool g_prev_frame_limiter_enabled = false;
    std::string g_prev_frame_limiter_provider;
    bool g_prev_frame_limiter_provider_set = false;
    bool g_prev_disable_vsync_ullm = false;
    std::string g_prev_rtss_frame_limit_type;
    bool g_prev_rtss_frame_limit_type_set = false;

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

  void frame_limiter_streaming_start(int fps, bool gen1_framegen_fix, bool gen2_framegen_fix, std::optional<int> lossless_rtss_limit, bool smooth_motion) {
    g_active_provider = frame_limiter_provider::none;
    g_nvcp_started = false;
    g_gen1_framegen_fix_active = gen1_framegen_fix;
    g_gen2_framegen_fix_active = gen2_framegen_fix;

    const bool frame_limit_enabled = config::frame_limiter.enable || gen1_framegen_fix || gen2_framegen_fix || (lossless_rtss_limit && *lossless_rtss_limit > 0);
    const bool nvidia_gpu_present = platf::has_nvidia_gpu();
    const bool nvcp_ready = frame_limiter_nvcp::is_available();
    const bool want_smooth_motion = smooth_motion && nvidia_gpu_present;

    // Gen1 fix: Force RTSS with front-edge sync (for DLSS3, FSR3, Lossless Scaling)
    if (gen1_framegen_fix) {
      g_prev_frame_limiter_enabled = config::frame_limiter.enable;
      g_prev_frame_limiter_provider = config::frame_limiter.provider;
      g_prev_frame_limiter_provider_set = true;
      g_prev_disable_vsync_ullm = config::rtss.disable_vsync_ullm;
      g_prev_rtss_frame_limit_type = config::rtss.frame_limit_type;
      g_prev_rtss_frame_limit_type_set = true;
      config::frame_limiter.enable = true;
      config::frame_limiter.provider = "rtss";
      config::rtss.disable_vsync_ullm = true;
      config::rtss.frame_limit_type = "front edge sync";
    }
    // Gen2 fix: Force NVIDIA Control Panel limiter (for DLSS4)
    else if (gen2_framegen_fix) {
      g_prev_frame_limiter_enabled = config::frame_limiter.enable;
      g_prev_frame_limiter_provider = config::frame_limiter.provider;
      g_prev_frame_limiter_provider_set = true;
      config::frame_limiter.enable = true;
      config::frame_limiter.provider = "nvidia-control-panel";
    } else {
      g_prev_frame_limiter_provider_set = false;
      g_prev_rtss_frame_limit_type_set = false;
    }

    const bool want_nv_overrides = (config::rtss.disable_vsync_ullm || gen1_framegen_fix || gen2_framegen_fix) && nvidia_gpu_present && nvcp_ready;

    bool nvcp_already_invoked = false;
    const int effective_limit = (lossless_rtss_limit && *lossless_rtss_limit > 0) ? *lossless_rtss_limit : fps;

    if (frame_limit_enabled) {
      auto configured = parse_provider(config::frame_limiter.provider);
      std::vector<frame_limiter_provider> order;

      switch (configured) {
        case frame_limiter_provider::none:
          break;
        case frame_limiter_provider::auto_detect:
          order = {frame_limiter_provider::rtss, frame_limiter_provider::nvidia_control_panel};
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
            effective_limit,
            true,
            want_nv_overrides,
            want_nv_overrides,
            want_smooth_motion
          );
          if (ok) {
            g_active_provider = frame_limiter_provider::nvidia_control_panel;
            applied = true;
            nvcp_already_invoked = true;
            BOOST_LOG(info) << "Frame limiter provider 'nvidia-control-panel' applied";
            break;
          }
        } else if (provider == frame_limiter_provider::rtss) {
          bool ok = rtss_streaming_start(effective_limit);
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

    if ((want_nv_overrides || want_smooth_motion) && !nvcp_already_invoked) {
      bool nvcp_result = frame_limiter_nvcp::streaming_start(
        effective_limit,
        false,
        want_nv_overrides,
        want_nv_overrides,
        want_smooth_motion
      );
      nvcp_already_invoked = true;
      if (want_smooth_motion && !nvcp_result) {
        BOOST_LOG(warning) << "Requested NVIDIA Smooth Motion but NVIDIA Control Panel overrides failed";
      }
    }

    if (nvcp_already_invoked) {
      g_nvcp_started = true;
    }
  }

  void frame_limiter_streaming_stop() {
    if (g_gen1_framegen_fix_active || g_gen2_framegen_fix_active) {
      config::frame_limiter.enable = g_prev_frame_limiter_enabled;
      if (g_prev_frame_limiter_provider_set) {
        config::frame_limiter.provider = g_prev_frame_limiter_provider;
      }
      if (g_gen1_framegen_fix_active) {
        config::rtss.disable_vsync_ullm = g_prev_disable_vsync_ullm;
        if (g_prev_rtss_frame_limit_type_set) {
          config::rtss.frame_limit_type = g_prev_rtss_frame_limit_type;
        }
      }
      g_gen1_framegen_fix_active = false;
      g_gen2_framegen_fix_active = false;
      g_prev_frame_limiter_provider_set = false;
      g_prev_rtss_frame_limit_type_set = false;
    }

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
