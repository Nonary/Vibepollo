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
  #include <numeric>
  #include <optional>
  #include <string>
  #include <vector>

namespace platf {

  namespace {

    frame_limiter_provider g_active_provider = frame_limiter_provider::none;
    unsigned int g_stream_owner_count = 0;
    bool g_nvcp_started = false;
    bool g_gen1_framegen_fix_active = false;
    bool g_gen2_framegen_fix_active = false;
    bool g_prev_frame_limiter_enabled = false;
    std::string g_prev_frame_limiter_provider;
    bool g_prev_frame_limiter_provider_set = false;
    bool g_prev_disable_vsync = false;
    std::string g_prev_rtss_frame_limit_type;
    bool g_prev_rtss_frame_limit_type_set = false;
    int g_last_rtss_limit_value = 0;
    int g_last_rtss_limit_denominator = 1;

    struct rtss_limit_t {
      int value = 0;
      int denominator = 1;
    };

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

    bool has_amd_gpu() {
      for (const auto &gpu : enumerate_gpus()) {
        if (gpu.vendor_id == 0x1002 || gpu.vendor_id == 0x1022) {
          return true;
        }
      }
      return false;
    }

    std::string normalize_frame_generation_provider(const std::string &value) {
      std::string normalized;
      normalized.reserve(value.size());
      for (char ch : value) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
          normalized.push_back((char) std::tolower(static_cast<unsigned char>(ch)));
        }
      }
      return normalized;
    }

    std::string select_capture_fix_sync_limiter(const std::string &frame_generation_provider, bool nvidia_gpu_present, bool amd_gpu_present) {
      const auto normalized_provider = normalize_frame_generation_provider(frame_generation_provider);
      if (normalized_provider == "gameprovided" && nvidia_gpu_present && !amd_gpu_present) {
        return "nvidia reflex";
      }
      return "front edge sync";
    }

    int apply_stream_fps_offset(int requested_fps, int offset) {
      if (requested_fps <= 0 || offset <= 0) {
        return requested_fps;
      }

      return std::max(1, requested_fps - offset);
    }

    void reduce_rtss_limit(rtss_limit_t &limit) {
      if (limit.denominator <= 1 || limit.value <= 0) {
        limit.denominator = 1;
        return;
      }

      if (limit.value % 1000 == 0) {
        limit.value /= 1000;
        limit.denominator = 1;
      } else if (limit.value % 100 == 0) {
        limit.value /= 100;
        limit.denominator = 10;
      } else if (limit.value % 10 == 0) {
        limit.value /= 10;
        limit.denominator = 100;
      }

      int gcd_value = std::gcd(limit.value, limit.denominator);
      if (gcd_value > 1) {
        limit.value /= gcd_value;
        limit.denominator /= gcd_value;
      }
    }

    rtss_limit_t apply_stream_fps_offset_scaled(int requested_fps, int requested_fps_scaled, int offset) {
      if (requested_fps_scaled <= 0) {
        return {apply_stream_fps_offset(requested_fps, offset), 1};
      }

      const int scaled_offset = offset > 0 ? offset * 1000 : 0;
      int scaled_limit = requested_fps_scaled - scaled_offset;
      if (offset > 0) {
        scaled_limit = std::max(1000, scaled_limit);
      } else {
        scaled_limit = std::max(0, scaled_limit);
      }
      rtss_limit_t limit {
        scaled_limit,
        1000
      };
      reduce_rtss_limit(limit);
      return limit;
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

  void frame_limiter_streaming_start(int fps, int fps_scaled, bool gen1_framegen_fix, bool gen2_framegen_fix, std::optional<int> lossless_rtss_limit, const std::string &frame_generation_provider, bool smooth_motion) {
    if (g_stream_owner_count > 0) {
      ++g_stream_owner_count;
      BOOST_LOG(debug) << "Frame limiter start requested while already active; reusing existing overrides (owners=" << g_stream_owner_count << ")";
      return;
    }
    g_stream_owner_count = 1;
    g_active_provider = frame_limiter_provider::none;
    g_nvcp_started = false;
    g_gen1_framegen_fix_active = gen1_framegen_fix;
    g_gen2_framegen_fix_active = gen2_framegen_fix;

    const bool capture_fix_enabled = gen1_framegen_fix || gen2_framegen_fix;
    const bool frame_limit_enabled = config::frame_limiter.enable || capture_fix_enabled || (lossless_rtss_limit && *lossless_rtss_limit > 0);
    const bool nvidia_gpu_present = platf::has_nvidia_gpu();
    const bool amd_gpu_present = has_amd_gpu();
    const bool nvcp_ready = frame_limiter_nvcp::is_available();
    const bool want_smooth_motion = smooth_motion && nvidia_gpu_present;

    const bool provider_overridden = config::has_runtime_config_override("frame_limiter_provider");
    const bool rtss_sync_overridden = config::has_runtime_config_override("rtss_frame_limit_type");
    const auto configured_provider = parse_provider(config::frame_limiter.provider);
    const bool provider_explicit = configured_provider != frame_limiter_provider::auto_detect;
    const bool allow_capture_fix_rtss_override = !(provider_overridden && provider_explicit);

    // Frame generation capture fix: force RTSS unless the user explicitly selected a provider.
    if (capture_fix_enabled) {
      g_prev_frame_limiter_enabled = config::frame_limiter.enable;
      g_prev_frame_limiter_provider = config::frame_limiter.provider;
      g_prev_frame_limiter_provider_set = true;
      g_prev_disable_vsync = config::frame_limiter.disable_vsync;
      config::frame_limiter.enable = true;
      config::frame_limiter.disable_vsync = true;
      if (allow_capture_fix_rtss_override) {
        config::frame_limiter.provider = "rtss";
        if (!rtss_sync_overridden) {
          g_prev_rtss_frame_limit_type = config::rtss.frame_limit_type;
          g_prev_rtss_frame_limit_type_set = true;
          config::rtss.frame_limit_type = select_capture_fix_sync_limiter(frame_generation_provider, nvidia_gpu_present, amd_gpu_present);
        } else {
          g_prev_rtss_frame_limit_type_set = false;
        }
      } else {
        g_prev_rtss_frame_limit_type_set = false;
      }
    } else {
      g_prev_frame_limiter_provider_set = false;
      g_prev_rtss_frame_limit_type_set = false;
    }

    const bool want_nv_vsync_override = (config::frame_limiter.disable_vsync || capture_fix_enabled) && nvidia_gpu_present && nvcp_ready;

    bool nvcp_already_invoked = false;
    const bool using_lossless_rtss_limit = lossless_rtss_limit && *lossless_rtss_limit > 0;
    const bool using_manual_fps_limit = config::frame_limiter.fps_limit > 0;
    const bool using_stream_fps_limit = !using_lossless_rtss_limit && !using_manual_fps_limit;

    int effective_limit = using_lossless_rtss_limit ? *lossless_rtss_limit : fps;
    if (using_stream_fps_limit) {
      effective_limit = apply_stream_fps_offset(effective_limit, config::frame_limiter.fps_offset);
    }
    if (config::frame_limiter.fps_limit > 0) {
      effective_limit = config::frame_limiter.fps_limit;
    }

    int rtss_limit_value = effective_limit;
    int rtss_limit_denominator = 1;

    if (using_stream_fps_limit) {
      const auto rtss_limit = apply_stream_fps_offset_scaled(fps, fps_scaled, config::frame_limiter.fps_offset);
      rtss_limit_value = rtss_limit.value;
      rtss_limit_denominator = rtss_limit.denominator;
    }
    g_last_rtss_limit_value = rtss_limit_value;
    g_last_rtss_limit_denominator = rtss_limit_denominator;

    if (using_stream_fps_limit && config::frame_limiter.fps_offset > 0) {
      BOOST_LOG(info) << "Frame limiter FPS offset applied: requested=" << fps
                      << ", offset=" << config::frame_limiter.fps_offset
                      << ", effective=" << effective_limit;
    }

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
          bool ok = rtss_streaming_start(rtss_limit_value, rtss_limit_denominator);
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
    const bool capture_fix_enabled = gen1_framegen_fix || gen2_framegen_fix;
    const bool frame_limit_enabled = config::frame_limiter.enable || capture_fix_enabled || (lossless_rtss_limit && *lossless_rtss_limit > 0);
    if (!frame_limit_enabled) {
      return false;
    }

    const bool rtss_available = rtss_is_configured();
    bool want_rtss = false;
    const bool provider_overridden = config::has_runtime_config_override("frame_limiter_provider");

    if (capture_fix_enabled) {
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

  void frame_limiter_streaming_stop(bool keep_rtss_running) {
    if (g_stream_owner_count == 0) {
      return;
    }
    if (--g_stream_owner_count > 0) {
      BOOST_LOG(debug) << "Frame limiter stop deferred; remaining stream owners=" << g_stream_owner_count;
      return;
    }

    if (g_gen1_framegen_fix_active || g_gen2_framegen_fix_active) {
      config::frame_limiter.enable = g_prev_frame_limiter_enabled;
      if (g_prev_frame_limiter_provider_set) {
        config::frame_limiter.provider = g_prev_frame_limiter_provider;
      }
      config::frame_limiter.disable_vsync = g_prev_disable_vsync;
      if (g_prev_rtss_frame_limit_type_set) {
        config::rtss.frame_limit_type = g_prev_rtss_frame_limit_type;
      }
      g_gen1_framegen_fix_active = false;
      g_gen2_framegen_fix_active = false;
      g_prev_frame_limiter_provider_set = false;
      g_prev_rtss_frame_limit_type_set = false;
    }

    if (g_active_provider == frame_limiter_provider::rtss) {
      rtss_streaming_stop(keep_rtss_running);
    }

    if (g_nvcp_started || g_active_provider == frame_limiter_provider::nvidia_control_panel) {
      frame_limiter_nvcp::streaming_stop();
    }

    g_active_provider = frame_limiter_provider::none;
    g_nvcp_started = false;
    g_last_rtss_limit_value = 0;
    g_last_rtss_limit_denominator = 1;
  }

  void frame_limiter_streaming_refresh() {
    if (g_active_provider != frame_limiter_provider::rtss || g_last_rtss_limit_value <= 0) {
      return;
    }

    if (rtss_streaming_refresh(g_last_rtss_limit_value, g_last_rtss_limit_denominator)) {
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
