/**
 * @file src/confighttp.cpp
 * @brief Definitions for the Web UI Config HTTP server.
 *
 * @todo Authentication, better handling of routes common to nvhttp, cleanup
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <algorithm>
#include <boost/regex.hpp>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>

// lib includes
#include <boost/algorithm/string.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/filesystem.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <nlohmann/json.hpp>
#include <Simple-Web-Server/crypto.hpp>
#include <Simple-Web-Server/server_https.hpp>

// local includes
#include "config.h"
#include "confighttp.h"
#include "crypto.h"
#include "file_handler.h"
#include "globals.h"
#include "http_auth.h"
#include "httpcommon.h"
#include "platform/common.h"
#ifdef _WIN32
  #include "src/platform/windows/image_convert.h"

#endif
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "webrtc_stream.h"

#include <nlohmann/json.hpp>
#if defined(_WIN32)
  #include "platform/windows/misc.h"
  #include "src/platform/windows/ipc/misc_utils.h"
  #include "src/platform/windows/playnite_integration.h"

  #include <windows.h>
#endif
#if defined(_WIN32)
  #include "platform/windows/misc.h"

  #include <KnownFolders.h>
  #include <ShlObj.h>
  #include <windows.h>
#endif
#include "display_helper_integration.h"
#include "process.h"
#include "utility.h"
#include "uuid.h"

// libdisplaydevice JSON usage is encapsulated in display_helper_integration

using namespace std::literals;
namespace pt = boost::property_tree;

namespace confighttp {
  // Global MIME type lookup used for static file responses
  const std::map<std::string, std::string> mime_types = {
    {"css", "text/css"},
    {"gif", "image/gif"},
    {"htm", "text/html"},
    {"html", "text/html"},
    {"ico", "image/x-icon"},
    {"jpeg", "image/jpeg"},
    {"jpg", "image/jpeg"},
    {"js", "application/javascript"},
    {"json", "application/json"},
    {"png", "image/png"},
    {"svg", "image/svg+xml"},
    {"ttf", "font/ttf"},
    {"txt", "text/plain"},
    {"woff2", "font/woff2"},
    {"xml", "text/xml"},
  };

  // Helper: sort apps by their 'name' field, if present
  static void sort_apps_by_name(nlohmann::json &file_tree) {
    try {
      if (!file_tree.contains("apps") || !file_tree["apps"].is_array()) {
        return;
      }
      auto &apps_node = file_tree["apps"];
      std::sort(apps_node.begin(), apps_node.end(), [](const nlohmann::json &a, const nlohmann::json &b) {
        try {
          return a.at("name").get<std::string>() < b.at("name").get<std::string>();
        } catch (...) {
          return false;
        }
      });
    } catch (...) {}
  }

  bool refresh_client_apps_cache(nlohmann::json &file_tree) {
    try {
      sort_apps_by_name(file_tree);
      file_handler::write_file(config::stream.file_apps.c_str(), file_tree.dump(4));
      proc::refresh(config::stream.file_apps);
      return true;
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "refresh_client_apps_cache: failed: " << e.what();
    } catch (...) {
      BOOST_LOG(warning) << "refresh_client_apps_cache: failed (unknown)";
    }
    return false;
  }
  namespace fs = std::filesystem;
  using enum confighttp::StatusCode;

  using https_server_t = SimpleWeb::Server<SimpleWeb::HTTPS>;

  using args_t = SimpleWeb::CaseInsensitiveMultimap;
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;

  // Forward declaration for error helper implemented later
  void bad_request(resp_https_t response, req_https_t request, const std::string &error_message);

#ifdef _WIN32
  // Forward declarations for Playnite handlers implemented in confighttp_playnite.cpp
  void getPlayniteStatus(std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request> request);
  void installPlaynite(std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request> request);
  void uninstallPlaynite(std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request> request);
  void getPlayniteGames(std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request> request);
  void getPlayniteCategories(std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request> request);
  void postPlayniteForceSync(std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request> request);
  void postPlayniteLaunch(std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request> request);
  // Helper to keep confighttp.cpp free of Playnite details
  void enhance_app_with_playnite_cover(nlohmann::json &input_tree);
  // New: download Playnite-related logs as a ZIP

  // RTSS status endpoint (Windows-only)
  void getRtssStatus(std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request> request);
  void getLosslessScalingStatus(std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request> request);
  void downloadPlayniteLogs(std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request> request);
  void getCrashDumpStatus(std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request> request);
  void postCrashDumpDismiss(std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request> request);
  void downloadCrashBundle(std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request> request);
  // Display helper: export current OS state as golden restore snapshot
  void postExportGoldenDisplay(resp_https_t response, req_https_t request);
#endif

  enum class op_e {
    ADD,  ///< Add client
    REMOVE  ///< Remove client
  };

  /**
   * @brief Log the request details.
   * @param request The HTTP request object.
   */
  void print_req(const req_https_t &request) {
    BOOST_LOG(debug) << "HTTP "sv << request->method << ' ' << request->path;

    if (!request->header.empty()) {
      BOOST_LOG(verbose) << "Headers:"sv;
      for (auto &[name, val] : request->header) {
        BOOST_LOG(verbose) << name << " -- "
                           << (name == "Authorization" ? "CREDENTIALS REDACTED" : val);
      }
    }

    auto query = request->parse_query_string();
    if (!query.empty()) {
      BOOST_LOG(verbose) << "Query Params:"sv;
      for (auto &[name, val] : query) {
        BOOST_LOG(verbose) << name << " -- " << val;
      }
    }
  }

  /**
   * @brief Get the CORS origin for localhost (no wildcard).
   * @return The CORS origin string.
   */
  static std::string get_cors_origin() {
    std::uint16_t https_port = net::map_port(PORT_HTTPS);
    return std::format("https://localhost:{}", https_port);
  }

  /**
   * @brief Helper to add CORS headers for API responses.
   * @param headers The headers to add CORS to.
   */
  void add_cors_headers(SimpleWeb::CaseInsensitiveMultimap &headers) {
    headers.emplace("Access-Control-Allow-Origin", get_cors_origin());
    headers.emplace("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    headers.emplace("Access-Control-Allow-Headers", "Content-Type, Authorization");
  }

  /**
   * @brief Send a response.
   * @param response The HTTP response object.
   * @param output_tree The JSON tree to send.
   */
  void send_response(resp_https_t response, const nlohmann::json &output_tree) {
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json; charset=utf-8");
    add_cors_headers(headers);
    response->write(success_ok, output_tree.dump(), headers);
  }

  nlohmann::json load_webrtc_ice_servers() {
    auto env = std::getenv("SUNSHINE_WEBRTC_ICE_SERVERS");
    if (!env || !*env) {
      return nlohmann::json::array();
    }

    try {
      auto parsed = nlohmann::json::parse(env);
      if (parsed.is_array()) {
        return parsed;
      }
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "WebRTC: invalid SUNSHINE_WEBRTC_ICE_SERVERS: "sv << e.what();
    }

    return nlohmann::json::array();
  }

  nlohmann::json webrtc_session_to_json(const webrtc_stream::SessionState &state) {
    nlohmann::json output;
    output["id"] = state.id;
    output["audio"] = state.audio;
    output["video"] = state.video;
    output["encoded"] = state.encoded;
    output["audio_packets"] = state.audio_packets;
    output["video_packets"] = state.video_packets;
    output["audio_dropped"] = state.audio_dropped;
    output["video_dropped"] = state.video_dropped;
    output["has_remote_offer"] = state.has_remote_offer;
    output["has_local_answer"] = state.has_local_answer;
    output["ice_candidates"] = state.ice_candidates;
    output["width"] = state.width ? nlohmann::json(*state.width) : nlohmann::json(nullptr);
    output["height"] = state.height ? nlohmann::json(*state.height) : nlohmann::json(nullptr);
    output["fps"] = state.fps ? nlohmann::json(*state.fps) : nlohmann::json(nullptr);
    output["bitrate_kbps"] = state.bitrate_kbps ? nlohmann::json(*state.bitrate_kbps) : nlohmann::json(nullptr);
    output["codec"] = state.codec ? nlohmann::json(*state.codec) : nlohmann::json(nullptr);
    output["hdr"] = state.hdr ? nlohmann::json(*state.hdr) : nlohmann::json(nullptr);
    output["audio_channels"] = state.audio_channels ? nlohmann::json(*state.audio_channels) : nlohmann::json(nullptr);
    output["audio_codec"] = state.audio_codec ? nlohmann::json(*state.audio_codec) : nlohmann::json(nullptr);
    output["profile"] = state.profile ? nlohmann::json(*state.profile) : nlohmann::json(nullptr);
    output["last_audio_bytes"] = state.last_audio_bytes;
    output["last_video_bytes"] = state.last_video_bytes;
    output["last_video_idr"] = state.last_video_idr;
    output["last_video_frame_index"] = state.last_video_frame_index;

    auto now = std::chrono::steady_clock::now();
    auto age_or_null = [&now](const std::optional<std::chrono::steady_clock::time_point> &tp) -> nlohmann::json {
      if (!tp) {
        return nullptr;
      }
      return std::chrono::duration_cast<std::chrono::milliseconds>(now - *tp).count();
    };

    output["last_audio_age_ms"] = age_or_null(state.last_audio_time);
    output["last_video_age_ms"] = age_or_null(state.last_video_time);
    return output;
  }

  /**
   * @brief Write an APIResponse to an HTTP response object.
   * @param response The HTTP response object.
   * @param api_response The APIResponse containing the structured response data.
   */
  void write_api_response(resp_https_t response, const APIResponse &api_response) {
    SimpleWeb::CaseInsensitiveMultimap headers = api_response.headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    add_cors_headers(headers);
    response->write(api_response.status_code, api_response.body, headers);
  }

  /**
   * @brief Send a 401 Unauthorized response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void send_unauthorized(resp_https_t response, req_https_t request) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    BOOST_LOG(info) << "Web UI: ["sv << address << "] -- not authorized"sv;

    constexpr auto code = client_error_unauthorized;

    nlohmann::json tree;
    tree["status_code"] = code;
    tree["status"] = false;
    tree["error"] = "Unauthorized";

    const SimpleWeb::CaseInsensitiveMultimap headers {
      {"Content-Type", "application/json"},
      {"X-Frame-Options", "DENY"},
      {"Content-Security-Policy", "frame-ancestors 'none';"},
      {"Access-Control-Allow-Origin", get_cors_origin()}
    };

    response->write(code, tree.dump(), headers);
  }

  /**
   * @brief Send a redirect response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param path The path to redirect to.
   */
  void send_redirect(resp_https_t response, req_https_t request, const char *path) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    BOOST_LOG(info) << "Web UI: ["sv << address << "] -- not authorized"sv;
    const SimpleWeb::CaseInsensitiveMultimap headers {
      {"Location", path},
      {"X-Frame-Options", "DENY"},
      {"Content-Security-Policy", "frame-ancestors 'none';"}
    };
    response->write(redirection_temporary_redirect, headers);
  }

  /**
   * @brief Check authentication and authorization for an HTTP request.
   * @param request The HTTP request object.
   * @return AuthResult with outcome and response details if not authorized.
   */
  AuthResult check_auth(const req_https_t &request) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    std::string auth_header;
    // Try Authorization header
    if (auto auth_it = request->header.find("authorization"); auth_it != request->header.end()) {
      auth_header = auth_it->second;
    } else {
      std::string token = extract_session_token_from_cookie(request->header);
      if (!token.empty()) {
        auth_header = "Session " + token;
      }
    }
    return check_auth(address, auth_header, request->path, request->method);
  }

  /**
   * @brief Authenticate the user or API token for a specific path/method.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @return True if authenticated and authorized, false otherwise.
   */
  bool authenticate(resp_https_t response, req_https_t request) {
    if (auto result = check_auth(request); !result.ok) {
      if (result.code == StatusCode::redirection_temporary_redirect) {
        response->write(result.code, result.headers);
      } else if (!result.body.empty()) {
        response->write(result.code, result.body, result.headers);
      } else {
        response->write(result.code);
      }
      return false;
    }
    return true;
  }

  /**
   * @brief Get the list of available display devices.
   * @api_examples{/api/display-devices| GET| [{"device_id":"{...}","display_name":"\\\\.\\DISPLAY1","friendly_name":"Monitor"}, ...]}
   * @note Pass query param detail=full to include extended metadata (refresh lists, inactive displays).
   */
  void getDisplayDevices(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    try {
      display_device::DeviceEnumerationDetail detail = display_device::DeviceEnumerationDetail::Minimal;
      const auto query = request->parse_query_string();
      if (const auto it = query.find("detail"); it != query.end()) {
        const auto value = boost::algorithm::to_lower_copy(it->second);
        if (value == "full") {
          detail = display_device::DeviceEnumerationDetail::Full;
        }
      } else if (const auto full_it = query.find("full"); full_it != query.end()) {
        const auto value = boost::algorithm::to_lower_copy(full_it->second);
        if (value == "1" || value == "true" || value == "yes") {
          detail = display_device::DeviceEnumerationDetail::Full;
        }
      }

      const auto json_str = display_helper_integration::enumerate_devices_json(detail);
      nlohmann::json tree = nlohmann::json::parse(json_str);
      send_response(response, tree);
    } catch (const std::exception &e) {
      nlohmann::json tree;
      tree["status"] = false;
      tree["error"] = std::string {"Failed to enumerate display devices: "} + e.what();
      send_response(response, tree);
    }
  }

#ifdef _WIN32
  /**
   * @brief Validate refresh capabilities for a display via EDID for frame generation health checks.
   * @api_examples{/api/framegen/edid-refresh?device_id=\\.\DISPLAY1| GET| {"status":true,"targets":[{"hz":120,"supported":true,"method":"range"}]}}
   */
  void getFramegenEdidRefresh(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    try {
      const auto query = request->parse_query_string();
      auto read_first = [&](std::initializer_list<std::string> keys) -> std::string {
        for (const auto &key : keys) {
          const auto it = query.find(key);
          if (it != query.end()) {
            auto value = boost::algorithm::trim_copy(it->second);
            if (!value.empty()) {
              return value;
            }
          }
        }
        return {};
      };

      std::string device_hint = read_first({"device_id", "device", "id", "display"});
      if (device_hint.empty()) {
        bad_request(response, request, "device_id query parameter is required");
        return;
      }

      std::vector<int> targets {120, 180, 240, 288};
      if (const auto it = query.find("targets"); it != query.end()) {
        std::vector<int> parsed;
        std::vector<std::string> parts;
        boost::split(parts, it->second, boost::is_any_of(","));
        for (auto part : parts) {
          boost::algorithm::trim(part);
          if (part.empty()) {
            continue;
          }
          try {
            int hz = std::stoi(part);
            if (hz > 0) {
              parsed.push_back(hz);
            }
          } catch (...) {
            // ignore invalid entries
          }
        }
        if (!parsed.empty()) {
          targets = std::move(parsed);
        }
      }

      auto result = display_helper_integration::framegen_edid_refresh_support(device_hint, targets);
      nlohmann::json out;
      if (!result) {
        out["status"] = false;
        out["error"] = "Display device not found for EDID refresh validation.";
        send_response(response, out);
        return;
      }

      out["status"] = true;
      out["device_id"] = result->device_id;
      out["device_label"] = result->device_label;
      out["edid_present"] = result->edid_present;
      if (result->max_vertical_hz) {
        out["max_vertical_hz"] = *result->max_vertical_hz;
      }
      if (result->max_timing_hz) {
        out["max_timing_hz"] = *result->max_timing_hz;
      }

      nlohmann::json targets_json = nlohmann::json::array();
      for (const auto &entry : result->targets) {
        nlohmann::json target_json;
        target_json["hz"] = entry.hz;
        target_json["supported"] = entry.supported.has_value() ? nlohmann::json(*entry.supported) : nlohmann::json(nullptr);
        target_json["method"] = entry.method;
        targets_json.push_back(std::move(target_json));
      }
      out["targets"] = std::move(targets_json);

      send_response(response, out);
    } catch (const std::exception &e) {
      bad_request(response, request, e.what());
    } catch (...) {
      bad_request(response, request, "Failed to validate display refresh via EDID.");
    }
  }

  /**
   * @brief Health check for ViGEm (Virtual Gamepad) installation on Windows.
   * @api_examples{/api/health/vigem| GET| {"installed":true,"version":"<hint>"}}
   */
  void getVigemHealth(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    try {
      std::string version;
      bool installed = platf::is_vigem_installed(&version);
      nlohmann::json out;
      out["installed"] = installed;
      if (!version.empty()) {
        out["version"] = version;
      }
      send_response(response, out);
    } catch (...) {
      bad_request(response, request, "Failed to evaluate ViGEm health");
    }
  }
#endif

  /**
   * @brief Send a 404 Not Found response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void not_found(resp_https_t response, [[maybe_unused]] req_https_t request) {
    constexpr auto code = client_error_not_found;

    nlohmann::json tree;
    tree["status_code"] = code;
    tree["error"] = "Not Found";

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("Access-Control-Allow-Origin", get_cors_origin());
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");

    response->write(code, tree.dump(), headers);
  }

  /**
   * @brief Send a 400 Bad Request response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param error_message The error message to include in the response.
   */
  void bad_request(resp_https_t response, [[maybe_unused]] req_https_t request, const std::string &error_message = "Bad Request") {
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    add_cors_headers(headers);
    nlohmann::json error = {{"error", error_message}};
    response->write(client_error_bad_request, error.dump(), headers);
  }

  /**
   * @brief Validate the request content type and send bad request when mismatch.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param contentType The expected content type
   */
  bool check_content_type(resp_https_t response, req_https_t request, const std::string_view &contentType) {
    auto requestContentType = request->header.find("content-type");
    if (requestContentType == request->header.end()) {
      bad_request(response, request, "Content type not provided");
      return false;
    }
    // Extract the media type part before any parameters (e.g., charset)
    std::string actualContentType = requestContentType->second;
    size_t semicolonPos = actualContentType.find(';');
    if (semicolonPos != std::string::npos) {
      actualContentType = actualContentType.substr(0, semicolonPos);
    }

    // Trim whitespace and convert to lowercase for case-insensitive comparison
    boost::algorithm::trim(actualContentType);
    boost::algorithm::to_lower(actualContentType);

    std::string expectedContentType(contentType);
    boost::algorithm::to_lower(expectedContentType);

    if (actualContentType != expectedContentType) {
      bad_request(response, request, "Content type mismatch");
      return false;
    }
    return true;
  }

  /**
   * @brief Send an HTTP redirect.
   */
  // Consolidated redirect helper: use the const char* variant below.

  /**
   * @brief SPA entry responder - serves the single-page app shell (index.html)
   * for any non-API and non-static-asset GET requests. Allows unauthenticated
   * access so the frontend can render login/first-run flows. Static and API
   * routes are expected to be registered explicitly; this function returns
   * a 404 for reserved prefixes to avoid accidentally exposing files.
   */
  void getSpaEntry(resp_https_t response, req_https_t request) {
    print_req(request);

    const std::string &p = request->path;
    // Reserved prefixes that should not be handled by the SPA entry
    static const std::vector<std::string> reserved = {"/api", "/assets", "/covers", "/images", "/images/"};
    for (const auto &r : reserved) {
      if (p.rfind(r, 0) == 0) {
        // Let explicit handlers or default not_found handle these
        not_found(response, request);
        return;
      }
    }

    // Serve the SPA shell (index.html) without server-side auth so frontend
    // can manage routing and authentication flows.
    std::string content = file_handler::read_file(WEB_DIR "index.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  // legacy per-page handlers removed; SPA entry handles these routes

  /**
   * @brief Get the favicon image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @todo combine function with getSunshineLogoImage and possibly getNodeModules
   * @todo use mime_types map
   */
  void getFaviconImage(resp_https_t response, req_https_t request) {
    print_req(request);

    std::ifstream in(WEB_DIR "images/sunshine.ico", std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/x-icon");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(success_ok, in, headers);
  }

  /**
   * @brief Get the Sunshine logo image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @todo combine function with getFaviconImage and possibly getNodeModules
   * @todo use mime_types map
   */
  void getSunshineLogoImage(resp_https_t response, req_https_t request) {
    print_req(request);

    std::ifstream in(WEB_DIR "images/logo-sunshine-45.png", std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/png");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(success_ok, in, headers);
  }

  /**
   * @brief Check if a path is a child of another path.
   * @param base The base path.
   * @param query The path to check.
   * @return True if the path is a child of the base path, false otherwise.
   */
  bool isChildPath(fs::path const &base, fs::path const &query) {
    auto relPath = fs::relative(base, query);
    return *(relPath.begin()) != fs::path("..");
  }

  /**
   * @brief Get an asset from the node_modules directory.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getNodeModules(resp_https_t response, req_https_t request) {
    print_req(request);
    fs::path webDirPath(WEB_DIR);
    fs::path nodeModulesPath(webDirPath / "assets");

    // .relative_path is needed to shed any leading slash that might exist in the request path
    auto filePath = fs::weakly_canonical(webDirPath / fs::path(request->path).relative_path());

    // Don't do anything if file does not exist or is outside the assets directory
    if (!isChildPath(filePath, nodeModulesPath)) {
      BOOST_LOG(warning) << "Someone requested a path " << filePath << " that is outside the assets folder";
      bad_request(response, request);
      return;
    }
    if (!fs::exists(filePath)) {
      not_found(response, request);
      return;
    }

    auto relPath = fs::relative(filePath, webDirPath);
    // get the mime type from the file extension mime_types map
    // remove the leading period from the extension
    auto mimeType = mime_types.find(relPath.extension().string().substr(1));
    // check if the extension is in the map at the x position
    if (mimeType == mime_types.end()) {
      bad_request(response, request);
      return;
    }

    // if it is, set the content type to the mime type
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", mimeType->second);
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    std::ifstream in(filePath.string(), std::ios::binary);
    response->write(success_ok, in, headers);
  }

  /**
   * @brief Get the list of available applications.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps| GET| null}
   */
  void getApps(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::string content = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json file_tree = nlohmann::json::parse(content);

#ifdef _WIN32
      // No auto-insert here; controlled by config 'playnite_fullscreen_entry_enabled'.
#endif

      // Legacy versions of Sunshine used strings for boolean and integers, let's convert them
      // List of keys to convert to boolean
      std::vector<std::string> boolean_keys = {
        "exclude-global-prep-cmd",
        "elevated",
        "auto-detach",
        "wait-all",
        "gen1-framegen-fix",
        "gen2-framegen-fix",
        "dlss-framegen-capture-fix",  // backward compatibility
        "lossless-scaling-framegen"
      };

      // List of keys to convert to integers
      std::vector<std::string> integer_keys = {
        "exit-timeout",
        "lossless-scaling-target-fps",
        "lossless-scaling-rtss-limit"
      };

      bool mutated = false;
      auto normalize_lossless_profile_overrides = [](nlohmann::json &node) -> bool {
        if (!node.is_object()) {
          return false;
        }
        bool changed = false;
        auto convert_int = [&](const char *key) {
          if (!node.contains(key)) {
            return;
          }
          auto &value = node[key];
          if (value.is_string()) {
            try {
              value = std::stoi(value.get<std::string>());
              changed = true;
            } catch (...) {
            }
          }
        };
        auto convert_bool = [&](const char *key) {
          if (!node.contains(key)) {
            return;
          }
          auto &value = node[key];
          if (value.is_string()) {
            auto text = value.get<std::string>();
            if (text == "true" || text == "false") {
              value = (text == "true");
              changed = true;
            } else if (text == "1" || text == "0") {
              value = (text == "1");
              changed = true;
            }
          }
        };
        convert_bool("performance-mode");
        convert_int("flow-scale");
        convert_int("resolution-scale");
        convert_int("sharpening");
        convert_bool("anime4k-vrs");
        if (node.contains("scaling-type") && node["scaling-type"].is_string()) {
          auto text = node["scaling-type"].get<std::string>();
          boost::algorithm::to_lower(text);
          node["scaling-type"] = text;
          changed = true;
        }
        if (node.contains("anime4k-size") && node["anime4k-size"].is_string()) {
          auto text = node["anime4k-size"].get<std::string>();
          boost::algorithm::to_upper(text);
          node["anime4k-size"] = text;
          changed = true;
        }
        return changed;
      };
      // Walk fileTree and convert true/false strings to boolean or integer values
      for (auto &app : file_tree["apps"]) {
        for (const auto &key : boolean_keys) {
          if (app.contains(key) && app[key].is_string()) {
            app[key] = app[key] == "true";
            mutated = true;
          }
        }
        for (const auto &key : integer_keys) {
          if (app.contains(key) && app[key].is_string()) {
            app[key] = std::stoi(app[key].get<std::string>());
            mutated = true;
          }
        }
        if (app.contains("lossless-scaling-recommended")) {
          mutated = normalize_lossless_profile_overrides(app["lossless-scaling-recommended"]) || mutated;
        }
        if (app.contains("lossless-scaling-custom")) {
          mutated = normalize_lossless_profile_overrides(app["lossless-scaling-custom"]) || mutated;
        }
        if (app.contains("prep-cmd")) {
          for (auto &prep : app["prep-cmd"]) {
            if (prep.contains("elevated") && prep["elevated"].is_string()) {
              prep["elevated"] = prep["elevated"] == "true";
              mutated = true;
            }
          }
        }
        // Ensure each app has a UUID (auto-insert if missing/empty)
        if (!app.contains("uuid") || app["uuid"].is_null() || (app["uuid"].is_string() && app["uuid"].get<std::string>().empty())) {
          app["uuid"] = uuid_util::uuid_t::generate().string();
          mutated = true;
        }
      }

      // Add computed app ids for UI clients (best-effort, do not persist).
      if (file_tree.contains("apps") && file_tree["apps"].is_array()) {
        try {
          const auto apps_snapshot = proc::proc.get_apps();
          const auto count = std::min(file_tree["apps"].size(), apps_snapshot.size());
          for (size_t idx = 0; idx < count; ++idx) {
            auto &app = file_tree["apps"][idx];
            app["id"] = apps_snapshot[idx].id;
            app["index"] = static_cast<int>(idx);
          }
        } catch (...) {
        }
      }

      // If any normalization occurred, persist back to disk
      if (mutated) {
        try {
          file_handler::write_file(config::stream.file_apps.c_str(), file_tree.dump(4));
        } catch (std::exception &e) {
          BOOST_LOG(warning) << "GetApps persist normalization failed: "sv << e.what();
        }
      }

      send_response(response, file_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "GetApps: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Serve a specific application's cover image by UUID.
   *        Looks for files named @c uuid with a supported image extension in the covers directory.
   * @api_examples{/api/apps/@c uuid/cover| GET| null}
   */
  void getAppCover(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    std::string uuid;
    if (request->path_match.size() > 1) {
      uuid = request->path_match[1];
    }
    if (uuid.empty()) {
      bad_request(response, request, "Missing application uuid");
      return;
    }

    const fs::path cover_dir = platf::appdata() / "covers";
    const std::vector<std::string> extensions = {".png", ".jpg", ".jpeg"};
    fs::path cover_path;
    for (const auto &ext : extensions) {
      fs::path candidate = cover_dir / (uuid + ext);
      if (fs::exists(candidate)) {
        cover_path = std::move(candidate);
        break;
      }
    }

    if (cover_path.empty()) {
      // Fallback to image-path from apps config if present
      try {
        std::string content = file_handler::read_file(config::stream.file_apps.c_str());
        nlohmann::json file_tree = nlohmann::json::parse(content);
        if (file_tree.contains("apps") && file_tree["apps"].is_array()) {
          for (const auto &app : file_tree["apps"]) {
            if (!app.contains("uuid") || !app["uuid"].is_string()) {
              continue;
            }
            if (app["uuid"].get<std::string>() != uuid) {
              continue;
            }
            if (app.contains("image-path") && app["image-path"].is_string()) {
              std::string raw_path = app["image-path"].get<std::string>();
              boost::algorithm::trim(raw_path);
              if (!raw_path.empty() && raw_path.front() == '"' && raw_path.back() == '"') {
                raw_path = raw_path.substr(1, raw_path.size() - 2);
              }
              constexpr std::string_view file_prefix = "file://";
              if (raw_path.rfind(file_prefix, 0) == 0) {
                raw_path.erase(0, file_prefix.size());
                if (!raw_path.empty() && raw_path.front() == '/') {
                  raw_path.erase(0, 1);
                }
              }
#ifdef _WIN32
              const char *appdata = std::getenv("APPDATA");
              if (appdata) {
                const std::string key = "%APPDATA%";
                auto pos = raw_path.find(key);
                if (pos != std::string::npos) {
                  raw_path.replace(pos, key.size(), appdata);
                }
              }
              const char *userprofile = std::getenv("USERPROFILE");
              if (userprofile) {
                const std::string key = "%USERPROFILE%";
                auto pos = raw_path.find(key);
                if (pos != std::string::npos) {
                  raw_path.replace(pos, key.size(), userprofile);
                }
              }
#endif
              fs::path candidate = raw_path;
              if (candidate.is_relative()) {
                if (app.contains("working-dir") && app["working-dir"].is_string()) {
                  fs::path working_dir = app["working-dir"].get<std::string>();
                  candidate = working_dir / candidate;
                } else {
                  candidate = cover_dir / candidate;
                }
              }
              if (!candidate.has_extension()) {
                auto with_png = candidate;
                with_png += ".png";
                if (fs::exists(with_png)) {
                  cover_path = std::move(with_png);
                  break;
                }
              }
              if (fs::exists(candidate)) {
                cover_path = std::move(candidate);
                break;
              }
            }
            break;
          }
        }
      } catch (...) {
      }
    }

    if (cover_path.empty()) {
      not_found(response, request);
      return;
    }

    auto ext = cover_path.extension().string();
    if (!ext.empty() && ext.front() == '.') {
      ext.erase(0, 1);
    }
    boost::algorithm::to_lower(ext);

    auto mimeType = mime_types.find(ext);
    if (mimeType == mime_types.end()) {
      bad_request(response, request);
      return;
    }

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", mimeType->second);
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    std::ifstream in(cover_path.string(), std::ios::binary);
    if (!in) {
      not_found(response, request);
      return;
    }
    response->write(success_ok, in, headers);
  }

  /**
   * @brief Save an application. To save a new application the index must be `-1`. To update an existing application, you must provide the current index of the application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the post request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "name": "Application Name",
   *   "output": "Log Output Path",
   *   "cmd": "Command to run the application",
   *   "index": -1,
   *   "exclude-global-prep-cmd": false,
   *   "elevated": false,
   *   "auto-detach": true,
   *   "wait-all": true,
   *   "exit-timeout": 5,
   *   "prep-cmd": [
   *     {
   *       "do": "Command to prepare",
   *       "undo": "Command to undo preparation",
   *       "elevated": false
   *     }
   *   ],
   *   "detached": [
   *     "Detached command"
   *   ],
   *   "image-path": "Full path to the application image. Must be a png file."
   * }
   * @endcode
   *
   * @api_examples{/api/apps| POST| {"name":"Hello, World!","index":-1}}
   */
  void saveApp(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      // TODO: Input Validation
      nlohmann::json output_tree;
      nlohmann::json input_tree = nlohmann::json::parse(ss);
      std::string file = file_handler::read_file(config::stream.file_apps.c_str());
      BOOST_LOG(info) << file;
      nlohmann::json file_tree = nlohmann::json::parse(file);

      if (input_tree["prep-cmd"].empty()) {
        input_tree.erase("prep-cmd");
      }

      if (input_tree["detached"].empty()) {
        input_tree.erase("detached");
      }

      // If image-path omitted but we have a Playnite id, let Playnite helper resolve a cover (Windows)
#ifdef _WIN32
      enhance_app_with_playnite_cover(input_tree);
#endif

#ifndef _WIN32
      if ((input_tree.contains("gen1-framegen-fix") && input_tree["gen1-framegen-fix"].is_boolean() && input_tree["gen1-framegen-fix"].get<bool>()) ||
          (input_tree.contains("dlss-framegen-capture-fix") && input_tree["dlss-framegen-capture-fix"].is_boolean() && input_tree["dlss-framegen-capture-fix"].get<bool>())) {
        bad_request(response, request, "Frame generation capture fixes are only supported on Windows hosts.");
        return;
      }
      if (input_tree.contains("gen2-framegen-fix") && input_tree["gen2-framegen-fix"].is_boolean() && input_tree["gen2-framegen-fix"].get<bool>()) {
        bad_request(response, request, "Frame generation capture fixes are only supported on Windows hosts.");
        return;
      }
#else
      // Migrate old field name to new for backward compatibility
      if (input_tree.contains("dlss-framegen-capture-fix") && !input_tree.contains("gen1-framegen-fix")) {
        input_tree["gen1-framegen-fix"] = input_tree["dlss-framegen-capture-fix"];
      }
      // Remove old field to avoid duplication
      input_tree.erase("dlss-framegen-capture-fix");
#endif

      auto &apps_node = file_tree["apps"];
      int index = input_tree["index"].get<int>();  // this will intentionally cause exception if the provided value is the wrong type

      input_tree.erase("index");

      if (index == -1) {
        // New app: generate a UUID if not provided
        if (!input_tree.contains("uuid") || input_tree["uuid"].is_null() || (input_tree["uuid"].is_string() && input_tree["uuid"].get<std::string>().empty())) {
          input_tree["uuid"] = uuid_util::uuid_t::generate().string();
        }
        apps_node.push_back(input_tree);
      } else {
        nlohmann::json newApps = nlohmann::json::array();
        for (size_t i = 0; i < apps_node.size(); ++i) {
          if (i == index) {
            // Preserve existing UUID if present
            try {
              if ((!input_tree.contains("uuid") || input_tree["uuid"].is_null() || (input_tree["uuid"].is_string() && input_tree["uuid"].get<std::string>().empty())) &&
                  apps_node[i].contains("uuid") && apps_node[i]["uuid"].is_string()) {
                input_tree["uuid"] = apps_node[i]["uuid"].get<std::string>();
              }
            } catch (...) {}
            newApps.push_back(input_tree);
          } else {
            newApps.push_back(apps_node[i]);
          }
        }
        file_tree["apps"] = newApps;
      }

      // Update apps file and refresh client cache
      confighttp::refresh_client_apps_cache(file_tree);

      output_tree["status"] = true;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SaveApp: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Serve a specific application's cover image by UUID.
   *        Looks for files named @c uuid with a supported image extension in the covers directory.
   * @api_examples{/api/apps/@c uuid/cover| GET| null}
   */

  /**
   * @brief Upload or set a specific application's cover image by UUID.
   *        Accepts either a JSON body with {"url": "..."} (restricted to images.igdb.com) or {"data": base64}.
   *        Saves to appdata/covers/@c uuid.@c ext where ext is derived from URL or defaults to .png for data.
   * @api_examples{/api/apps/@c uuid/cover| POST| {"url":"https://images.igdb.com/.../abc.png"}}
   */

  /**
   * @brief Close the currently running application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps/close| POST| null}
   */
  void closeApp(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    proc::proc.terminate();

    nlohmann::json output_tree;
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Delete an application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps/9999| DELETE| null}
   */
  void deleteApp(resp_https_t response, req_https_t request) {
    // Skip check_content_type() for this endpoint since the request body is not used.

    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      nlohmann::json output_tree;
      nlohmann::json new_apps = nlohmann::json::array();
      std::string file = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json file_tree = nlohmann::json::parse(file);
      auto &apps_node = file_tree["apps"];
      const int index = std::stoi(request->path_match[1]);

      if (index < 0 || index >= static_cast<int>(apps_node.size())) {
        std::string error;
        if (const int max_index = static_cast<int>(apps_node.size()) - 1; max_index < 0) {
          error = "No applications to delete";
        } else {
          error = std::format("'index' {} out of range, max index is {}", index, max_index);
        }
        bad_request(response, request, error);
        return;
      }

      // Detect if the app being removed is the Playnite fullscreen launcher
      auto is_playnite_fullscreen = [](const nlohmann::json &app) -> bool {
        try {
          if (app.contains("playnite-fullscreen") && app["playnite-fullscreen"].is_boolean() && app["playnite-fullscreen"].get<bool>()) {
            return true;
          }
          if (app.contains("cmd") && app["cmd"].is_string()) {
            auto s = app["cmd"].get<std::string>();
            if (s.find("playnite-launcher") != std::string::npos && s.find("--fullscreen") != std::string::npos) {
              return true;
            }
          }
          if (app.contains("name") && app["name"].is_string() && app["name"].get<std::string>() == "Playnite (Fullscreen)") {
            return true;
          }
        } catch (...) {}
        return false;
      };

      bool disabled_fullscreen_flag = false;
      for (size_t i = 0; i < apps_node.size(); ++i) {
        if (i != index) {
          new_apps.push_back(apps_node[i]);
        } else {
          // If user deletes the Playnite fullscreen app, turn off the config flag
#ifdef _WIN32
          try {
            if (is_playnite_fullscreen(apps_node[i])) {
              auto current_cfg = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
              current_cfg["playnite_fullscreen_entry_enabled"] = "false";
              std::stringstream config_stream;
              for (const auto &kv : current_cfg) {
                config_stream << kv.first << " = " << kv.second << std::endl;
              }
              file_handler::write_file(config::sunshine.config_file.c_str(), config_stream.str());
              config::apply_config_now();
              disabled_fullscreen_flag = true;
            }
          } catch (...) {
          }
#endif
        }
      }
      file_tree["apps"] = new_apps;

      file_handler::write_file(config::stream.file_apps.c_str(), file_tree.dump(4));
      proc::refresh(config::stream.file_apps);

      output_tree["status"] = true;
      output_tree["result"] = std::format("application {} deleted", index);
      if (disabled_fullscreen_flag) {
        output_tree["playniteFullscreenDisabled"] = true;
      }
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "DeleteApp: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Get the list of paired clients.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/clients/list| GET| null}
   */
  void getClients(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    const nlohmann::json named_certs = nvhttp::get_all_clients();

    nlohmann::json output_tree;
    output_tree["named_certs"] = named_certs;
    output_tree["status"] = true;
    output_tree["platform"] = SUNSHINE_PLATFORM;
    send_response(response, output_tree);
  }

#ifdef _WIN32
  static std::optional<uint64_t> file_creation_time_ms(const std::filesystem::path &path) {
    WIN32_FILE_ATTRIBUTE_DATA data {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
      return std::nullopt;
    }
    ULARGE_INTEGER t {};
    t.LowPart = data.ftCreationTime.dwLowDateTime;
    t.HighPart = data.ftCreationTime.dwHighDateTime;

    // FILETIME is in 100ns units since 1601-01-01.
    constexpr uint64_t kEpochDiff100ns = 116444736000000000ULL;  // 1970-01-01 - 1601-01-01
    if (t.QuadPart < kEpochDiff100ns) {
      return std::nullopt;
    }
    return (t.QuadPart - kEpochDiff100ns) / 10000ULL;
  }

  static std::filesystem::path windows_color_profile_dir() {
    wchar_t system_root[MAX_PATH] = {};
    if (GetSystemWindowsDirectoryW(system_root, _countof(system_root)) == 0) {
      return std::filesystem::path(L"C:\\Windows\\System32\\spool\\drivers\\color");
    }
    std::filesystem::path root(system_root);
    return root / L"System32" / L"spool" / L"drivers" / L"color";
  }
#endif

  /**
   * @brief Get a list of available HDR color profiles (Windows only).
   *
   * @api_examples{/api/clients/hdr-profiles| GET| null}
   */
  void getHdrProfiles(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = true;
    nlohmann::json profiles = nlohmann::json::array();

#ifdef _WIN32
    try {
      const auto dir = windows_color_profile_dir();
      struct entry_t {
        std::string filename;
        uint64_t added_ms;
      };
      std::vector<entry_t> entries;
      for (const auto &entry : std::filesystem::directory_iterator(dir)) {
        std::error_code ec;
        if (!entry.is_regular_file(ec)) {
          continue;
        }

        auto ext = entry.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        if (ext != L".icm" && ext != L".icc") {
          continue;
        }

        const auto filename_utf8 = platf::to_utf8(entry.path().filename().wstring());
        const auto added_ms = file_creation_time_ms(entry.path()).value_or(0);
        entries.push_back({filename_utf8, added_ms});
      }

      std::sort(entries.begin(), entries.end(), [](const entry_t &a, const entry_t &b) {
        if (a.added_ms != b.added_ms) {
          return a.added_ms > b.added_ms;
        }
        return a.filename < b.filename;
      });

      for (const auto &e : entries) {
        nlohmann::json node;
        node["filename"] = e.filename;
        node["added_ms"] = e.added_ms;
        profiles.push_back(std::move(node));
      }
    } catch (const std::exception &e) {
      output_tree["status"] = false;
      output_tree["error"] = e.what();
    } catch (...) {
      output_tree["status"] = false;
      output_tree["error"] = "unknown error";
    }
#endif

    output_tree["profiles"] = std::move(profiles);
    send_response(response, output_tree);
  }

#ifdef _WIN32
  // removed unused forward declaration for default_playnite_ext_dir()
#endif

  /**
   * @brief Update stored settings for a paired client.
   */
  void updateClient(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();

    try {
      const nlohmann::json input_tree = nlohmann::json::parse(ss);
      nlohmann::json output_tree;

      const std::string uuid = input_tree.value("uuid", "");

      std::optional<std::string> hdr_profile;
      if (input_tree.contains("hdr_profile")) {
        if (input_tree["hdr_profile"].is_null()) {
          hdr_profile = std::string {};
        } else {
          hdr_profile = input_tree.value("hdr_profile", "");
        }
      }

      const bool has_extended_fields =
        input_tree.contains("name") ||
        input_tree.contains("display_mode") ||
        input_tree.contains("output_name_override") ||
        input_tree.contains("always_use_virtual_display") ||
        input_tree.contains("virtual_display_mode") ||
        input_tree.contains("virtual_display_layout") ||
        input_tree.contains("config_overrides") ||
        input_tree.contains("prefer_10bit_sdr");

      if (!has_extended_fields) {
        output_tree["status"] = nvhttp::set_client_hdr_profile(uuid, hdr_profile.value_or(""));
        send_response(response, output_tree);
        return;
      }

      const std::string name = input_tree.value("name", "");
      const std::string display_mode = input_tree.value("display_mode", "");
      const std::string output_name_override = input_tree.value("output_name_override", "");
      const bool always_use_virtual_display = input_tree.value("always_use_virtual_display", false);
      const std::string virtual_display_mode = input_tree.value("virtual_display_mode", "");
      const std::string virtual_display_layout = input_tree.value("virtual_display_layout", "");

      std::optional<std::unordered_map<std::string, std::string>> config_overrides;
      if (input_tree.contains("config_overrides")) {
        if (input_tree["config_overrides"].is_null()) {
          config_overrides = std::unordered_map<std::string, std::string> {};
        } else if (input_tree["config_overrides"].is_object()) {
          std::unordered_map<std::string, std::string> overrides;
          for (const auto &item : input_tree["config_overrides"].items()) {
            const std::string &key = item.key();
            const auto &val = item.value();
            if (key.empty() || val.is_null()) {
              continue;
            }
            std::string encoded;
            if (val.is_string()) {
              encoded = val.get<std::string>();
            } else {
              encoded = val.dump();
            }
            overrides.emplace(key, std::move(encoded));
          }
          config_overrides = std::move(overrides);
        }
      }

      std::optional<bool> prefer_10bit_sdr;
      if (input_tree.contains("prefer_10bit_sdr") && !input_tree["prefer_10bit_sdr"].is_null()) {
        prefer_10bit_sdr = input_tree["prefer_10bit_sdr"].get<bool>();
      } else {
        prefer_10bit_sdr.reset();
      }

      output_tree["status"] = nvhttp::update_device_info(
        uuid,
        name,
        display_mode,
        output_name_override,
        always_use_virtual_display,
        virtual_display_mode,
        virtual_display_layout,
        std::move(config_overrides),
        prefer_10bit_sdr,
        hdr_profile
      );
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "UpdateClient: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Disconnect a client session without unpairing it.
   */
  void disconnectClient(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();

    try {
      const nlohmann::json input_tree = nlohmann::json::parse(ss);
      nlohmann::json output_tree;
      const std::string uuid = input_tree.value("uuid", "");
      output_tree["status"] = nvhttp::disconnect_client(uuid);
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "DisconnectClient: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Unpair a client.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the post request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *  "uuid": "@c uuid"
   * }
   * @endcode
   *
   * @api_examples{/api/unpair| POST| {"uuid":"1234"}}
   */
  void unpair(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();

    try {
      // TODO: Input Validation
      nlohmann::json output_tree;
      const nlohmann::json input_tree = nlohmann::json::parse(ss);
      const std::string uuid = input_tree.value("uuid", "");
      output_tree["status"] = nvhttp::unpair_client(uuid);
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "Unpair: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Unpair all clients.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/clients/unpair-all| POST| null}
   */
  void unpairAll(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    nvhttp::erase_all_clients();
    proc::proc.terminate();

    nlohmann::json output_tree;
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Get the configuration settings.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/config| GET| null}
   */
  void getConfig(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = true;

    auto vars = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));

    for (auto &[name, value] : vars) {
      output_tree[name] = std::move(value);
    }

    send_response(response, output_tree);
  }

  /**
   * @brief Get immutables metadata about the server.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/meta| GET| null}
   */
  void getMetadata(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["platform"] = SUNSHINE_PLATFORM;
    output_tree["version"] = PROJECT_VERSION;
    output_tree["commit"] = PROJECT_VERSION_COMMIT;
#ifdef PROJECT_VERSION_PRERELEASE
    output_tree["prerelease"] = PROJECT_VERSION_PRERELEASE;
#else
    output_tree["prerelease"] = "";
#endif
#ifdef PROJECT_VERSION_BRANCH
    output_tree["branch"] = PROJECT_VERSION_BRANCH;
#else
    output_tree["branch"] = "unknown";
#endif
    // Build/release date provided by CMake (ISO 8601 when available)
    output_tree["release_date"] = PROJECT_RELEASE_DATE;
#if defined(_WIN32)
    try {
      const auto gpus = platf::enumerate_gpus();
      if (!gpus.empty()) {
        nlohmann::json gpu_array = nlohmann::json::array();
        bool has_nvidia = false;
        bool has_amd = false;
        bool has_intel = false;

        for (const auto &gpu : gpus) {
          nlohmann::json gpu_entry;
          gpu_entry["description"] = gpu.description;
          gpu_entry["vendor_id"] = gpu.vendor_id;
          gpu_entry["device_id"] = gpu.device_id;
          gpu_entry["dedicated_video_memory"] = gpu.dedicated_video_memory;
          gpu_array.push_back(std::move(gpu_entry));

          switch (gpu.vendor_id) {
            case 0x10DE:  // NVIDIA
              has_nvidia = true;
              break;
            case 0x1002:  // AMD/ATI
            case 0x1022:  // AMD alternative PCI vendor ID (APUs)
              has_amd = true;
              break;
            case 0x8086:  // Intel
              has_intel = true;
              break;
            default:
              break;
          }
        }

        output_tree["gpus"] = std::move(gpu_array);
        output_tree["has_nvidia_gpu"] = has_nvidia;
        output_tree["has_amd_gpu"] = has_amd;
        output_tree["has_intel_gpu"] = has_intel;
      }

      const auto version = platf::query_windows_version();
      if (!version.display_version.empty()) {
        output_tree["windows_display_version"] = version.display_version;
      }
      if (!version.release_id.empty()) {
        output_tree["windows_release_id"] = version.release_id;
      }
      if (!version.product_name.empty()) {
        output_tree["windows_product_name"] = version.product_name;
      }
      if (!version.current_build.empty()) {
        output_tree["windows_current_build"] = version.current_build;
      }
      if (version.build_number.has_value()) {
        output_tree["windows_build_number"] = version.build_number.value();
      }
      if (version.major_version.has_value()) {
        output_tree["windows_major_version"] = version.major_version.value();
      }
      if (version.minor_version.has_value()) {
        output_tree["windows_minor_version"] = version.minor_version.value();
      }
    } catch (...) {
      // Non-fatal; keep metadata response minimal if enumeration fails.
    }
#endif
    send_response(response, output_tree);
  }

  /**
   * @brief Get the locale setting. This endpoint does not require authentication.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/configLocale| GET| null}
   */
  void getLocale(resp_https_t response, req_https_t request) {
    // we need to return the locale whether authenticated or not

    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["locale"] = config::sunshine.locale;
    send_response(response, output_tree);
  }

  /**
   * @brief Save the configuration settings.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the post request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "key": "value"
   * }
   * @endcode
   *
   * @attention{It is recommended to ONLY save the config settings that differ from the default behavior.}
   *
   * @api_examples{/api/config| POST| {"key":"value"}}
   */
  void saveConfig(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      // TODO: Input Validation
      std::stringstream config_stream;
      nlohmann::json output_tree;
      nlohmann::json input_tree = nlohmann::json::parse(ss);
      for (const auto &[k, v] : input_tree.items()) {
        if (v.is_null() || (v.is_string() && v.get<std::string>().empty())) {
          continue;
        }

        // v.dump() will dump valid json, which we do not want for strings in the config right now
        // we should migrate the config file to straight json and get rid of all this nonsense
        config_stream << k << " = " << (v.is_string() ? v.get<std::string>() : v.dump()) << std::endl;
      }
      file_handler::write_file(config::sunshine.config_file.c_str(), config_stream.str());

      // Detect restart-required keys
      static const std::set<std::string> restart_required_keys = {
        "port",
        "address_family",
        "upnp",
        "pkey",
        "cert"
      };
      bool restart_required = false;
      for (const auto &[k, _] : input_tree.items()) {
        if (restart_required_keys.count(k)) {
          restart_required = true;
          break;
        }
      }

      bool applied_now = false;
      bool deferred = false;

      if (!restart_required) {
        if (rtsp_stream::session_count() == 0) {
          // Apply immediately
          config::apply_config_now();
          applied_now = true;
        } else {
          config::mark_deferred_reload();
          deferred = true;
        }
      }

      output_tree["status"] = true;
      output_tree["appliedNow"] = applied_now;
      output_tree["deferred"] = deferred;
      output_tree["restartRequired"] = restart_required;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SaveConfig: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Partial update of configuration (PATCH /api/config).
   * Merges provided JSON object into the existing key=value style config file.
   * Removes keys when value is null or an empty string. Detects whether a
   * restart is required and attempts to apply immediately when safe.
   */
  void patchConfig(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      nlohmann::json output_tree;
      nlohmann::json patch_tree = nlohmann::json::parse(ss);
      if (!patch_tree.is_object()) {
        bad_request(response, request, "PATCH body must be a JSON object");
        return;
      }

      // Load existing config into a map
      std::unordered_map<std::string, std::string> current = config::parse_config(
        file_handler::read_file(config::sunshine.config_file.c_str())
      );

      // Track which keys are being modified to detect restart requirements
      std::set<std::string> changed_keys;

      for (auto it = patch_tree.begin(); it != patch_tree.end(); ++it) {
        const std::string key = it.key();
        const nlohmann::json &val = it.value();
        changed_keys.insert(key);

        // Remove key when explicitly null or empty string
        if (val.is_null() || (val.is_string() && val.get<std::string>().empty())) {
          auto curIt = current.find(key);
          if (curIt != current.end()) {
            current.erase(curIt);
          }
          continue;
        }

        // Persist value: strings are raw, non-strings are dumped as JSON
        if (val.is_string()) {
          current[key] = val.get<std::string>();
        } else {
          current[key] = val.dump();
        }
      }

      // Write back full merged config file
      std::stringstream config_stream;
      for (const auto &kv : current) {
        config_stream << kv.first << " = " << kv.second << std::endl;
      }
      file_handler::write_file(config::sunshine.config_file.c_str(), config_stream.str());

      // Detect restart-required keys
      static const std::set<std::string> restart_required_keys = {
        "port",
        "address_family",
        "upnp",
        "pkey",
        "cert"
      };
      bool restart_required = false;
      for (const auto &k : changed_keys) {
        if (restart_required_keys.count(k)) {
          restart_required = true;
          break;
        }
      }

      bool applied_now = false;
      bool deferred = false;
      if (!restart_required) {
        // Determine if only Playnite-related keys were changed; these are safe to hot-apply
        // even when a streaming session is active.
        bool only_playnite = !changed_keys.empty();
        for (const auto &k : changed_keys) {
          if (k.rfind("playnite_", 0) != 0) {
            only_playnite = false;
            break;
          }
        }
        if (only_playnite || rtsp_stream::session_count() == 0) {
          // Apply immediately
          config::apply_config_now();
          applied_now = true;
        } else {
          config::mark_deferred_reload();
          deferred = true;
        }
      }

      output_tree["status"] = true;
      output_tree["appliedNow"] = applied_now;
      output_tree["deferred"] = deferred;
      output_tree["restartRequired"] = restart_required;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "PatchConfig: "sv << e.what();
      bad_request(response, request, e.what());
      return;
    }
  }

  // Lightweight session status for UI messaging
  void getSessionStatus(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);

    nlohmann::json output_tree;
    const int active = rtsp_stream::session_count();
    const bool app_running = proc::proc.running() > 0;
    output_tree["activeSessions"] = active;
    output_tree["appRunning"] = app_running;
    output_tree["paused"] = app_running && active == 0;
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  void listWebRTCSessions(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    nlohmann::json output;
    output["sessions"] = nlohmann::json::array();
    for (const auto &session : webrtc_stream::list_sessions()) {
      output["sessions"].push_back(webrtc_session_to_json(session));
    }
    send_response(response, output);
  }

  void createWebRTCSession(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    BOOST_LOG(debug) << "WebRTC: create session request received";

    webrtc_stream::SessionOptions options;
    std::stringstream ss;
    ss << request->content.rdbuf();
    auto body = ss.str();
    if (!body.empty()) {
      if (!check_content_type(response, request, "application/json")) {
        return;
      }
      try {
        nlohmann::json input = nlohmann::json::parse(body);
        if (input.contains("audio")) {
          options.audio = input.at("audio").get<bool>();
        }
        if (input.contains("host_audio")) {
          options.host_audio = input.at("host_audio").get<bool>();
        }
        if (input.contains("video")) {
          options.video = input.at("video").get<bool>();
        }
        if (input.contains("encoded")) {
          options.encoded = input.at("encoded").get<bool>();
        }
        if (input.contains("width")) {
          options.width = input.at("width").get<int>();
        }
        if (input.contains("height")) {
          options.height = input.at("height").get<int>();
        }
        if (input.contains("fps")) {
          options.fps = input.at("fps").get<int>();
        }
        if (input.contains("bitrate_kbps")) {
          options.bitrate_kbps = input.at("bitrate_kbps").get<int>();
        }
        if (input.contains("codec")) {
          options.codec = input.at("codec").get<std::string>();
        }
        if (input.contains("hdr")) {
          options.hdr = input.at("hdr").get<bool>();
        }
        if (input.contains("audio_channels")) {
          options.audio_channels = input.at("audio_channels").get<int>();
        }
        if (input.contains("audio_codec")) {
          options.audio_codec = input.at("audio_codec").get<std::string>();
        }
        if (input.contains("profile")) {
          options.profile = input.at("profile").get<std::string>();
        }
        if (input.contains("app_id")) {
          options.app_id = input.at("app_id").get<int>();
        }
        if (input.contains("resume")) {
          options.resume = input.at("resume").get<bool>();
        }

        if (options.codec) {
          auto lower = *options.codec;
          boost::algorithm::to_lower(lower);
          if (lower != "h264" && lower != "hevc" && lower != "av1") {
            bad_request(response, request, "Unsupported codec");
            return;
          }
          options.codec = std::move(lower);
        }
        if (options.audio_codec) {
          auto lower = *options.audio_codec;
          boost::algorithm::to_lower(lower);
          if (lower != "opus" && lower != "aac") {
            bad_request(response, request, "Unsupported audio codec");
            return;
          }
          options.audio_codec = std::move(lower);
        }
        if (options.audio_channels) {
          int channels = *options.audio_channels;
          if (channels != 2 && channels != 6 && channels != 8) {
            bad_request(response, request, "Unsupported audio channel count");
            return;
          }
        }
      } catch (const std::exception &e) {
        bad_request(response, request, e.what());
        return;
      }
    }

    BOOST_LOG(debug) << "WebRTC: creating session";
    if (auto error = webrtc_stream::ensure_capture_started(options)) {
      bad_request(response, request, error->c_str());
      return;
    }
    auto session = webrtc_stream::create_session(options);
    BOOST_LOG(debug) << "WebRTC: session created id=" << session.id;
    nlohmann::json output;
    output["status"] = true;
    output["session"] = webrtc_session_to_json(session);
    output["cert_fingerprint"] = webrtc_stream::get_server_cert_fingerprint();
    output["cert_pem"] = webrtc_stream::get_server_cert_pem();
    output["ice_servers"] = load_webrtc_ice_servers();
    send_response(response, output);
  }

  void getWebRTCSession(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    std::string session_id;
    if (request->path_match.size() > 1) {
      session_id = request->path_match[1];
    }

    auto session = webrtc_stream::get_session(session_id);
    if (!session) {
      bad_request(response, request, "Session not found");
      return;
    }

    nlohmann::json output;
    output["session"] = webrtc_session_to_json(*session);
    send_response(response, output);
  }

  void deleteWebRTCSession(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    std::string session_id;
    if (request->path_match.size() > 1) {
      session_id = request->path_match[1];
    }

    nlohmann::json output;
    if (webrtc_stream::close_session(session_id)) {
      output["status"] = true;
    } else {
      output["error"] = "Session not found";
    }
    send_response(response, output);
  }

  void postWebRTCOffer(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    if (!check_content_type(response, request, "application/json")) {
      return;
    }

    std::string session_id;
    if (request->path_match.size() > 1) {
      session_id = request->path_match[1];
    }

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      nlohmann::json input = nlohmann::json::parse(ss.str());
      auto sdp = input.at("sdp").get<std::string>();
      auto type = input.value("type", "offer");
      nlohmann::json output;
      if (!webrtc_stream::set_remote_offer(session_id, sdp, type)) {
        if (!webrtc_stream::get_session(session_id)) {
          output["error"] = "Session not found";
        } else {
          output["error"] = "Failed to process offer";
        }
        send_response(response, output);
        return;
      }

      std::string answer_sdp;
      std::string answer_type;
      if (webrtc_stream::get_local_answer(session_id, answer_sdp, answer_type)) {
        output["status"] = true;
        output["answer_ready"] = true;
        output["sdp"] = answer_sdp;
        output["type"] = answer_type;
      } else {
        output["status"] = true;
        output["answer_ready"] = false;
        output["sdp"] = nullptr;
        output["type"] = nullptr;
      }
      send_response(response, output);
    } catch (const std::exception &e) {
      bad_request(response, request, e.what());
    }
  }

  void getWebRTCAnswer(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    std::string session_id;
    if (request->path_match.size() > 1) {
      session_id = request->path_match[1];
    }

    std::string answer_sdp;
    std::string answer_type;
    nlohmann::json output;
    if (webrtc_stream::get_local_answer(session_id, answer_sdp, answer_type)) {
      output["status"] = true;
      output["answer_ready"] = true;
      output["sdp"] = answer_sdp;
      output["type"] = answer_type;
    } else {
      output["status"] = false;
      output["error"] = "Answer not ready";
    }
    send_response(response, output);
  }

  void postWebRTCIce(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    if (!check_content_type(response, request, "application/json")) {
      return;
    }

    std::string session_id;
    if (request->path_match.size() > 1) {
      session_id = request->path_match[1];
    }

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      nlohmann::json input = nlohmann::json::parse(ss.str());
      auto mid = input.value("sdpMid", "");
      auto mline_index = input.value("sdpMLineIndex", -1);
      auto candidate = input.at("candidate").get<std::string>();
      nlohmann::json output;
      if (webrtc_stream::add_ice_candidate(session_id, std::move(mid), mline_index, std::move(candidate))) {
        output["status"] = true;
      } else {
        output["error"] = "Session not found";
      }
      send_response(response, output);
    } catch (const std::exception &e) {
      bad_request(response, request, e.what());
    }
  }

  void getWebRTCIce(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    std::string session_id;
    if (request->path_match.size() > 1) {
      session_id = request->path_match[1];
    }

    std::size_t since = 0;
    auto query = request->parse_query_string();
    auto since_it = query.find("since");
    if (since_it != query.end()) {
      try {
        since = static_cast<std::size_t>(std::stoull(since_it->second));
      } catch (...) {
        bad_request(response, request, "Invalid since parameter");
        return;
      }
    }

    auto candidates = webrtc_stream::get_local_candidates(session_id, since);
    nlohmann::json output;
    output["status"] = true;
    output["candidates"] = nlohmann::json::array();
    std::size_t last_index = since;
    for (const auto &candidate : candidates) {
      nlohmann::json item;
      item["sdpMid"] = candidate.mid;
      item["sdpMLineIndex"] = candidate.mline_index;
      item["candidate"] = candidate.candidate;
      item["index"] = candidate.index;
      output["candidates"].push_back(std::move(item));
      last_index = std::max(last_index, candidate.index);
    }
    output["next_since"] = last_index;
    send_response(response, output);
  }

  void getWebRTCIceStream(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    std::string session_id;
    if (request->path_match.size() > 1) {
      session_id = request->path_match[1];
    }

    if (!webrtc_stream::get_session(session_id)) {
      bad_request(response, request, "Session not found");
      return;
    }

    std::size_t since = 0;
    auto query = request->parse_query_string();
    auto since_it = query.find("since");
    if (since_it != query.end()) {
      try {
        since = static_cast<std::size_t>(std::stoull(since_it->second));
      } catch (...) {
        bad_request(response, request, "Invalid since parameter");
        return;
      }
    }

    std::thread([response, session_id, since]() mutable {
      response->close_connection_after_response = true;

      response->write({
        {"Content-Type", "text/event-stream"},
        {"Cache-Control", "no-cache"},
        {"Connection", "keep-alive"},
        {"Access-Control-Allow-Origin", get_cors_origin()}
      });

      std::promise<bool> header_error;
      response->send([&header_error](const SimpleWeb::error_code &ec) {
        header_error.set_value(static_cast<bool>(ec));
      });
      if (header_error.get_future().get()) {
        return;
      }

      auto last_index = since;
      auto last_keepalive = std::chrono::steady_clock::now();

      while (true) {
        auto candidates = webrtc_stream::get_local_candidates(session_id, last_index);
        for (const auto &candidate : candidates) {
          nlohmann::json payload;
          payload["sdpMid"] = candidate.mid;
          payload["sdpMLineIndex"] = candidate.mline_index;
          payload["candidate"] = candidate.candidate;

          *response << "event: candidate\n";
          *response << "id: " << candidate.index << "\n";
          *response << "data: " << payload.dump() << "\n\n";

          std::promise<bool> error;
          response->send([&error](const SimpleWeb::error_code &ec) {
            error.set_value(static_cast<bool>(ec));
          });
          if (error.get_future().get()) {
            return;
          }

          last_index = std::max(last_index, candidate.index);
        }

        auto now = std::chrono::steady_clock::now();
        if (now - last_keepalive > std::chrono::seconds(2)) {
          *response << "event: keepalive\n";
          *response << "data: {}\n\n";
          std::promise<bool> error;
          response->send([&error](const SimpleWeb::error_code &ec) {
            error.set_value(static_cast<bool>(ec));
          });
          if (error.get_future().get()) {
            return;
          }
          last_keepalive = now;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
    }).detach();
  }

  void getWebRTCCert(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    nlohmann::json output;
    output["cert_fingerprint"] = webrtc_stream::get_server_cert_fingerprint();
    output["cert_pem"] = webrtc_stream::get_server_cert_pem();
    send_response(response, output);
  }

  /**
   * @brief Upload a cover image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the post request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "key": "igdb_<game_id>",
   *   "url": "https://images.igdb.com/igdb/image/upload/t_cover_big_2x/<slug>.png"
   * }
   * @endcode
   *
   * @api_examples{/api/covers/upload| POST| {"key":"igdb_1234","url":"https://images.igdb.com/igdb/image/upload/t_cover_big_2x/abc123.png"}}
   */
  void uploadCover(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      nlohmann::json output_tree;
      nlohmann::json input_tree = nlohmann::json::parse(ss);

      std::string key = input_tree.value("key", "");
      if (key.empty()) {
        bad_request(response, request, "Cover key is required");
        return;
      }
      std::string url = input_tree.value("url", "");

      const std::string coverdir = platf::appdata().string() + "/covers/";
      file_handler::make_directory(coverdir);

      // Final destination PNG path
      const std::string dest_png = coverdir + http::url_escape(key) + ".png";

      // Helper to check PNG magic header
      auto file_is_png = [](const std::string &p) -> bool {
        std::ifstream f(p, std::ios::binary);

        if (!f) {
          return false;
        }
        unsigned char sig[8] {};
        f.read(reinterpret_cast<char *>(sig), 8);
        static const unsigned char pngsig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

        return f.gcount() == 8 && std::equal(std::begin(sig), std::end(sig), std::begin(pngsig));
      };

      // Build a temp source path (extension based on URL if available)
      auto ext_from_url = [](std::string u) -> std::string {
        auto qpos = u.find_first_of("?#");

        if (qpos != std::string::npos) {
          u = u.substr(0, qpos);
        }
        auto slash = u.find_last_of('/');
        if (slash != std::string::npos) {
          u = u.substr(slash + 1);
        }
        auto dot = u.find_last_of('.');
        if (dot == std::string::npos) {
          return std::string {".img"};
        }
        std::string e = u.substr(dot);
        // sanitize extension
        if (e.size() > 8) {
          return std::string {".img"};
        }
        for (char &c : e) {
          c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        return e;
      };

      std::string src_tmp;
      if (!url.empty()) {
        if (http::url_get_host(url) != "images.igdb.com") {
          bad_request(response, request, "Only images.igdb.com is allowed");
          return;
        }
        const std::string ext = ext_from_url(url);
        src_tmp = coverdir + http::url_escape(key) + "_src" + ext;
        if (!http::download_file(url, src_tmp)) {
          bad_request(response, request, "Failed to download cover");
          return;
        }
      }

      bool converted = false;
#ifdef _WIN32
      {
        // Convert using WIC helper; falls back to copying if already PNG
        std::wstring src_w(src_tmp.begin(), src_tmp.end());
        std::wstring dst_w(dest_png.begin(), dest_png.end());
        converted = platf::img::convert_to_png_96dpi(src_w, dst_w);
        if (!converted && file_is_png(src_tmp)) {
          std::error_code ec {};
          std::filesystem::copy_file(src_tmp, dest_png, std::filesystem::copy_options::overwrite_existing, ec);
          converted = !ec.operator bool();
        }
      }
#else
      // Non-Windows: we can’t transcode here; accept only already-PNG data
      if (file_is_png(src_tmp)) {
        std::error_code ec {};

        std::filesystem::rename(src_tmp, dest_png, ec);
        if (ec) {
          // If rename fails (cross-device), try copy
          std::filesystem::copy_file(src_tmp, dest_png, std::filesystem::copy_options::overwrite_existing, ec);
          if (!ec) {
            std::filesystem::remove(src_tmp);
            converted = true;
          }
        } else {
          converted = true;
        }
      } else {
        // Leave a clear error on non-Windows when not PNG
        bad_request(response, request, "Cover must be PNG on this platform");
        return;
      }
#endif

      // Cleanup temp source file when possible
      if (!src_tmp.empty()) {
        std::error_code del_ec {};

        std::filesystem::remove(src_tmp, del_ec);
      }

      if (!converted) {
        bad_request(response, request, "Failed to convert cover to PNG");
        return;
      }

      output_tree["status"] = true;
      output_tree["path"] = dest_png;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "UploadCover: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Purge all auto-synced Playnite applications (playnite-managed == "auto").
   * @api_examples{/api/apps/purge_autosync| POST| null}
   */
  void purgeAutoSyncedApps(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      nlohmann::json output_tree;
      nlohmann::json new_apps = nlohmann::json::array();
      std::string file = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json file_tree = nlohmann::json::parse(file);
      auto &apps_node = file_tree["apps"];

      int removed = 0;
      for (auto &app : apps_node) {
        std::string managed = app.contains("playnite-managed") && app["playnite-managed"].is_string() ? app["playnite-managed"].get<std::string>() : std::string();
        if (managed == "auto") {
          ++removed;
          continue;
        }
        new_apps.push_back(app);
      }

      file_tree["apps"] = new_apps;
      confighttp::refresh_client_apps_cache(file_tree);

      output_tree["status"] = true;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "purgeAutoSyncedApps: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Get the logs from the log file.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/logs| GET| null}
   */
  void getLogs(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::string content;
    auto log_path = logging::current_log_file();
    if (!log_path.empty()) {
      const std::string log_path_str = log_path.string();
      content = file_handler::read_file(log_path_str.c_str());
    }
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/plain");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(success_ok, content, headers);
  }

#ifdef _WIN32
#endif

  /**
   * @brief Update existing credentials.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the post request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "currentUsername": "Current Username",
   *   "currentPassword": "Current Password",
   *   "newUsername": "New Username",
   *   "newPassword": "New Password",
   *   "confirmNewPassword": "Confirm New Password"
   * }
   * @endcode
   *
   * @api_examples{/api/password| POST| {"currentUsername":"admin","currentPassword":"admin","newUsername":"admin","newPassword":"admin","confirmNewPassword":"admin"}}
   */
  void savePassword(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!config::sunshine.username.empty() && !authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::vector<std::string> errors = {};
    std::stringstream ss;
    std::stringstream config_stream;
    ss << request->content.rdbuf();
    try {
      // TODO: Input Validation
      nlohmann::json output_tree;
      nlohmann::json input_tree = nlohmann::json::parse(ss);
      std::string username = input_tree.value("currentUsername", "");
      std::string newUsername = input_tree.value("newUsername", "");
      std::string password = input_tree.value("currentPassword", "");
      std::string newPassword = input_tree.value("newPassword", "");
      std::string confirmPassword = input_tree.value("confirmNewPassword", "");
      if (newUsername.empty()) {
        newUsername = username;
      }
      if (newUsername.empty()) {
        errors.emplace_back("Invalid Username");
      } else {
        auto hash = util::hex(crypto::hash(password + config::sunshine.salt)).to_string();
        if (config::sunshine.username.empty() || (boost::iequals(username, config::sunshine.username) && hash == config::sunshine.password)) {
          if (newPassword.empty() || newPassword != confirmPassword) {
            errors.emplace_back("Password Mismatch");
          } else {
            http::save_user_creds(config::sunshine.credentials_file, newUsername, newPassword);
            http::reload_user_creds(config::sunshine.credentials_file);
            output_tree["status"] = true;
          }
        } else {
          errors.emplace_back("Invalid Current Credentials");
        }
      }

      if (!errors.empty()) {
        // join the errors array
        std::string error = std::accumulate(errors.begin(), errors.end(), std::string(), [](const std::string &a, const std::string &b) {
          return a.empty() ? b : a + ", " + b;
        });
        bad_request(response, request, error);
        return;
      }

      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SavePassword: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Send a pin code to the host. The pin is generated from the Moonlight client during the pairing process.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the post request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "pin": "<pin>",
   *   "name": "Friendly Client Name"
   * }
   * @endcode
   *
   * @api_examples{/api/pin| POST| {"pin":"1234","name":"My PC"}}
   */
  void savePin(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      nlohmann::json output_tree;
      nlohmann::json input_tree = nlohmann::json::parse(ss);
      const std::string name = input_tree.value("name", "");
      const std::string pin = input_tree.value("pin", "");

      int _pin = 0;
      _pin = std::stoi(pin);
      if (_pin < 0 || _pin > 9999) {
        bad_request(response, request, "PIN must be between 0000 and 9999");
      }

      output_tree["status"] = nvhttp::pin(pin, name);
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SavePin: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Reset the display device persistence.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/reset-display-device-persistence| POST| null}
   */
  void resetDisplayDevicePersistence(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = display_helper_integration::reset_persistence();
    send_response(response, output_tree);
  }

#ifdef _WIN32
  /**
   * @brief Export the current Windows display settings as a golden restore snapshot.
   * @api_examples{/api/display/export_golden| POST| {"status":true}}
   */
  void postExportGoldenDisplay(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);
    nlohmann::json out;
    try {
      const bool ok = display_helper_integration::export_golden_restore();
      out["status"] = ok;
    } catch (...) {
      out["status"] = false;
    }
    send_response(response, out);
  }
#endif

#ifdef _WIN32
  // --- Golden snapshot helpers (Windows-only) ---
  static bool file_exists_nofail(const std::filesystem::path &p) {
    try {
      std::error_code ec;
      return std::filesystem::exists(p, ec);
    } catch (...) {
      return false;
    }
  }

  // Return candidate paths where the helper writes the golden snapshot.
  // We probe both the active user's Roaming/Local AppData and the current
  // process's CSIDL paths, mirroring the log bundle collection logic.
  static std::vector<std::filesystem::path> golden_snapshot_candidates() {
    std::vector<std::filesystem::path> out;
    auto add_if = [&](const std::filesystem::path &base) {
      if (!base.empty()) {
        out.emplace_back(base / L"Sunshine" / L"display_golden_restore.json");
      }
    };

    try {
      // Prefer the active user's known folders (impersonated) when available
      try {
        platf::dxgi::safe_token user_token;
        user_token.reset(platf::dxgi::retrieve_users_token(false));
        auto add_known = [&](REFKNOWNFOLDERID id) {
          PWSTR baseW = nullptr;
          if (SUCCEEDED(SHGetKnownFolderPath(id, 0, user_token.get(), &baseW)) && baseW) {
            add_if(std::filesystem::path(baseW));
            CoTaskMemFree(baseW);
          }
        };
        add_known(FOLDERID_RoamingAppData);
        add_known(FOLDERID_LocalAppData);
      } catch (...) {
        // ignore
      }

      // Also probe the current process's CSIDL APPDATA and LOCAL_APPDATA
      auto add_csidl = [&](int csidl) {
        wchar_t baseW[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, baseW))) {
          add_if(std::filesystem::path(baseW));
        }
      };
      add_csidl(CSIDL_APPDATA);
      add_csidl(CSIDL_LOCAL_APPDATA);
    } catch (...) {
      // best-effort
    }
    return out;
  }

  void getGoldenStatus(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);
    nlohmann::json out;
    bool exists = false;
    try {
      for (const auto &p : golden_snapshot_candidates()) {
        if (file_exists_nofail(p)) {
          exists = true;
          break;
        }
      }
    } catch (...) {
    }
    out["exists"] = exists;
    send_response(response, out);
  }

  void deleteGolden(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);
    nlohmann::json out;
    bool any_deleted = false;
    try {
      for (const auto &p : golden_snapshot_candidates()) {
        if (file_exists_nofail(p)) {
          std::error_code ec;
          std::filesystem::remove(p, ec);
          if (!ec) {
            any_deleted = true;
          }
        }
      }
    } catch (...) {
    }
    out["deleted"] = any_deleted;
    send_response(response, out);
  }
#endif

  /**
   * @brief Restart Sunshine.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/restart| POST| null}
   */
  void restart(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    // We may not return from this call
    platf::restart();
  }

  /**
   * @brief Generate a new API token with specified scopes.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/token| POST| {"scopes":[{"path":"/api/apps","methods":["GET"]}]}}}
   *
   * Request body example:
   * {
   *   "scopes": [
   *     { "path": "/api/apps", "methods": ["GET", "POST"] }
   *   ]
   * }
   *
   * Response example:
   * { "token": "..." }
   */
  void generateApiToken(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    std::stringstream ss;
    ss << request->content.rdbuf();
    const std::string request_body = ss.str();
    auto token_opt = api_token_manager.generate_api_token(request_body, config::sunshine.username);
    nlohmann::json output_tree;
    if (!token_opt) {
      output_tree["error"] = "Invalid token request";
      send_response(response, output_tree);
      return;
    }
    output_tree["token"] = *token_opt;
    send_response(response, output_tree);
  }

  /**
   * @brief List all active API tokens and their scopes.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/tokens| GET| null}
   *
   * Response example:
   * [
   *   {
   *     "hash": "...",
   *     "username": "admin",
   *     "created_at": 1719000000,
   *     "scopes": [
   *       { "path": "/api/apps", "methods": ["GET"] }
   *     ]
   *   }
   * ]
   */
  void listApiTokens(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    nlohmann::json output_tree = nlohmann::json::parse(api_token_manager.list_api_tokens_json());
    send_response(response, output_tree);
  }

  /**
   * @brief Revoke (delete) an API token by its hash.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/token/abcdef1234567890| DELETE| null}
   *
   * Response example:
   * { "status": true }
   */
  void revokeApiToken(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    std::string hash;
    if (request->path_match.size() > 1) {
      hash = request->path_match[1];
    }
    bool result = api_token_manager.revoke_api_token_by_hash(hash);
    nlohmann::json output_tree;
    if (result) {
      output_tree["status"] = true;
    } else {
      output_tree["error"] = "Internal server error";
    }
    send_response(response, output_tree);
  }

  void listSessions(resp_https_t response, req_https_t request);
  void revokeSession(resp_https_t response, req_https_t request);

  void start() {
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    auto port_https = net::map_port(PORT_HTTPS);
    auto address_family = net::af_from_enum_string(config::sunshine.address_family);

    https_server_t server(config::nvhttp.cert, config::nvhttp.pkey);
    server.default_resource["DELETE"] = [](resp_https_t response, req_https_t request) {
      bad_request(response, request);
    };
    server.default_resource["PATCH"] = [](resp_https_t response, req_https_t request) {
      bad_request(response, request);
    };
    server.default_resource["POST"] = [](resp_https_t response, req_https_t request) {
      bad_request(response, request);
    };
    server.default_resource["PUT"] = [](resp_https_t response, req_https_t request) {
      bad_request(response, request);
    };

    // Serve the SPA shell for any unmatched GET route. Explicit static and API
    // routes are registered below; UI page routes are deprecated server-side
    // and are handled by the SPA entry responder so frontend can manage
    // authentication and routing.
    server.default_resource["GET"] = getSpaEntry;
    server.resource["^/$"]["GET"] = getSpaEntry;
    server.resource["^/pin/?$"]["GET"] = getSpaEntry;
    server.resource["^/apps/?$"]["GET"] = getSpaEntry;
    server.resource["^/clients/?$"]["GET"] = getSpaEntry;
    server.resource["^/config/?$"]["GET"] = getSpaEntry;
    server.resource["^/password/?$"]["GET"] = getSpaEntry;
    server.resource["^/welcome/?$"]["GET"] = getSpaEntry;
    server.resource["^/login/?$"]["GET"] = getSpaEntry;
    server.resource["^/troubleshooting/?$"]["GET"] = getSpaEntry;
    server.resource["^/api/pin$"]["POST"] = savePin;
    server.resource["^/api/apps$"]["GET"] = getApps;
    server.resource["^/api/logs$"]["GET"] = getLogs;
    server.resource["^/api/apps$"]["POST"] = saveApp;
    server.resource["^/api/config$"]["GET"] = getConfig;
    server.resource["^/api/config$"]["POST"] = saveConfig;
    // Partial updates for config settings; merges with existing file and
    // removes keys when value is null or empty string.
    server.resource["^/api/config$"]["PATCH"] = patchConfig;
    server.resource["^/api/metadata$"]["GET"] = getMetadata;
    server.resource["^/api/configLocale$"]["GET"] = getLocale;
    server.resource["^/api/restart$"]["POST"] = restart;
    server.resource["^/api/reset-display-device-persistence$"]["POST"] = resetDisplayDevicePersistence;
#if defined(_WIN32)
    server.resource["^/api/display/export_golden$"]["POST"] = postExportGoldenDisplay;
    server.resource["^/api/display/golden_status$"]["GET"] = getGoldenStatus;
    server.resource["^/api/display/golden$"]["DELETE"] = deleteGolden;
#endif
    server.resource["^/api/password$"]["POST"] = savePassword;
    server.resource["^/api/display-devices$"]["GET"] = getDisplayDevices;
#ifdef _WIN32
    server.resource["^/api/framegen/edid-refresh$"]["GET"] = getFramegenEdidRefresh;
    server.resource["^/api/health/vigem$"]["GET"] = getVigemHealth;
    server.resource["^/api/health/crashdump$"]["GET"] = getCrashDumpStatus;
    server.resource["^/api/health/crashdump/dismiss$"]["POST"] = postCrashDumpDismiss;
#endif
    server.resource["^/api/apps/([A-Fa-f0-9-]+)/cover$"]["GET"] = getAppCover;
    server.resource["^/api/apps/([0-9]+)$"]["DELETE"] = deleteApp;
    server.resource["^/api/clients/unpair-all$"]["POST"] = unpairAll;
    server.resource["^/api/clients/list$"]["GET"] = getClients;
    server.resource["^/api/clients/hdr-profiles$"]["GET"] = getHdrProfiles;
    server.resource["^/api/clients/update$"]["POST"] = updateClient;
    server.resource["^/api/clients/unpair$"]["POST"] = unpair;
    server.resource["^/api/clients/disconnect$"]["POST"] = disconnectClient;
    server.resource["^/api/apps/close$"]["POST"] = closeApp;
    server.resource["^/api/session/status$"]["GET"] = getSessionStatus;
    server.resource["^/api/webrtc/sessions$"]["GET"] = listWebRTCSessions;
    server.resource["^/api/webrtc/sessions$"]["POST"] = createWebRTCSession;
    server.resource["^/api/webrtc/sessions/([A-Fa-f0-9-]+)$"]["GET"] = getWebRTCSession;
    server.resource["^/api/webrtc/sessions/([A-Fa-f0-9-]+)$"]["DELETE"] = deleteWebRTCSession;
    server.resource["^/api/webrtc/sessions/([A-Fa-f0-9-]+)/offer$"]["POST"] = postWebRTCOffer;
    server.resource["^/api/webrtc/sessions/([A-Fa-f0-9-]+)/answer$"]["GET"] = getWebRTCAnswer;
    server.resource["^/api/webrtc/sessions/([A-Fa-f0-9-]+)/ice$"]["GET"] = getWebRTCIce;
    server.resource["^/api/webrtc/sessions/([A-Fa-f0-9-]+)/ice$"]["POST"] = postWebRTCIce;
    server.resource["^/api/webrtc/sessions/([A-Fa-f0-9-]+)/ice/stream$"]["GET"] = getWebRTCIceStream;
    server.resource["^/api/webrtc/cert$"]["GET"] = getWebRTCCert;
    // Keep legacy cover upload endpoint present in upstream master
    server.resource["^/api/covers/upload$"]["POST"] = uploadCover;
    server.resource["^/api/apps/purge_autosync$"]["POST"] = purgeAutoSyncedApps;
#ifdef _WIN32
    server.resource["^/api/playnite/status$"]["GET"] = getPlayniteStatus;
    server.resource["^/api/rtss/status$"]["GET"] = getRtssStatus;
    server.resource["^/api/lossless_scaling/status$"]["GET"] = getLosslessScalingStatus;
    server.resource["^/api/playnite/install$"]["POST"] = installPlaynite;
    server.resource["^/api/playnite/uninstall$"]["POST"] = uninstallPlaynite;
    server.resource["^/api/playnite/games$"]["GET"] = getPlayniteGames;
    server.resource["^/api/playnite/categories$"]["GET"] = getPlayniteCategories;
    server.resource["^/api/playnite/force_sync$"]["POST"] = postPlayniteForceSync;
    server.resource["^/api/playnite/launch$"]["POST"] = postPlayniteLaunch;
    // Export logs bundle (Windows only)
    server.resource["^/api/logs/export$"]["GET"] = downloadPlayniteLogs;
    server.resource["^/api/logs/export_crash$"]["GET"] = downloadCrashBundle;
#endif
    server.resource["^/images/sunshine.ico$"]["GET"] = getFaviconImage;
    server.resource["^/images/logo-sunshine-45.png$"]["GET"] = getSunshineLogoImage;
    server.resource["^/assets\\/.+$"]["GET"] = getNodeModules;
    server.resource["^/api/token$"]["POST"] = generateApiToken;
    server.resource["^/api/tokens$"]["GET"] = listApiTokens;
    server.resource["^/api/token/([a-fA-F0-9]+)$"]["DELETE"] = revokeApiToken;
    // Session validation endpoint used by the web UI to detect HttpOnly session cookies
    server.resource["^/api-tokens/?$"]["GET"] = getTokenPage;
    server.resource["^/api/auth/login$"]["POST"] = loginUser;
    server.resource["^/api/auth/refresh$"]["POST"] = refreshSession;
    server.resource["^/api/auth/logout$"]["POST"] = logoutUser;
    server.resource["^/api/auth/status$"]["GET"] = authStatus;
    server.resource["^/api/auth/sessions$"]["GET"] = listSessions;
    server.resource["^/api/auth/sessions/([A-Fa-f0-9]+)$"]["DELETE"] = revokeSession;
    server.config.reuse_address = true;
    server.config.address = net::af_to_any_address_string(address_family);
    server.config.port = port_https;

    auto accept_and_run = [&](auto *server) {
      try {
        server->start([](unsigned short port) {
          BOOST_LOG(info) << "Configuration UI available at [https://localhost:"sv << port << "]";
        });
      } catch (boost::system::system_error &err) {
        // It's possible the exception gets thrown after calling server->stop() from a different thread
        if (shutdown_event->peek()) {
          return;
        }
        BOOST_LOG(fatal) << "Couldn't start Configuration HTTPS server on port ["sv << port_https << "]: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
    };
    api_token_manager.load_api_tokens();
    session_token_manager.load_session_tokens();
    std::thread tcp {accept_and_run, &server};

    // Start a background task to clean up expired session tokens every hour
    std::jthread cleanup_thread([shutdown_event]() {
      while (!shutdown_event->view(std::chrono::hours(1))) {
        if (session_token_manager.cleanup_expired_session_tokens()) {
          session_token_manager.save_session_tokens();
        }
      }
    });

    // Wait for any event
    shutdown_event->view();

    server.stop();

    tcp.join();
    // std::jthread (cleanup_thread) auto-joins on destruction, no need for joinable/join
  }

  /**
   * @brief Handles the HTTP request to serve the API token management page.
   *
   * This function authenticates the incoming request and, if successful,
   * reads the "api-tokens.html" file from the web directory and sends its
   * contents as an HTTP response with the appropriate content type.
   *
   * @param response The HTTP response object used to send data back to the client.
   * @param request The HTTP request object containing client request data.
   */
  void getTokenPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);
    std::string content = file_handler::read_file(WEB_DIR "api-tokens.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    response->write(content, headers);
  }

  /**
   * @brief Converts a string representation of a token scope to its corresponding TokenScope enum value.
   *
   * This function takes a string view and returns the matching TokenScope enum value.
   * Supported string values are "Read", "read", "Write", and "write".
   * If the input string does not match any known scope, an std::invalid_argument exception is thrown.
   *
   * @param s The string view representing the token scope.
   * @return TokenScope The corresponding TokenScope enum value.
   * @throws std::invalid_argument If the input string does not match any known scope.
   */
  TokenScope scope_from_string(std::string_view s) {
    if (s == "Read" || s == "read") {
      return TokenScope::Read;
    }
    if (s == "Write" || s == "write") {
      return TokenScope::Write;
    }
    throw std::invalid_argument("Unknown TokenScope: " + std::string(s));
  }

  /**
   * @brief Converts a TokenScope enum value to its string representation.
   * @param scope The TokenScope enum value to convert.
   * @return The string representation of the scope.
   */
  std::string scope_to_string(TokenScope scope) {
    switch (scope) {
      case TokenScope::Read:
        return "Read";
      case TokenScope::Write:
        return "Write";
      default:
        throw std::invalid_argument("Unknown TokenScope enum value");
    }
  }

  /**
   * @brief User login endpoint to generate session tokens.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * Expects JSON body:
   * {
   *   "username": "string",
   *   "password": "string"
   * }
   *
   * Returns:
   * {
   *   "status": true,
   *   "token": "session_token_string",
   *   "expires_in": 86400
   * }
   *
   * @api_examples{/api/auth/login| POST| {"username": "admin", "password": "password"}}
   */
  void loginUser(resp_https_t response, req_https_t request) {
    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      nlohmann::json input_tree = nlohmann::json::parse(ss);
      if (!input_tree.contains("username") || !input_tree.contains("password")) {
        bad_request(response, request, "Missing username or password");
        return;
      }

      std::string username = input_tree["username"].get<std::string>();
      std::string password = input_tree["password"].get<std::string>();
      std::string redirect_url = input_tree.value("redirect", "/");
      bool remember_me = false;
      if (auto it = input_tree.find("remember_me"); it != input_tree.end()) {
        try {
          remember_me = it->get<bool>();
        } catch (const nlohmann::json::exception &) {
          remember_me = false;
        }
      }

      std::string user_agent;
      if (auto ua = request->header.find("user-agent"); ua != request->header.end()) {
        user_agent = ua->second;
      }
      std::string remote_address = net::addr_to_normalized_string(request->remote_endpoint().address());

    APIResponse api_response = session_token_api.login(username, password, redirect_url, remember_me, user_agent, remote_address);
    write_api_response(response, api_response);

  } catch (const nlohmann::json::exception &e) {
    BOOST_LOG(warning) << "Login JSON error:"sv << e.what();
    bad_request(response, request, "Invalid JSON format");
  }
}

  void refreshSession(resp_https_t response, req_https_t request) {
    print_req(request);

    std::string refresh_token;
    if (auto auth = request->header.find("authorization");
        auth != request->header.end() && auth->second.rfind("Refresh ", 0) == 0) {
      refresh_token = auth->second.substr(8);
    }
    if (refresh_token.empty()) {
      refresh_token = extract_refresh_token_from_cookie(request->header);
    }

    // Allow JSON body input for API clients that do not rely on cookies/Authorization header
    if (refresh_token.empty()) {
      std::stringstream ss;
      ss << request->content.rdbuf();
      if (!ss.str().empty()) {
        try {
          auto body = nlohmann::json::parse(ss);
          if (auto it = body.find("refresh_token"); it != body.end() && it->is_string()) {
            refresh_token = it->get<std::string>();
          }
        } catch (const nlohmann::json::exception &) {
        }
      }
    }

    std::string user_agent;
    if (auto ua = request->header.find("user-agent"); ua != request->header.end()) {
      user_agent = ua->second;
    }
    std::string remote_address = net::addr_to_normalized_string(request->remote_endpoint().address());

    APIResponse api_response = session_token_api.refresh_session(refresh_token, user_agent, remote_address);
    write_api_response(response, api_response);
  }

  /**
   * @brief User logout endpoint to revoke session tokens.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/auth/logout| POST| null}
   */
  void logoutUser(resp_https_t response, req_https_t request) {
    print_req(request);

    std::string session_token;
    if (auto auth = request->header.find("authorization");
        auth != request->header.end() && auth->second.rfind("Session ", 0) == 0) {
      session_token = auth->second.substr(8);
    }
    if (session_token.empty()) {
      session_token = extract_session_token_from_cookie(request->header);
    }

    std::string refresh_token = extract_refresh_token_from_cookie(request->header);

    APIResponse api_response = session_token_api.logout(session_token, refresh_token);
    write_api_response(response, api_response);
  }

  void listSessions(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);

    std::string raw_token;
    if (auto auth = request->header.find("authorization");
        auth != request->header.end() && auth->second.rfind("Session ", 0) == 0) {
      raw_token = auth->second.substr(8);
    }
    if (raw_token.empty()) {
      raw_token = extract_session_token_from_cookie(request->header);
    }
    std::string active_hash;
    if (!raw_token.empty()) {
      if (auto hash = session_token_manager.get_hash_for_token(raw_token)) {
        active_hash = *hash;
      }
    }

    APIResponse api_response = session_token_api.list_sessions(config::sunshine.username, active_hash);
    write_api_response(response, api_response);
  }

  void revokeSession(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);

    if (request->path_match.size() < 2) {
      bad_request(response, request, "Session id required");
      return;
    }
    std::string session_hash = request->path_match[1].str();

    std::string raw_token;
    if (auto auth = request->header.find("authorization");
        auth != request->header.end() && auth->second.rfind("Session ", 0) == 0) {
      raw_token = auth->second.substr(8);
    }
    if (raw_token.empty()) {
      raw_token = extract_session_token_from_cookie(request->header);
    }
    bool is_current = false;
    if (!raw_token.empty()) {
      if (auto hash = session_token_manager.get_hash_for_token(raw_token)) {
        is_current = boost::iequals(*hash, session_hash);
      }
    }

    APIResponse api_response = session_token_api.revoke_session_by_hash(session_hash);
    if (api_response.status_code == StatusCode::success_ok && is_current) {
      std::string clear_cookie = std::string(session_cookie_name) + "=; Path=/; HttpOnly; SameSite=Strict; Secure; Priority=High; Expires=Thu, 01 Jan 1970 00:00:00 GMT; Max-Age=0";
      std::string clear_refresh_cookie = std::string(refresh_cookie_name) + "=; Path=/; HttpOnly; SameSite=Strict; Secure; Priority=High; Expires=Thu, 01 Jan 1970 00:00:00 GMT; Max-Age=0";
      api_response.headers.emplace("Set-Cookie", std::move(clear_cookie));
      api_response.headers.emplace("Set-Cookie", std::move(clear_refresh_cookie));
    }
    write_api_response(response, api_response);
  }

  /**
   * @brief Authentication status endpoint.
   * Returns whether credentials are configured and if authentication is required for protected API calls.
   * This allows the frontend to avoid showing a login modal when not necessary.
   *
   * Response JSON shape:
   * {
   *   "credentials_configured": true|false,
   *   "login_required": true|false,
   *   "authenticated": true|false
   * }
   *
   * login_required becomes true only when credentials are configured and the supplied
   * request lacks valid authentication (session token or bearer token) for protected APIs.
   */
  void authStatus(resp_https_t response, req_https_t request) {
    print_req(request);

    bool credentials_configured = !config::sunshine.username.empty();

    // Determine if current request has valid auth (session or bearer) using existing check_auth
    bool authenticated = false;
    if (credentials_configured) {
      if (auto result = check_auth(request); result.ok) {
        authenticated = true;  // check_auth returns ok for public routes; refine below
        // We only consider it authenticated if an auth header or cookie was present and validated.
        std::string auth_header;
        if (auto auth_it = request->header.find("authorization"); auth_it != request->header.end()) {
          auth_header = auth_it->second;
        } else {
          std::string token = extract_session_token_from_cookie(request->header);
          if (!token.empty()) {
            auth_header = "Session " + token;
          }
        }
        if (auth_header.empty()) {
          authenticated = false;  // public access granted but no credentials supplied
        } else {
          // Re-run only auth layer for supplied header specifically to ensure validity
          auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
          auto header_check = check_auth(address, auth_header, "/api/config", "GET");  // use protected path for validation
          authenticated = header_check.ok;
        }
      }
    }

    bool login_required = credentials_configured && !authenticated;

    nlohmann::json tree;
    tree["credentials_configured"] = credentials_configured;
    tree["login_required"] = login_required;
    tree["authenticated"] = authenticated;

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json; charset=utf-8");
    add_cors_headers(headers);
    response->write(SimpleWeb::StatusCode::success_ok, tree.dump(), headers);
  }
}  // namespace confighttp
