/**
 * @file src/confighttp.h
 * @brief Declarations for the Web UI Config HTTP server.
 */
#pragma once

// standard includes
#include <chrono>
#include <functional>
#include <map>
#include <set>
#include <chrono>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>

// third-party includes
#include <nlohmann/json.hpp>
#include <unordered_map>

// local includes
#include "http_auth.h"
#include "http_auth.h"
#include "thread_safe.h"

#include <Simple-Web-Server/server_https.hpp>

#define WEB_DIR SUNSHINE_ASSETS_DIR "/web/"

namespace confighttp {
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;

  constexpr auto PORT_HTTPS = 1;
  void start();

  // Token scopes for API tokens used by tests and UI
  enum class TokenScope {
    Read,  ///< Read-only scope: allows GET/HEAD style operations
    Write  ///< Read-write scope: allows modifying operations (POST/PUT/DELETE)
  };

  // Authentication helpers
  AuthResult check_auth(const req_https_t &request);
  bool authenticate(resp_https_t response, req_https_t request);

  // Token scope helpers
  TokenScope scope_from_string(std::string_view s);
  std::string scope_to_string(TokenScope scope);

  // Web UI endpoints
  void listApiTokens(resp_https_t response, req_https_t request);
  void revokeApiToken(resp_https_t response, req_https_t request);
  void getTokenPage(resp_https_t response, req_https_t request);
  void loginUser(resp_https_t response, req_https_t request);
  void authStatus(resp_https_t response, req_https_t request);
  void logoutUser(resp_https_t response, req_https_t request);
  void getLoginPage(resp_https_t response, req_https_t request);
  void getSpaEntry(resp_https_t response, req_https_t request);

  // Writes the apps file and refreshes the client-visible app cache/list
  // Sorts entries by name for a stable UI.
  bool refresh_client_apps_cache(nlohmann::json &file_tree);

  // Authentication functions
  AuthResult check_auth(const req_https_t &request);
  bool authenticate(resp_https_t response, req_https_t request);

  TokenScope scope_from_string(std::string_view s);
  std::string scope_to_string(TokenScope scope);

  // Web UI endpoints
  void listApiTokens(resp_https_t response, req_https_t request);
  void revokeApiToken(resp_https_t response, req_https_t request);
  void getTokenPage(resp_https_t response, req_https_t request);
  void loginUser(resp_https_t response, req_https_t request);
  void authStatus(resp_https_t response, req_https_t request);
  void logoutUser(resp_https_t response, req_https_t request);
  void getLoginPage(resp_https_t response, req_https_t request);
  void getSpaEntry(resp_https_t response, req_https_t request);

}  // namespace confighttp

// mime types map (defined in confighttp.cpp)
namespace confighttp {
  extern const std::map<std::string, std::string> mime_types;
}
