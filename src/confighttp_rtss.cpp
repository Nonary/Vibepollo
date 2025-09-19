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
    out["configured_path"] = rtss.configured_path;
    out["path_configured"] = rtss.path_configured;
    out["resolved_path"] = rtss.resolved_path;
    out["path_exists"] = rtss.path_exists;
    out["hooks_found"] = rtss.hooks_found;
    out["profile_found"] = rtss.profile_found;

    // A user-friendly message hinting required action
    std::string message;
    bool prefer_nvcp = fl.configured_provider == platf::frame_limiter_provider::nvidia_control_panel ||
                       (fl.configured_provider == platf::frame_limiter_provider::auto_detect && fl.nvcp_ready);
    bool prefer_rtss = fl.configured_provider == platf::frame_limiter_provider::rtss ||
                       (fl.configured_provider == platf::frame_limiter_provider::auto_detect && !prefer_nvcp);

    if (!fl.enabled) {
      message = "Frame limiter disabled; enable in settings to activate.";
    } else if (fl.active_provider == platf::frame_limiter_provider::nvidia_control_panel) {
      message = "NVIDIA Control Panel frame limiter active.";
    } else if (prefer_nvcp && fl.nvidia_available && fl.nvcp_ready) {
      message = "NVIDIA Control Panel detected; limiter will engage when streaming.";
    } else if (prefer_nvcp && !fl.nvidia_available) {
      message = "No NVIDIA GPU detected. Switch to RTSS or install NVIDIA drivers.";
    } else if (prefer_nvcp && !fl.nvcp_ready) {
      message = "NVIDIA Control Panel integration unavailable (NvAPI not ready).";
    } else if (prefer_rtss) {
      if (!rtss.path_exists) {
        message = "RTSS not found at resolved path. Install RTSS or adjust install path.";
      } else if (!rtss.hooks_found) {
        message = "RTSSHooks DLL not found. Install RTSS or correct path.";
      } else if (!rtss.profile_found) {
        message = "RTSS Global profile not found (Profiles/Global). Launch RTSS once or correct path.";
      } else {
        message = "RTSS detected.";
      }
    } else {
      message = "Frame limiter enabled but no provider active.";
    }
    out["message"] = message;

    send_response(response, out);
  }

}  // namespace confighttp

#endif  // _WIN32
