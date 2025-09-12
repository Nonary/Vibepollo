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
  #include "src/platform/windows/rtss_integration.h"

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
    auto st = platf::rtss_get_status();
    out["enabled"] = st.enabled;
    out["configured_path"] = st.configured_path;
    out["path_configured"] = st.path_configured;
    out["resolved_path"] = st.resolved_path;
    out["path_exists"] = st.path_exists;
    out["hooks_found"] = st.hooks_found;
    out["profile_found"] = st.profile_found;

    // A user-friendly message hinting required action
    std::string message;
    if (!st.enabled) {
      message = "RTSS limiter disabled; enable in settings to activate.";
    } else if (!st.path_exists) {
      message = "RTSS not found at resolved path. Install RTSS or adjust install path.";
    } else if (!st.hooks_found) {
      message = "RTSSHooks DLL not found. Install RTSS or correct path.";
    } else if (!st.profile_found) {
      message = "RTSS Global profile not found (Profiles/Global). Launch RTSS once or correct path.";
    } else {
      message = "RTSS detected.";
    }
    out["message"] = message;

    send_response(response, out);
  }

}  // namespace confighttp

#endif  // _WIN32
