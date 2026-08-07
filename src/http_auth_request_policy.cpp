#include "http_auth_request_policy.h"

#include <boost/algorithm/string.hpp>
#include <filesystem>
#include <format>
#include <nlohmann/json.hpp>
#include <ranges>

namespace confighttp::policy {
  AuthResult RequestAuthPolicy::make_error(StatusCode code, const std::string &error) const {
    AuthResult result {false, code, {}, {}};
    result.headers.emplace("Access-Control-Allow-Origin", std::format("https://localhost:{}", _dependencies.https_port()));
    if (!error.empty()) {
      result.body = nlohmann::json {{"status", false}, {"error", error}}.dump();
      result.headers.emplace("Content-Type", "application/json");
    }
    return result;
  }

  AuthResult RequestAuthPolicy::basic_error() const {
    auto result = make_error(StatusCode::client_error_unauthorized, "Unauthorized");
    result.headers.emplace("WWW-Authenticate", "Basic realm=\"Sunshine\"");
    return result;
  }

  AuthResult RequestAuthPolicy::check_basic(const std::string &raw_auth) const {
    const auto space = raw_auth.find(' ');
    if (space == std::string::npos || !boost::iequals(raw_auth.substr(0, space), "Basic")) return basic_error();
    try {
      const auto decoded = _dependencies.decode_base64(raw_auth.substr(space + 1));
      const auto colon = decoded.find(':');
      if (colon == std::string::npos || !_dependencies.credentials_valid(decoded.substr(0, colon), decoded.substr(colon + 1))) return basic_error();
    } catch (...) { return basic_error(); }
    return {true, StatusCode::success_ok, {}, {}};
  }

  AuthResult RequestAuthPolicy::check_bearer(const std::string &raw_auth, const std::string &path, const std::string &method) const {
    if (!_dependencies.bearer_valid(raw_auth, path, method)) return make_error(StatusCode::client_error_forbidden, "Forbidden: Token does not have permission for this path/method.");
    return {true, StatusCode::success_ok, {}, {}};
  }

  AuthResult RequestAuthPolicy::check_session(const std::string &raw_auth) const {
    if (!raw_auth.starts_with("Session ")) return make_error(StatusCode::client_error_unauthorized, "Invalid session token format");
    if (!_dependencies.session_valid(raw_auth.substr(8))) return make_error(StatusCode::client_error_unauthorized, "Invalid or expired session token");
    return {true, StatusCode::success_ok, {}, {}};
  }

  bool RequestAuthPolicy::is_html_request(const std::string &path) const {
    if (path.starts_with("/api/") || path.starts_with("/assets/") || path.starts_with("/images/")) return false;
    auto ext = boost::algorithm::to_lower_copy(std::filesystem::path(path).extension().string());
    static const std::vector<std::string> non_html {".js", ".css", ".map", ".json", ".woff", ".woff2", ".ttf", ".eot", ".ico", ".png", ".jpg", ".jpeg", ".svg"};
    return std::ranges::find(non_html, ext) == non_html.end();
  }

  AuthResult RequestAuthPolicy::check(const std::string &remote_address, const std::string &auth_header, const std::string &path, const std::string &method) const {
    auto base_path = path.substr(0, path.find('?'));
    if (base_path == "/welcome" || base_path == "/welcome/") return {true, StatusCode::success_ok, {}, {}};
    if (!_dependencies.remote_allowed(remote_address)) return make_error(StatusCode::client_error_forbidden, "Forbidden");
    const bool is_api = base_path.starts_with("/api/");
    if (!is_api || base_path == "/api/auth/login" || base_path == "/api/auth/logout" || base_path == "/api/csrf-token") return {true, StatusCode::success_ok, {}, {}};
    if (!_dependencies.credentials_configured()) return make_error(StatusCode::client_error_unauthorized, "Credentials not configured");
    if (auth_header.empty()) return make_error(StatusCode::client_error_unauthorized, "Unauthorized");
    if (auth_header.starts_with("Bearer ")) return check_bearer(auth_header, path, method);
    if (auth_header.starts_with("Session ")) return check_session(auth_header);
    if (const auto space = auth_header.find(' '); space != std::string::npos && boost::iequals(auth_header.substr(0, space), "Basic")) return check_basic(auth_header);
    return make_error(StatusCode::client_error_unauthorized, "Unauthorized");
  }

  std::string RequestAuthPolicy::extract_cookie(const SimpleWeb::CaseInsensitiveMultimap &headers, std::string_view name) const {
    const auto it = headers.find("Cookie");
    if (it == headers.end()) return {};
    const auto prefix = std::string(name) + "=";
    auto start = it->second.find(prefix);
    if (start == std::string::npos) return {};
    start += prefix.size();
    const auto end = it->second.find(';', start);
    return _dependencies.cookie_unescape(it->second.substr(start, end == std::string::npos ? std::string::npos : end - start));
  }
}  // namespace confighttp::policy
