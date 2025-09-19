/**
 * @file src/confighttp_rtss.cpp
 * @brief RTSS-specific HTTP endpoints (Windows-only).
 */

#ifdef _WIN32

  // standard includes
  #include <filesystem>
  #include <string>

  // third-party includes
  #include <nlohmann/json.hpp>
  #include <Simple-Web-Server/server_https.hpp>

  // local includes
  #include "confighttp.h"
  #include "src/logging.h"
  #include "src/platform/windows/frame_limiter.h"

namespace confighttp {

  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;

  // Forward declarations for helpers defined in confighttp.cpp
  bool authenticate(resp_https_t response, req_https_t request);
  void print_req(const req_https_t &request);
  void send_response(resp_https_t response, const nlohmann::json &output_tree);

  void getRtssStatus(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);

    nlohmann::json out;
    auto rtss = platf::rtss_get_status();
    auto fl = platf::frame_limiter_get_status();

    out["enabled"] = fl.enabled;
    out["configured_provider"] = platf::frame_limiter_provider_to_string(fl.configured_provider);
    out["active_provider"] = platf::frame_limiter_provider_to_string(fl.active_provider);
    out["nvidia_available"] = fl.nvidia_available;
    out["nvcp_ready"] = fl.nvcp_ready;
    out["rtss_available"] = fl.rtss_available;
    out["disable_vsync_ullm"] = fl.disable_vsync_ullm;
    out["nv_overrides_supported"] = fl.nv_overrides_supported;
    out["configured_path"] = rtss.configured_path;
    out["path_configured"] = rtss.path_configured;
    out["resolved_path"] = rtss.resolved_path;
    out["path_exists"] = rtss.path_exists;
    out["hooks_found"] = rtss.hooks_found;
    out["profile_found"] = rtss.profile_found;

    // A user-friendly message hinting required action
    std::string message;
    auto provider_to_string = [](platf::frame_limiter_provider provider) -> const char * {
      switch (provider) {
        case platf::frame_limiter_provider::nvidia_control_panel:
          return "nvidia-control-panel";
        case platf::frame_limiter_provider::rtss:
          return "rtss";
        case platf::frame_limiter_provider::auto_detect:
          return "auto";
        case platf::frame_limiter_provider::none:
        default:
          return "none";
      }
    };

    auto describe_provider = [&](const std::string &id) -> std::string {
      if (id == "nvidia-control-panel") {
        return "NVIDIA Control Panel";
      }
      if (id == "rtss") {
        return "RTSS";
      }
      if (id == "auto") {
        return "Auto";
      }
      return "None";
    };

    std::string provider_message;
    if (fl.enabled) {
      if (fl.active_provider == platf::frame_limiter_provider::nvidia_control_panel) {
        provider_message = "NVIDIA Control Panel frame limiter active.";
      } else if (fl.active_provider == platf::frame_limiter_provider::rtss) {
        provider_message = "RTSS frame limiter active.";
      } else {
        std::string configured = provider_to_string(fl.configured_provider);
        if (fl.configured_provider == platf::frame_limiter_provider::nvidia_control_panel) {
          if (!fl.nvidia_available) {
            provider_message = "No NVIDIA GPU detected. Switch to RTSS or install NVIDIA drivers.";
          } else if (!fl.nvcp_ready) {
            provider_message = "NVIDIA Control Panel integration unavailable (NvAPI not ready).";
          } else {
            provider_message = "NVIDIA Control Panel detected; limiter will engage when streaming.";
          }
        } else if (fl.configured_provider == platf::frame_limiter_provider::rtss || fl.configured_provider == platf::frame_limiter_provider::auto_detect) {
          if (!rtss.path_exists) {
            provider_message = "RTSS not found at resolved path. Install RTSS or adjust install path.";
          } else if (!rtss.hooks_found) {
            provider_message = "RTSSHooks DLL not found. Install RTSS or correct path.";
          } else if (!rtss.profile_found) {
            provider_message = "RTSS Global profile not found (Profiles/Global). Launch RTSS once or correct path.";
          } else {
            provider_message = std::string("Frame limiter configured for ") + describe_provider(configured) + "; awaiting next stream.";
          }
        } else {
          provider_message = "Frame limiter enabled but no provider active.";
        }
      }
    } else {
      provider_message = "Frame limiter disabled; enable in settings to activate.";
    }

    std::string override_message;
    if (fl.disable_vsync_ullm) {
      if (fl.nv_overrides_supported) {
        override_message = "NVIDIA overrides ready: VSYNC and Low Latency Mode will be forced off during streams.";
      } else if (fl.nvidia_available && !fl.nvcp_ready) {
        override_message = "NvAPI unavailable; Sunshine will fall back to refresh-rate overrides.";
      } else if (!fl.nvidia_available) {
        override_message = "Using refresh-rate override to mitigate VSYNC and Low Latency engagement.";
      }
    }

    if (!override_message.empty() && !provider_message.empty()) {
      message = provider_message + " " + override_message;
    } else if (!override_message.empty()) {
      message = override_message;
    } else {
      message = provider_message;
    }
    out["message"] = message;

    send_response(response, out);
  }

}  // namespace confighttp

#endif  // _WIN32
