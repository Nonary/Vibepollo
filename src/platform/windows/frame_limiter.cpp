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

    bool start_with_provider(frame_limiter_provider provider, int fps) {
      switch (provider) {
        case frame_limiter_provider::nvidia_control_panel:
          return frame_limiter_nvcp::streaming_start(fps);
        case frame_limiter_provider::rtss:
          return rtss_streaming_start(fps);
        default:
          return false;
      }
    }

    bool has_nvidia_gpu() {
      constexpr std::uint32_t NVIDIA_VENDOR_ID = 0x10DE;
      for (const auto &gpu : platf::enumerate_gpus()) {
        if (gpu.vendor_id == NVIDIA_VENDOR_ID) {
          return true;
        }
      }
      return false;
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

    if (!config::frame_limiter.enable) {
      return;
    }

    auto configured = parse_provider(config::frame_limiter.provider);
    std::vector<frame_limiter_provider> order;

    switch (configured) {
      case frame_limiter_provider::none:
        return;
      case frame_limiter_provider::auto_detect:
        order = {
          frame_limiter_provider::nvidia_control_panel,
          frame_limiter_provider::rtss
        };
        break;
      default:
        order = {configured};
        break;
    }

    for (auto provider : order) {
      if (!provider_available(provider)) {
        BOOST_LOG(warning) << "Frame limiter provider '" << frame_limiter_provider_to_string(provider)
                           << "' not available";
        if (configured != frame_limiter_provider::auto_detect) {
          break;
        }
        continue;
      }
      if (start_with_provider(provider, fps)) {
        g_active_provider = provider;
        BOOST_LOG(info) << "Frame limiter provider '" << frame_limiter_provider_to_string(provider)
                        << "' applied";
        break;
      }
      BOOST_LOG(warning) << "Frame limiter provider '" << frame_limiter_provider_to_string(provider)
                         << "' failed to apply limit";
    }

    if (g_active_provider == frame_limiter_provider::none) {
      BOOST_LOG(warning) << "Frame limiter enabled but no provider applied";
    }
  }

  void frame_limiter_streaming_stop() {
    switch (g_active_provider) {
      case frame_limiter_provider::nvidia_control_panel:
        frame_limiter_nvcp::streaming_stop();
        break;
      case frame_limiter_provider::rtss:
        rtss_streaming_stop();
        break;
      default:
        break;
    }
    g_active_provider = frame_limiter_provider::none;
  }

  frame_limiter_provider frame_limiter_active_provider() {
    return g_active_provider;
  }

  frame_limiter_status_t frame_limiter_get_status() {
    frame_limiter_status_t status {};
    status.enabled = config::frame_limiter.enable;
    status.configured_provider = parse_provider(config::frame_limiter.provider);
    status.active_provider = g_active_provider;
    status.nvidia_available = has_nvidia_gpu();
    status.nvcp_ready = frame_limiter_nvcp::is_available();
    status.rtss_available = rtss_is_configured();
    status.rtss = rtss_get_status();
    return status;
  }

}  // namespace platf

#endif  // _WIN32
