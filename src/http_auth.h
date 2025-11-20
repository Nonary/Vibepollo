#pragma once
/**
 * @file src/http_auth.h
 * @brief Declarations for HTTP authentication, API tokens, and session token management utilities.
 */
// standard includes
#include <chrono>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// local includes
#include "config.h"
#include "crypto.h"
#include "utility.h"

// platform includes
#include <boost/function.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <nlohmann/json.hpp>
#include <Simple-Web-Server/server_https.hpp>

namespace confighttp {
  inline constexpr std::string_view session_cookie_name {"__Host-sunshine_session"};
  inline constexpr std::string_view refresh_cookie_name {"__Host-sunshine_refresh"};
  using StatusCode = SimpleWeb::StatusCode;
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;

  struct AuthResult {
    bool ok;
    StatusCode code;
    std::string body;
    SimpleWeb::CaseInsensitiveMultimap headers;
  };

  struct ApiTokenManagerDependencies {
    boost::function<bool(const std::string &)> file_exists;
    boost::function<void(const std::string &, boost::property_tree::ptree &)> read_json;
    boost::function<void(const std::string &, const boost::property_tree::ptree &)> write_json;
    boost::function<std::chrono::system_clock::time_point()> now;
    boost::function<std::string(std::size_t)> rand_alphabet;
    boost::function<std::string(const std::string &)> hash;
    ApiTokenManagerDependencies() = default;
    ApiTokenManagerDependencies(const ApiTokenManagerDependencies &) = default;
    ApiTokenManagerDependencies &operator=(const ApiTokenManagerDependencies &) = default;
    ApiTokenManagerDependencies(ApiTokenManagerDependencies &&) noexcept = default;
    ApiTokenManagerDependencies &operator=(ApiTokenManagerDependencies &&) noexcept = default;
  };

  struct ApiTokenInfo {
    std::string hash;
    std::map<std::string, std::set<std::string, std::less<>>, std::less<>> path_methods;
    std::string username;
    std::chrono::system_clock::time_point created_at;
  };

  class InvalidScopeException: public std::exception {
  public:
    /**
     * @brief Construct with a human readable message.
     * @param msg Explanation of the invalid scope condition.
     */
    explicit InvalidScopeException(const std::string &msg);
    /**
     * @brief Describe the error.
     * @return Null terminated message string.
     */
    const char *what() const noexcept override;

  private:
    std::string _message;  ///< Stored explanatory message
  };

  class ApiTokenManager {
  public:
    /**
     * @brief Construct a manager with injected dependencies.
     * @param dependencies Dependency functors (defaults to production implementations).
     */
    explicit ApiTokenManager(const ApiTokenManagerDependencies &dependencies = ApiTokenManager::make_default_dependencies());
    /**
     * @brief Authenticate a Bearer token for a given path and method.
     * @param raw_auth Raw Authorization header value (e.g. "Bearer <token>").
     * @param path Requested URI path.
     * @param method HTTP method (e.g. GET / POST ...).
     * @return `true` if authorized for the requested scope.
     */
    bool authenticate_bearer(std::string_view raw_auth, const std::string &path, const std::string &method);
    /**
     * @brief Generate a new API token from a creation request body.
     * @param request_body JSON string describing requested scopes.
     * @param username User who owns the token.
     * @return The plain token string (not hashed) or `std::nullopt` on failure.
     */
    std::optional<std::string> generate_api_token(const std::string &request_body, const std::string &username);
    /**
     * @brief Serialize the loaded API tokens to JSON.
     * @return JSON string listing tokens (hashed) and metadata.
     */
    std::string list_api_tokens_json() const;
    /**
     * @brief Authenticate using a raw token (not hash) for path & method.
     * @param token The un-hashed provided token.
     * @param path Target path.
     * @param method HTTP method.
     * @return `true` if token exists and includes the scope.
     */
    bool authenticate_token(const std::string &token, const std::string &path, const std::string &method);
    /**
     * @brief Create and store a token from structured JSON scopes.
     * @param scopes_json JSON specifying scopes (path->methods mapping).
     * @param username Owner username.
     * @return The plain token value, or `std::nullopt` if validation fails.
     */
    std::optional<std::string> create_api_token(const nlohmann::json &scopes_json, const std::string &username);
    /**
     * @brief Get a structured list of tokens.
     * @return JSON array/object of tokens and properties.
     */
    nlohmann::json get_api_tokens_list() const;
    /**
     * @brief Revoke a token by hash (mutable operation).
     * @param hash Hash of the token.
     * @return `true` if removed.
     */
    bool revoke_api_token_by_hash(const std::string &hash);
    /**
     * @brief Persist tokens to backing storage.
     */
    void save_api_tokens() const;
    /**
     * @brief Load tokens from backing storage.
     */
    void load_api_tokens();
    /**
     * @brief Provide default dependency functors.
     * @return Dependencies configured with production behaviors.
     */
    static ApiTokenManagerDependencies make_default_dependencies();
    /**
     * @brief Access currently loaded tokens (thread-safe for reading via mutex).
     * @return Map of token hash to token info.
     */
    const std::map<std::string, ApiTokenInfo, std::less<>> &retrieve_loaded_api_tokens() const;

  private:
    /**
     * @brief Parse scope JSON into internal map form.
     * @param scopes_json Incoming JSON object.
     * @return Scope map or `std::nullopt` on validation error.
     */
    std::optional<std::map<std::string, std::set<std::string, std::less<>>, std::less<>>>
      parse_json_scopes(const nlohmann::json &scopes_json) const;
    /**
     * @brief Parse a single scope entry from a property tree representation.
     * @param scope_tree Scope node tree.
     * @return Pair of path and method set or `std::nullopt` if invalid.
     */
    std::optional<std::pair<std::string, std::set<std::string, std::less<>>>> parse_scope(const boost::property_tree::ptree &scope_tree) const;
    /**
     * @brief Build scope mapping from PT subtree.
     * @param scopes_node Node containing scopes array/objects.
     * @return Map path->allowed methods set.
     */
    std::map<std::string, std::set<std::string, std::less<>>, std::less<>> build_scope_map(const boost::property_tree::ptree &scopes_node) const;
    ApiTokenManagerDependencies _dependencies;  ///< Injected dependencies
    mutable std::mutex _mutex;  ///< Guards token container
    std::map<std::string, ApiTokenInfo, std::less<>> _api_tokens;  ///< Token storage keyed by hash
  };

  struct SessionToken {
    std::string username;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point expires_at;
    std::chrono::system_clock::time_point refresh_expires_at;
    std::string user_agent;
    std::string remote_address;
    std::chrono::system_clock::time_point last_seen;
    bool remember_me;
    std::string device_label;
    std::string refresh_token_hash;
    std::string rotation_id;
  };

  struct SessionTokenView {
    std::string hash;
    std::string username;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point expires_at;
    std::chrono::system_clock::time_point last_seen;
    bool remember_me;
    std::string user_agent;
    std::string remote_address;
    std::string device_label;
  };

  struct SessionTokenBundle {
    std::string session_token;
    std::string refresh_token;
    std::chrono::seconds session_ttl;
    std::chrono::seconds refresh_ttl;
    bool remember_me;
  };

  struct SessionTokenManagerDependencies {
    boost::function<std::chrono::system_clock::time_point()> now;
    boost::function<std::string(std::size_t)> rand_alphabet;
    boost::function<std::string(const std::string &)> hash;
    boost::function<bool(const std::string &)> file_exists;
    boost::function<void(const std::string &, boost::property_tree::ptree &)> read_json;
    boost::function<void(const std::string &, const boost::property_tree::ptree &)> write_json;
    SessionTokenManagerDependencies() = default;
    SessionTokenManagerDependencies(const SessionTokenManagerDependencies &) = default;
    SessionTokenManagerDependencies &operator=(const SessionTokenManagerDependencies &) = default;
    SessionTokenManagerDependencies(SessionTokenManagerDependencies &&) noexcept = default;
    SessionTokenManagerDependencies &operator=(SessionTokenManagerDependencies &&) noexcept = default;
  };

  class SessionTokenManager {
  public:
    /**
     * @brief Construct with injected dependencies.
     * @param dependencies Dependency functors (defaults available).
     */
    explicit SessionTokenManager(const SessionTokenManagerDependencies &dependencies = SessionTokenManager::make_default_dependencies());
    /**
     * @brief Create a new session token for a user.
     * @param username Account name.
     * @param lifetime Desired lifetime; defaults to config value when zero.
     * @return Newly generated opaque token string.
     */
    std::string generate_session_token(const std::string &username, std::chrono::seconds lifetime = std::chrono::seconds::zero(), const std::string &user_agent = std::string {}, const std::string &remote_address = std::string {}, bool remember_me = false);
    /**
     * @brief Issue a new access + refresh token pair.
     * @param username Account to bind.
     * @param session_ttl Optional access token lifetime (falls back to config when zero).
     * @param refresh_ttl Optional refresh token lifetime (defaults to sliding/remember-me policy).
     * @param user_agent Reported user agent string.
     * @param remote_address Source address.
     * @param remember_me Whether to extend refresh lifetime for long-lived device trust.
     * @return Bundle containing raw tokens and effective TTLs.
     */
    SessionTokenBundle issue_session_tokens(const std::string &username, std::chrono::seconds session_ttl = std::chrono::seconds::zero(), std::chrono::seconds refresh_ttl = std::chrono::seconds::zero(), const std::string &user_agent = std::string {}, const std::string &remote_address = std::string {}, bool remember_me = false);
    /**
     * @brief Refresh a session using a refresh token.
     * @param refresh_token Raw refresh token value.
     * @param user_agent Current user agent (for device labeling).
     * @param remote_address Current client address.
     * @return New token bundle if valid; std::nullopt otherwise.
     */
    std::optional<SessionTokenBundle> refresh_session_tokens(const std::string &refresh_token, const std::string &user_agent = std::string {}, const std::string &remote_address = std::string {});
    /**
     * @brief Validate a session token (and update internal state if expired).
     * @param token Opaque token string.
     * @return `true` if exists and not expired.
     */
    bool validate_session_token(const std::string &token);
    /**
     * @brief Revoke (delete) a session token.
     * @param token Token to remove.
     */
    void revoke_session_token(const std::string &token);
    /**
     * @brief Remove all expired tokens.
     */
    bool cleanup_expired_session_tokens();
    /**
     * @brief Lookup username for a token.
     * @param token Token string.
     * @return Username or `std::nullopt` if not found/expired.
     */
    std::optional<std::string> get_username_for_token(const std::string &token);
    std::optional<std::string> get_hash_for_token(const std::string &token) const;
    /**
     * @brief Count active session tokens.
     * @return Number of stored (possibly unexpired) sessions.
     */
    size_t session_count() const;
    /**
     * @brief Create default dependency set.
     * @return Dependency functors.
     */
    static SessionTokenManagerDependencies make_default_dependencies();
    /**
     * @brief Persist session tokens to disk.
     */
    void save_session_tokens() const;
    /**
     * @brief Load session tokens from disk, discarding expired entries.
     */
    void load_session_tokens();
    /**
     * @brief List session tokens, optionally filtered by username.
     * @param username_filter Case-insensitive user filter (empty for all).
     * @return Snapshot list of sessions.
     */
    std::vector<SessionTokenView> list_sessions(const std::string &username_filter = std::string {}) const;
    /**
     * @brief Revoke a session token by its stored hash.
     * @param token_hash Hashed token identifier.
     * @return `true` if a session was removed.
     */
    bool revoke_session_by_hash(const std::string &token_hash);
    /**
     * @brief Revoke a session using its refresh token value.
     * @param refresh_token Raw refresh token.
     * @return `true` if a session was removed.
     */
    bool revoke_refresh_token(const std::string &refresh_token);

  private:
    SessionTokenManagerDependencies _dependencies;  ///< Injected dependencies
    mutable std::mutex _mutex;  ///< Guards session token map

    struct TransparentStringHash {
      using is_transparent = void;

      std::size_t operator()(std::string_view txt) const noexcept {
        return std::hash<std::string_view> {}(txt);
      }

      std::size_t operator()(const std::string &txt) const noexcept {
        return std::hash<std::string_view> {}(txt);
      }

      std::size_t operator()(const char *txt) const noexcept {
        return std::hash<std::string_view> {}(txt);
      }
    };

    std::unordered_map<std::string, SessionToken, TransparentStringHash, std::equal_to<>> _session_tokens;  ///< Active session tokens keyed by hash
    std::unordered_map<std::string, std::string, TransparentStringHash, std::equal_to<>> _refresh_index;  ///< Maps refresh token hash -> session hash
    mutable bool _dirty = false;  ///< Tracks whether persistence is required
    mutable std::chrono::system_clock::time_point _last_persist;  ///< Last persistence timestamp
  };

  struct APIResponse {
    StatusCode status_code;
    std::string body;
    SimpleWeb::CaseInsensitiveMultimap headers;

    /**
     * @brief Construct a response object.
     * @param code HTTP status code.
     * @param response_body Body payload.
     * @param response_headers Extra headers (case-insensitive multimap).
     */
    APIResponse(StatusCode code, std::string response_body = "", SimpleWeb::CaseInsensitiveMultimap response_headers = {}):
        status_code(code),
        body(std::move(response_body)),
        headers(std::move(response_headers)) {}
  };

  class SessionTokenAPI {
  public:
    /**
     * @brief Construct API facade referencing a session manager.
     * @param session_manager Session manager instance.
     */
    explicit SessionTokenAPI(SessionTokenManager &session_manager);
    /**
     * @brief Handle login request validating credentials and returning session token.
     * @param username User name.
     * @param password Plain password candidate.
     * @param redirect_url Optional redirect URL for HTML flows.
     * @param remember_me Whether the session should persist beyond the browser session.
     * @return API response containing token or error.
     */
    APIResponse login(const std::string &username, const std::string &password, const std::string &redirect_url = "/", bool remember_me = false, const std::string &user_agent = std::string {}, const std::string &remote_address = std::string {});
    /**
     * @brief Invalidate the provided session token.
     * @param session_token Token to revoke.
     * @return Success or error response.
     */
    APIResponse logout(const std::string &session_token, const std::string &refresh_token = std::string {});
    /**
     * @brief Validate a session token for an active session.
     * @param session_token Token to validate.
     * @return Response containing validity state.
     */
    APIResponse validate_session(const std::string &session_token);
    /**
     * @brief Refresh a session using a refresh token.
     * @param refresh_token Refresh token value from cookie/header/body.
     * @param user_agent User agent to associate with the renewed session.
     * @param remote_address Remote address for device labeling.
     * @return API response with new cookies and token payload.
     */
    APIResponse refresh_session(const std::string &refresh_token, const std::string &user_agent, const std::string &remote_address);
    /**
     * @brief List active sessions for the current user.
     * @param username_filter Optional username filter.
     * @param active_session_hash Hash of the session making the request.
     * @return Response with session list payload.
     */
    APIResponse list_sessions(const std::string &username_filter, const std::string &active_session_hash = std::string {}) const;
    /**
     * @brief Revoke a session identified by its hash.
     * @param session_hash Target session hash.
     * @return Success or error response.
     */
    APIResponse revoke_session_by_hash(const std::string &session_hash);

  private:
    SessionTokenManager &_session_manager;  ///< Managed session token store reference
    /**
     * @brief Validate supplied credentials (implementation-defined policy).
     * @param username Name.
     * @param password Password candidate.
     * @return `true` if credentials accepted.
     */
    bool validate_credentials(const std::string &username, const std::string &password) const;
    /**
     * @brief Build a success JSON response.
     * @param data Additional JSON payload.
     * @return Response with status `200` and JSON body.
     */
    APIResponse create_success_response(const nlohmann::json &data = {}) const;
    /**
     * @brief Build an error JSON response.
     * @param error_message Human readable error.
     * @param status_code HTTP status code (default 400).
     * @return Response with error payload.
     */
    APIResponse create_error_response(const std::string &error_message, StatusCode status_code = StatusCode::client_error_bad_request) const;
  };

  /**
   * @brief Construct an AuthResult representing an authentication error (no Basic auth support).
   * @param code HTTP status to return.
   * @param error Message body.
   * @return Structured AuthResult.
   */
  AuthResult make_auth_error(StatusCode code, const std::string &error);
  /**
   * @brief Check a Bearer token Authorization header.
   * @param raw_auth Header value.
   * @param path Requested path.
   * @param method HTTP method.
   * @return AuthResult with authorization outcome.
   */
  AuthResult check_bearer_auth(const std::string &raw_auth, const std::string &path, const std::string &method);
  /**
   * @brief Check session cookie / header authentication.
   * @param raw_auth Raw header or cookie string.
   * @return AuthResult with outcome.
   */
  AuthResult check_session_auth(const std::string &raw_auth);
  /**
   * @brief Determine whether a request path expects an HTML response.
   * @param path Requested URI path.
   * @return `true` if path indicates HTML content negotiation.
   */
  bool is_html_request(const std::string &path);
  /**
   * @brief Perform layered auth checks (session, bearer, basic) for a request.
   * @param remote_address Client IP / address.
   * @param auth_header Authorization header value.
   * @param path Request path.
   * @param method HTTP method.
   * @return AuthResult representing authentication decision.
   */
  AuthResult check_auth(const std::string &remote_address, const std::string &auth_header, const std::string &path, const std::string &method);
  /**
   * @brief Extract session token from Cookie headers.
   * @param headers Case-insensitive header multimap.
   * @return Token string (may be empty if not present).
   */
  std::string extract_session_token_from_cookie(const SimpleWeb::CaseInsensitiveMultimap &headers);
  /**
   * @brief Extract refresh token from Cookie headers.
   * @param headers Case-insensitive header multimap.
   * @return Token string (may be empty if not present).
   */
  std::string extract_refresh_token_from_cookie(const SimpleWeb::CaseInsensitiveMultimap &headers);
  extern ApiTokenManager api_token_manager;
  extern SessionTokenManager session_token_manager;
  extern SessionTokenAPI session_token_api;

}  // namespace confighttp
