/**
 * @file src/confighttp.h
 * @brief Declarations for the Web UI Config HTTP server.
 */
#pragma once

// standard includes
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>

// third-party includes
#include <nlohmann/json.hpp>

// local includes
#include "http_auth.h"
#include "thread_safe.h"

#include <Simple-Web-Server/server_https.hpp>

#define WEB_DIR SUNSHINE_ASSETS_DIR "/web/"

using namespace std::chrono_literals;

namespace confighttp {
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;

  constexpr auto PORT_HTTPS = 1;
  constexpr auto SESSION_EXPIRE_DURATION = 24h * 15;
  void start();

  // Token scopes for API tokens used by tests and UI
  enum class TokenScope {
    Read,  ///< Read-only scope: allows GET/HEAD style operations
    Write  ///< Read-write scope: allows modifying operations (POST/PUT/DELETE)
  };

  // Authentication helpers
  AuthResult check_auth(const req_https_t &request);
  bool authenticate(resp_https_t response, req_https_t request);
  std::string get_client_id(const req_https_t &request);
  std::string generate_csrf_token(const std::string &client_id);
  bool validate_csrf_token(const resp_https_t &response, const req_https_t &request, const std::string &client_id);

  // Token scope helpers
  TokenScope scope_from_string(std::string_view s);
  std::string scope_to_string(TokenScope scope);

  // Web UI endpoints
  void generateApiToken(resp_https_t response, req_https_t request);
  void listApiTokens(resp_https_t response, req_https_t request);
  void revokeApiToken(resp_https_t response, req_https_t request);
  void getTokenPage(resp_https_t response, req_https_t request);
  void loginUser(resp_https_t response, req_https_t request);
  void refreshSession(resp_https_t response, req_https_t request);
  void authStatus(resp_https_t response, req_https_t request);
  void logoutUser(resp_https_t response, req_https_t request);
  void getSpaEntry(resp_https_t response, req_https_t request);
  void getCSRFToken(resp_https_t response, req_https_t request);
  void browseDirectory(resp_https_t response, req_https_t request);
  bool is_browsable_executable(const std::filesystem::directory_entry &entry, const std::filesystem::file_status &status);
  nlohmann::json build_browse_entries(const std::filesystem::path &dir_path, const std::string &type_str);
#ifdef _WIN32
  nlohmann::json get_windows_drives();
#endif

  // Writes the apps file and refreshes the client-visible app cache/list.
  bool refresh_client_apps_cache(nlohmann::json &file_tree, bool sort_by_name = true);

}  // namespace confighttp

// mime types map (defined in confighttp.cpp)
namespace confighttp {
  extern const std::map<std::string, std::string> mime_types;
}
