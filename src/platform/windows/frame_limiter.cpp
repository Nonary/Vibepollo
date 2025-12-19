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
    int g_last_effective_limit = 0;
    bool g_prev_frame_limiter_enabled = false;
    std::string g_prev_frame_limiter_provider;
    bool g_prev_frame_limiter_provider_set = false;
    bool g_prev_disable_vsync = false;
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

    const bool provider_overridden = config::has_runtime_config_override("frame_limiter_provider");
    const bool rtss_sync_overridden = config::has_runtime_config_override("rtss_frame_limit_type");
    const auto configured_provider = parse_provider(config::frame_limiter.provider);
    const bool provider_explicit = configured_provider != frame_limiter_provider::auto_detect;
    const bool allow_gen1_rtss_override = !(provider_overridden && provider_explicit);

    // Gen1 fix: Force RTSS with front-edge sync (for DLSS3, FSR3, Lossless Scaling)
    // Respect explicit provider overrides so users can force NVCP.
    if (gen1_framegen_fix) {
      g_prev_frame_limiter_enabled = config::frame_limiter.enable;
      g_prev_frame_limiter_provider = config::frame_limiter.provider;
      g_prev_frame_limiter_provider_set = true;
      g_prev_disable_vsync = config::frame_limiter.disable_vsync;
      config::frame_limiter.enable = true;
      config::frame_limiter.disable_vsync = true;
      if (allow_gen1_rtss_override) {
        config::frame_limiter.provider = "rtss";
        if (!rtss_sync_overridden) {
          g_prev_rtss_frame_limit_type = config::rtss.frame_limit_type;
          g_prev_rtss_frame_limit_type_set = true;
          config::rtss.frame_limit_type = "front edge sync";
        } else {
          g_prev_rtss_frame_limit_type_set = false;
        }
      } else {
        g_prev_rtss_frame_limit_type_set = false;
      }
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

    const bool want_nv_vsync_override = (config::frame_limiter.disable_vsync || gen1_framegen_fix || gen2_framegen_fix) && nvidia_gpu_present && nvcp_ready;

    bool nvcp_already_invoked = false;
    int effective_limit = (lossless_rtss_limit && *lossless_rtss_limit > 0) ? *lossless_rtss_limit : fps;
    if (config::frame_limiter.fps_limit > 0) {
      effective_limit = config::frame_limiter.fps_limit;
    }
    g_last_effective_limit = effective_limit;

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
            false,
            want_nv_vsync_override,
            false,
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

    const bool want_disable_nv_frame_limit = g_active_provider == frame_limiter_provider::rtss && nvidia_gpu_present && nvcp_ready;

    if ((want_disable_nv_frame_limit || want_nv_vsync_override || want_smooth_motion) && !nvcp_already_invoked) {
      bool nvcp_result = frame_limiter_nvcp::streaming_start(
        effective_limit,
        false,
        want_disable_nv_frame_limit,
        want_nv_vsync_override,
        false,
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

  bool frame_limiter_prepare_launch(bool gen1_framegen_fix, bool gen2_framegen_fix, std::optional<int> lossless_rtss_limit) {
    const bool frame_limit_enabled = config::frame_limiter.enable || gen1_framegen_fix || gen2_framegen_fix || (lossless_rtss_limit && *lossless_rtss_limit > 0);
    if (!frame_limit_enabled || gen2_framegen_fix) {
      return false;
    }

    const bool rtss_available = rtss_is_configured();
    bool want_rtss = false;
    const bool provider_overridden = config::has_runtime_config_override("frame_limiter_provider");

    if (gen1_framegen_fix) {
      if (provider_overridden) {
        auto configured = parse_provider(config::frame_limiter.provider);
        switch (configured) {
          case frame_limiter_provider::rtss:
          case frame_limiter_provider::auto_detect:
            want_rtss = rtss_available;
            break;
          default:
            want_rtss = false;
            break;
        }
      } else {
        want_rtss = rtss_available;
      }
    } else {
      auto configured = parse_provider(config::frame_limiter.provider);
      switch (configured) {
        case frame_limiter_provider::rtss:
          want_rtss = rtss_available;
          break;
        case frame_limiter_provider::auto_detect:
          want_rtss = rtss_available;
          break;
        default:
          want_rtss = false;
          break;
      }
    }

    if (!want_rtss) {
      return false;
    }

    return rtss_warmup_process();
  }

  void frame_limiter_streaming_stop() {
    if (g_gen1_framegen_fix_active || g_gen2_framegen_fix_active) {
      config::frame_limiter.enable = g_prev_frame_limiter_enabled;
      if (g_prev_frame_limiter_provider_set) {
        config::frame_limiter.provider = g_prev_frame_limiter_provider;
      }
      if (g_gen1_framegen_fix_active) {
        config::frame_limiter.disable_vsync = g_prev_disable_vsync;
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
    g_last_effective_limit = 0;
  }

  void frame_limiter_streaming_refresh() {
    if (g_active_provider != frame_limiter_provider::rtss || g_last_effective_limit <= 0) {
      return;
    }

    if (rtss_streaming_refresh(g_last_effective_limit)) {
      BOOST_LOG(info) << "Frame limiter provider 'rtss' refreshed";
    }
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
    status.disable_vsync = config::frame_limiter.disable_vsync;
    status.nv_overrides_supported = status.nvidia_available && status.nvcp_ready;
    status.rtss = rtss_get_status();
    return status;
  }

}  // namespace platf

#endif  // _WIN32
