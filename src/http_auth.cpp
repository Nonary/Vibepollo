#include "http_auth.h"

#include "config.h"
#include "confighttp.h"
#include "crypto.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "state_storage.h"
#include "utility.h"

#include <algorithm>
#include <utility>
#include <boost/algorithm/string.hpp>
#include <boost/function.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/regex.hpp>
#include <chrono>
#include <ctime>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <set>
#include <Simple-Web-Server/crypto.hpp>
#include <Simple-Web-Server/server_https.hpp>
#include <string>
#include <vector>
using namespace std::literals;
namespace pt = boost::property_tree;
namespace fs = std::filesystem;

namespace confighttp {

  // Global instances for authentication
  ApiTokenManager api_token_manager;
  SessionTokenManager session_token_manager(SessionTokenManager::make_default_dependencies());
  SessionTokenAPI session_token_api(session_token_manager);

  namespace {
    constexpr std::chrono::seconds remember_me_token_ttl {std::chrono::hours(24 * 30)};

    std::string detect_os(const std::string &ua_lower) {
      using boost::algorithm::icontains;
      if (icontains(ua_lower, "windows nt 10")) {
        return "Windows 10/11";
      }
      if (icontains(ua_lower, "windows nt 6.3")) {
        return "Windows 8.1";
      }
      if (icontains(ua_lower, "windows nt 6.2")) {
        return "Windows 8";
      }
      if (icontains(ua_lower, "windows nt 6.1")) {
        return "Windows 7";
      }
      if (icontains(ua_lower, "mac os x")) {
        return "macOS";
      }
      if (icontains(ua_lower, "iphone") || icontains(ua_lower, "ipad")) {
        return "iOS";
      }
      if (icontains(ua_lower, "android")) {
        return "Android";
      }
      if (icontains(ua_lower, "linux")) {
        return "Linux";
      }
      return {};
    }

    std::string detect_browser(const std::string &ua_lower) {
      using boost::algorithm::icontains;
      if (icontains(ua_lower, "edg/")) {
        return "Microsoft Edge";
      }
      if (icontains(ua_lower, "opr/")) {
        return "Opera";
      }
      if (icontains(ua_lower, "chrome/") && !icontains(ua_lower, "edg/") && !icontains(ua_lower, "opr/")) {
        return "Google Chrome";
      }
      if (icontains(ua_lower, "firefox")) {
        return "Mozilla Firefox";
      }
      if (icontains(ua_lower, "safari") && !icontains(ua_lower, "chrome")) {
        return "Safari";
      }
      if (icontains(ua_lower, "brave")) {
        return "Brave";
      }
      return {};
    }

    std::string truncate_label(const std::string &value) {
      constexpr std::size_t kMaxLabelLength = 80;
      if (value.size() <= kMaxLabelLength) {
        return value;
      }
      return value.substr(0, kMaxLabelLength - 1) + "…";
    }

    std::string derive_device_label(const std::string &user_agent, const std::string &remote_address) {
      if (user_agent.empty()) {
        if (!remote_address.empty()) {
          return remote_address;
        }
        return "Unknown device";
      }

      std::string ua_lower = boost::algorithm::to_lower_copy(user_agent);
      std::string os = detect_os(ua_lower);
      std::string browser = detect_browser(ua_lower);

      std::string label;
      if (!browser.empty() && !os.empty()) {
        label = std::format("{} on {}", browser, os);
      } else if (!browser.empty()) {
        label = browser;
      } else if (!os.empty()) {
        label = os;
      }

      if (label.empty()) {
        label = truncate_label(user_agent);
      }

      return label;
    }

    std::string format_cookie_expires(std::chrono::system_clock::time_point tp) {
      auto tt = std::chrono::system_clock::to_time_t(tp);
      std::tm tm {};
#if defined(_WIN32)
      gmtime_s(&tm, &tt);
#else
      gmtime_r(&tt, &tm);
#endif
      char buffer[64] {};
      if (std::strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &tm) == 0) {
        return {};
      }
      return buffer;
    }
  }  // namespace

  InvalidScopeException::InvalidScopeException(const std::string &msg):
      _message(msg) {}

  const char *InvalidScopeException::what() const noexcept {
    return _message.c_str();
  }

  ApiTokenManager::ApiTokenManager(const ApiTokenManagerDependencies &dependencies):
      _dependencies(dependencies) {}

  bool ApiTokenManager::authenticate_token(const std::string &token, const std::string &path, const std::string &method) {
    std::scoped_lock lock(_mutex);
    std::string token_hash = _dependencies.hash(token);
    auto it = _api_tokens.find(token_hash);
    if (it == _api_tokens.end()) {
      return false;
    }

    std::string req_method = boost::to_upper_copy(method);

    auto is_method_allowed = [&](const std::set<std::string, std::less<>> &methods) {
      return std::ranges::any_of(methods, [&](const std::string &m) {
        return boost::iequals(m, req_method);
      });
    };

    auto regex_path_match = [](const std::string &scope_path, const std::string &request_path) {
      std::string pattern = scope_path;
      if (pattern.empty() || pattern[0] != '^') {
        pattern = "^" + pattern;
      }
      if (pattern.empty() || pattern.back() != '$') {
        pattern += "$";
      }
      boost::regex re(pattern);
      return boost::regex_match(request_path, re);
    };

    return std::ranges::any_of(it->second.path_methods, [&](const auto &pair) {
      const auto &scope_path = pair.first;
      const auto &methods = pair.second;
      return regex_path_match(scope_path, path) && is_method_allowed(methods);
    });
  }

  bool ApiTokenManager::authenticate_bearer(std::string_view raw_auth, const std::string &path, const std::string &method) {
    if (raw_auth.length() <= 7 || !raw_auth.starts_with("Bearer ")) {
      return false;
    }
    auto token = std::string(raw_auth.substr(7));
    return authenticate_token(token, path, method);
  }

  std::optional<std::string> ApiTokenManager::create_api_token(const nlohmann::json &scopes_json, const std::string &username) {
    auto path_methods = parse_json_scopes(scopes_json);
    if (!path_methods) {
      return std::nullopt;
    }
    std::string token = _dependencies.rand_alphabet(32);
    std::string token_hash = _dependencies.hash(token);
    ApiTokenInfo info {token_hash, *path_methods, username, _dependencies.now()};
    {
      std::scoped_lock lock(_mutex);
      _api_tokens[token_hash] = info;
    }
    save_api_tokens();
    return token;
  }

  std::optional<std::string> ApiTokenManager::generate_api_token(const std::string &request_body, const std::string &username) {
    nlohmann::json input;
    try {
      input = nlohmann::json::parse(request_body);
    } catch (const nlohmann::json::exception &) {
      return std::nullopt;
    }
    if (!input.contains("scopes") || !input["scopes"].is_array()) {
      return std::nullopt;
    }
    return create_api_token(input["scopes"], username);
  }

  std::optional<std::map<std::string, std::set<std::string, std::less<>>, std::less<>>>
    ApiTokenManager::parse_json_scopes(const nlohmann::json &scopes_json) const {
    std::map<std::string, std::set<std::string, std::less<>>, std::less<>> path_methods;
    try {
      for (const auto &s : scopes_json) {
        if (!s.contains("path") || !s.contains("methods") || !s["methods"].is_array()) {
          throw InvalidScopeException("Invalid scopes configured on API Token, missing 'path' or 'methods' array");
        }
        std::string path = s["path"].get<std::string>();
        std::set<std::string, std::less<>> methods;
        for (const auto &m : s["methods"]) {
          methods.insert(boost::to_upper_copy(m.get<std::string>()));
        }
        path_methods[path] = methods;
      }
    } catch (const InvalidScopeException &) {
      BOOST_LOG(warning) << "Invalid scope detected in API token, please delete and recreate the token to resolve.";
      return std::nullopt;
    }
    return path_methods;
  }

  nlohmann::json ApiTokenManager::get_api_tokens_list() const {
    nlohmann::json arr = nlohmann::json::array();
    std::scoped_lock lock(_mutex);
    for (const auto &[hash, info] : _api_tokens) {
      nlohmann::json obj;
      obj["hash"] = hash;
      obj["username"] = info.username;
      obj["created_at"] = std::chrono::duration_cast<std::chrono::seconds>(info.created_at.time_since_epoch()).count();
      obj["scopes"] = nlohmann::json::array();
      for (const auto &[path, methods] : info.path_methods) {
        obj["scopes"].push_back({{"path", path}, {"methods", methods}});
      }
      arr.push_back(obj);
    }
    return arr;
  }

  std::string ApiTokenManager::list_api_tokens_json() const {
    return get_api_tokens_list().dump();
  }

  bool ApiTokenManager::revoke_api_token_by_hash(const std::string &hash) {
    if (hash.empty()) {
      return false;
    }
    bool erased = false;
    {
      std::scoped_lock lock(_mutex);
      erased = _api_tokens.erase(hash) > 0;
    }
    if (erased) {
      save_api_tokens();
    }
    return erased;
  }

  void ApiTokenManager::save_api_tokens() const {
    statefile::migrate_recent_state_keys();
    const auto &state_path = statefile::vibeshine_state_path();

    nlohmann::json j;
    {
      std::scoped_lock lock(_mutex);
      for (const auto &[hash, info] : _api_tokens) {
        nlohmann::json obj;
        obj["hash"] = hash;
        obj["username"] = info.username;
        obj["created_at"] = std::chrono::duration_cast<std::chrono::seconds>(info.created_at.time_since_epoch()).count();
        obj["scopes"] = nlohmann::json::array();
        for (const auto &[path, methods] : info.path_methods) {
          obj["scopes"].push_back({{"path", path}, {"methods", methods}});
        }
        j.push_back(obj);
      }
    }
    std::lock_guard<std::mutex> state_lock(statefile::state_mutex());

    pt::ptree root;
    if (_dependencies.file_exists(state_path)) {
      try {
        _dependencies.read_json(state_path, root);
      } catch (...) {
        root = {};
      }
    }
    pt::ptree tokens_pt;
    for (const auto &tok : j) {
      pt::ptree t;
      t.put("hash", tok["hash"].get<std::string>());
      t.put("username", tok["username"].get<std::string>());
      t.put("created_at", tok["created_at"].get<std::int64_t>());
      pt::ptree scopes_pt;
      for (const auto &s : tok["scopes"]) {
        pt::ptree spt;
        spt.put("path", s["path"].get<std::string>());
        pt::ptree mpt;
        for (const auto &m : s["methods"]) {
          mpt.push_back({"", pt::ptree(m.get<std::string>())});
        }
        spt.add_child("methods", mpt);
        scopes_pt.push_back({"", spt});
      }
      t.add_child("scopes", scopes_pt);
      tokens_pt.push_back({"", t});
    }
    root.put_child("root.api_tokens", tokens_pt);
    _dependencies.write_json(state_path, root);
  }

  std::optional<std::pair<std::string, std::set<std::string, std::less<>>>> ApiTokenManager::parse_scope(const pt::ptree &scope_tree) const {
    const std::string path = scope_tree.get<std::string>("path", "");
    if (path.empty()) {
      return std::nullopt;
    }
    std::set<std::string, std::less<>> methods;
    if (auto m_child = scope_tree.get_child_optional("methods")) {
      for (const auto &[_, method_node] : *m_child) {
        methods.insert(method_node.data());
      }
    }
    if (methods.empty()) {
      return std::nullopt;
    }
    return std::make_pair(path, std::move(methods));
  }

  std::map<std::string, std::set<std::string, std::less<>>, std::less<>> ApiTokenManager::build_scope_map(const pt::ptree &scopes_node) const {
    std::map<std::string, std::set<std::string, std::less<>>, std::less<>> out;
    for (const auto &[_, scope_tree] : scopes_node) {
      if (auto parsed = parse_scope(scope_tree)) {
        auto [path, methods] = std::move(*parsed);
        out.try_emplace(std::move(path), std::move(methods));
      }
    }
    return out;
  }

  void ApiTokenManager::load_api_tokens() {
    statefile::migrate_recent_state_keys();
    const auto &state_path = statefile::vibeshine_state_path();

    pt::ptree root;
    bool have_root = false;
    {
      std::lock_guard<std::mutex> state_lock(statefile::state_mutex());
      if (_dependencies.file_exists(state_path)) {
        try {
          _dependencies.read_json(state_path, root);
          have_root = true;
        } catch (...) {
        }
      }
    }

    std::scoped_lock lock(_mutex);
    _api_tokens.clear();
    if (!have_root) {
      return;
    }
    if (auto tokens_node = root.get_child_optional("root.api_tokens")) {
      for (const auto &[_, token_tree] : *tokens_node) {
        ApiTokenInfo info;
        info.hash = token_tree.get<std::string>("hash", "");
        if (info.hash.empty()) {
          continue;
        }
        info.username = token_tree.get<std::string>("username", "");
        std::int64_t created_secs = token_tree.get<std::int64_t>("created_at", 0);
        info.created_at = std::chrono::system_clock::time_point(std::chrono::seconds(created_secs));
        if (auto scopes_node = token_tree.get_child_optional("scopes")) {
          info.path_methods = build_scope_map(*scopes_node);
        }
        _api_tokens.try_emplace(info.hash, std::move(info));
      }
    }
  }

  ApiTokenManagerDependencies ApiTokenManager::make_default_dependencies() {
    ApiTokenManagerDependencies dependencies;
    dependencies.file_exists = [](const std::string &path) {
      return fs::exists(path);
    };
    dependencies.read_json = [](const std::string &path, pt::ptree &tree) {
      boost::property_tree::json_parser::read_json(path, tree);
    };
    dependencies.write_json = [](const std::string &path, const pt::ptree &tree) {
      boost::property_tree::json_parser::write_json(path, tree);
    };
    dependencies.now = []() {
      return std::chrono::system_clock::now();
    };
    dependencies.rand_alphabet = [](std::size_t length) {
      return crypto::rand_alphabet(length);
    };
    dependencies.hash = [](const std::string &input) {
      return util::hex(crypto::hash(input)).to_string();
    };
    return dependencies;
  }

  const std::map<std::string, ApiTokenInfo, std::less<>> &ApiTokenManager::retrieve_loaded_api_tokens() const {
    return _api_tokens;
  }

  SessionTokenManager::SessionTokenManager(const SessionTokenManagerDependencies &dependencies):
      _dependencies(dependencies) {}

  SessionTokenManagerDependencies SessionTokenManager::make_default_dependencies() {
    SessionTokenManagerDependencies deps;
    deps.now = []() {
      return std::chrono::system_clock::now();
    };
    deps.rand_alphabet = [](std::size_t length) {
      return crypto::rand_alphabet(length);
    };
    deps.hash = [](const std::string &input) {
      auto hash_result = crypto::hash(input);
      return util::hex(hash_result).to_string();
    };
    deps.file_exists = [](const std::string &path) {
      return fs::exists(path);
    };
    deps.read_json = [](const std::string &path, pt::ptree &tree) {
      boost::property_tree::json_parser::read_json(path, tree);
    };
    deps.write_json = [](const std::string &path, const pt::ptree &tree) {
      boost::property_tree::json_parser::write_json(path, tree);
    };
    return deps;
  }

  std::string SessionTokenManager::generate_session_token(const std::string &username, std::chrono::seconds lifetime, const std::string &user_agent, const std::string &remote_address, bool remember_me) {
    std::string token = _dependencies.rand_alphabet(64);
    std::string token_hash = _dependencies.hash(token);
    auto now = _dependencies.now();
    if (lifetime <= std::chrono::seconds::zero()) {
      lifetime = config::sunshine.session_token_ttl;
    }
    auto expires = now + lifetime;
    std::string device_label = derive_device_label(user_agent, remote_address);
    {
      std::scoped_lock lock(_mutex);
      _session_tokens[token_hash] = SessionToken {username, now, expires, user_agent, remote_address, now, remember_me, device_label};
      _dirty = true;
    }
    cleanup_expired_session_tokens();
    save_session_tokens();
    return token;
  }

  bool SessionTokenManager::validate_session_token(const std::string &token) {
    bool should_persist = false;
    bool valid = false;
    {
      std::scoped_lock lock(_mutex);
      std::string token_hash = _dependencies.hash(token);
      auto it = _session_tokens.find(token_hash);
      if (it == _session_tokens.end()) {
        return false;
      }
      auto now = _dependencies.now();
      if (now > it->second.expires_at) {
        _session_tokens.erase(it);
        _dirty = true;
        should_persist = true;
        return false;
      }
      valid = true;
      if (now - it->second.last_seen >= std::chrono::minutes(5)) {
        it->second.last_seen = now;
        _dirty = true;
        should_persist = true;
      }
    }
    if (should_persist) {
      save_session_tokens();
    }
    return valid;
  }

  void SessionTokenManager::revoke_session_token(const std::string &token) {
    std::string token_hash = _dependencies.hash(token);
    revoke_session_by_hash(token_hash);
  }

  bool SessionTokenManager::revoke_session_by_hash(const std::string &token_hash) {
    bool removed = false;
    {
      std::scoped_lock lock(_mutex);
      if (_session_tokens.erase(token_hash) > 0) {
        removed = true;
        _dirty = true;
      }
    }
    if (removed) {
      save_session_tokens();
    }
    return removed;
  }

  bool SessionTokenManager::cleanup_expired_session_tokens() {
    bool removed = false;
    {
      std::scoped_lock lock(_mutex);
      auto now = _dependencies.now();
      auto erased = std::erase_if(_session_tokens, [now](const auto &pair) {
        return now > pair.second.expires_at;
      });
      if (erased > 0) {
        removed = true;
        _dirty = true;
      }
    }
    return removed;
  }

  std::optional<std::string> SessionTokenManager::get_username_for_token(const std::string &token) {
    std::optional<std::string> username;
    bool should_persist = false;
    {
      std::scoped_lock lock(_mutex);
      std::string token_hash = _dependencies.hash(token);
      auto it = _session_tokens.find(token_hash);
      if (it == _session_tokens.end()) {
        return std::nullopt;
      }
      auto now = _dependencies.now();
      if (now > it->second.expires_at) {
        _session_tokens.erase(it);
        _dirty = true;
        should_persist = true;
        return std::nullopt;
      }
      username = it->second.username;
      if (now - it->second.last_seen >= std::chrono::minutes(5)) {
        it->second.last_seen = now;
        _dirty = true;
        should_persist = true;
      }
    }
    if (should_persist) {
      save_session_tokens();
    }
    return username;
  }

  size_t SessionTokenManager::session_count() const {
    std::scoped_lock lock(_mutex);
    return _session_tokens.size();
  }

  std::optional<std::string> SessionTokenManager::get_hash_for_token(const std::string &token) const {
    std::scoped_lock lock(_mutex);
    std::string token_hash = _dependencies.hash(token);
    if (_session_tokens.find(token_hash) == _session_tokens.end()) {
      return std::nullopt;
    }
    return token_hash;
  }

  void SessionTokenManager::save_session_tokens() const {
    statefile::migrate_recent_state_keys();
    const auto &state_path = statefile::vibeshine_state_path();

    std::vector<std::pair<std::string, SessionToken>> snapshot;
    bool had_dirty = false;
    {
      std::scoped_lock lock(_mutex);
      if (!_dirty) {
        return;
      }
      snapshot.reserve(_session_tokens.size());
      for (const auto &entry : _session_tokens) {
        snapshot.push_back(entry);
      }
      _dirty = false;
      had_dirty = true;
    }

    bool write_failed = false;
    {
      std::lock_guard<std::mutex> state_lock(statefile::state_mutex());
      pt::ptree root;
      if (_dependencies.file_exists(state_path)) {
        try {
          _dependencies.read_json(state_path, root);
        } catch (const std::exception &e) {
          BOOST_LOG(warning) << "SessionTokenManager: failed reading state file: " << e.what();
          root = {};
        }
      }

      pt::ptree sessions_pt;
      for (const auto &entry : snapshot) {
        const auto &hash = entry.first;
        const auto &token = entry.second;
        pt::ptree node;
        node.put("hash", hash);
        node.put("username", token.username);
        node.put("created_at", std::chrono::duration_cast<std::chrono::seconds>(token.created_at.time_since_epoch()).count());
        node.put("expires_at", std::chrono::duration_cast<std::chrono::seconds>(token.expires_at.time_since_epoch()).count());
        node.put("last_seen", std::chrono::duration_cast<std::chrono::seconds>(token.last_seen.time_since_epoch()).count());
        node.put("remember_me", token.remember_me);
        if (!token.user_agent.empty()) {
          node.put("user_agent", token.user_agent);
        }
        if (!token.remote_address.empty()) {
          node.put("remote_address", token.remote_address);
        }
        if (!token.device_label.empty()) {
          node.put("device_label", token.device_label);
        }
        sessions_pt.push_back({"", node});
      }

      root.put_child("root.session_tokens", sessions_pt);

      try {
        _dependencies.write_json(state_path, root);
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "SessionTokenManager: failed writing state file: " << e.what();
        write_failed = true;
      }
    }

    if (had_dirty) {
      std::scoped_lock lock(_mutex);
      if (write_failed) {
        _dirty = true;
      } else if (!_dirty) {
        _last_persist = _dependencies.now();
      }
    }
  }

  void SessionTokenManager::load_session_tokens() {
    statefile::migrate_recent_state_keys();
    const auto &state_path = statefile::vibeshine_state_path();

    pt::ptree root;
    bool have_root = false;
    bool load_failed = false;
    {
      std::lock_guard<std::mutex> state_lock(statefile::state_mutex());
      if (_dependencies.file_exists(state_path)) {
        try {
          _dependencies.read_json(state_path, root);
          have_root = true;
        } catch (const std::exception &e) {
          BOOST_LOG(warning) << "SessionTokenManager: failed loading state file: " << e.what();
          load_failed = true;
        }
      }
    }

    bool needs_resave = false;
    auto now = _dependencies.now();
    {
      std::scoped_lock lock(_mutex);
      _session_tokens.clear();
      if (!have_root || load_failed) {
        _dirty = false;
        _last_persist = now;
        return;
      }
      if (auto sessions_node = root.get_child_optional("root.session_tokens")) {
        for (const auto &[_, node] : *sessions_node) {
          auto hash = node.get<std::string>("hash", "");
          if (hash.empty()) {
            continue;
          }
          SessionToken token;
          token.username = node.get<std::string>("username", "");
          auto created_secs = node.get<std::int64_t>("created_at", 0);
          token.created_at = std::chrono::system_clock::time_point(std::chrono::seconds(created_secs));
          auto expires_secs = node.get<std::int64_t>("expires_at", 0);
          token.expires_at = std::chrono::system_clock::time_point(std::chrono::seconds(expires_secs));
          auto last_seen_secs = node.get<std::int64_t>("last_seen", created_secs);
          token.last_seen = std::chrono::system_clock::time_point(std::chrono::seconds(last_seen_secs));
          token.remember_me = node.get<bool>("remember_me", false);
          token.user_agent = node.get<std::string>("user_agent", "");
          token.remote_address = node.get<std::string>("remote_address", "");
          token.device_label = node.get<std::string>("device_label", "");
          if (token.device_label.empty()) {
            token.device_label = derive_device_label(token.user_agent, token.remote_address);
            needs_resave = true;
          }
          if (now > token.expires_at) {
            continue;
          }
          _session_tokens.emplace(std::move(hash), std::move(token));
        }
      }
      _dirty = needs_resave;
      _last_persist = now;
    }
    if (needs_resave) {
      save_session_tokens();
    }
  }

  std::vector<SessionTokenView> SessionTokenManager::list_sessions(const std::string &username_filter) const {
    std::vector<SessionTokenView> out;
    std::scoped_lock lock(_mutex);
    out.reserve(_session_tokens.size());
    for (const auto &[hash, token] : _session_tokens) {
      if (!username_filter.empty() && !boost::iequals(token.username, username_filter)) {
        continue;
      }
      out.push_back(SessionTokenView {
        hash,
        token.username,
        token.created_at,
        token.expires_at,
        token.last_seen,
        token.remember_me,
        token.user_agent,
        token.remote_address,
        token.device_label,
      });
    }
    return out;
  }

  SessionTokenAPI::SessionTokenAPI(SessionTokenManager &session_manager):
      _session_manager(session_manager) {}

  APIResponse SessionTokenAPI::login(const std::string &username, const std::string &password, const std::string &redirect_url, bool remember_me, const std::string &user_agent, const std::string &remote_address) {
    if (!validate_credentials(username, password)) {
      BOOST_LOG(info) << "Web UI: Login failed for user: " << username;
      return create_error_response("Invalid credentials", StatusCode::client_error_unauthorized);
    }

    auto lifetime = remember_me ? remember_me_token_ttl : config::sunshine.session_token_ttl;
    std::string session_token = _session_manager.generate_session_token(username, lifetime, user_agent, remote_address, remember_me);

    nlohmann::json response_data;
    response_data["token"] = session_token;
    response_data["expires_in"] = std::chrono::duration_cast<std::chrono::seconds>(lifetime).count();
    response_data["remember_me"] = remember_me;

    // Hardened secure redirect handling
    std::string safe_redirect = "/";
    if (!redirect_url.empty() && redirect_url[0] == '/') {
      std::string lower = redirect_url;
      boost::algorithm::to_lower(lower);
      // Disallow dangerous patterns
      if (!boost::algorithm::contains(lower, "://") &&
          !boost::algorithm::contains(lower, "%2f") &&
          !boost::algorithm::contains(lower, "\\") &&
          !boost::algorithm::contains(lower, "/..") &&
          !(redirect_url.size() > 1 && redirect_url[1] == '/')) {  // reject double slash
        // Unicode normalization: reject if normalized path differs
        std::string norm = redirect_url;
        std::ranges::replace(norm, '\\', '/');
        if (norm == redirect_url) {
          safe_redirect = redirect_url;
        }
      }
    }
    response_data["redirect"] = safe_redirect;

    APIResponse response = create_success_response(response_data);

    // Set session cookie with Secure if HTTPS or localhost
    // Percent-encode token for safe cookie storage
    std::string encoded = http::cookie_escape(session_token);
    std::string cookie = std::string(session_cookie_name) + "=" + encoded + "; Path=/; HttpOnly; SameSite=Strict; Secure; Priority=High";
    if (remember_me) {
      cookie += std::format("; Max-Age={}", std::chrono::duration_cast<std::chrono::seconds>(lifetime).count());
      auto expires_at = std::chrono::system_clock::now() + lifetime;
      if (auto expires_str = format_cookie_expires(expires_at); !expires_str.empty()) {
        cookie += "; Expires=" + expires_str;
      }
    }
    response.headers.emplace("Set-Cookie", cookie);

    // Set CORS header for localhost only (no wildcard), dynamically set port
    std::uint16_t https_port = net::map_port(nvhttp::PORT_HTTPS);
    std::string cors_origin = std::format("https://localhost:{}", https_port);
    response.headers.emplace("Access-Control-Allow-Origin", cors_origin);

    return response;
  }

  APIResponse SessionTokenAPI::logout(const std::string &session_token) {
    if (!session_token.empty()) {
      _session_manager.revoke_session_token(session_token);
    }

    nlohmann::json response_data;
    response_data["message"] = "Logged out successfully";

    // Create success response and clear the session cookie so the client no longer retains it
    APIResponse response = create_success_response(response_data);
    // Set-Cookie header to clear the session token on client
    std::string clear_cookie = std::string(session_cookie_name) + "=; Path=/; HttpOnly; SameSite=Strict; Secure; Priority=High; Expires=Thu, 01 Jan 1970 00:00:00 GMT; Max-Age=0";
    response.headers.emplace("Set-Cookie", clear_cookie);
    return response;
  }

  APIResponse SessionTokenAPI::validate_session(const std::string &session_token) {
    if (session_token.empty()) {
      return create_error_response("Session token required", SimpleWeb::StatusCode::client_error_unauthorized);
    }

    if (bool is_valid = _session_manager.validate_session_token(session_token); !is_valid) {
      return create_error_response("Invalid or expired session token", SimpleWeb::StatusCode::client_error_unauthorized);
    }

    return create_success_response();
  }

  APIResponse SessionTokenAPI::list_sessions(const std::string &username_filter, const std::string &active_session_hash) const {
    auto sessions = _session_manager.list_sessions(username_filter);
    nlohmann::json response;
    response["sessions"] = nlohmann::json::array();

    auto to_seconds = [](const std::chrono::system_clock::time_point &tp) {
      return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
    };

    for (const auto &session : sessions) {
      nlohmann::json entry;
      entry["id"] = session.hash;
      entry["username"] = session.username;
      entry["created_at"] = to_seconds(session.created_at);
      entry["expires_at"] = to_seconds(session.expires_at);
      entry["last_seen"] = to_seconds(session.last_seen);
      entry["remember_me"] = session.remember_me;
      entry["current"] = (!active_session_hash.empty() && active_session_hash == session.hash);
      if (!session.user_agent.empty()) {
        entry["user_agent"] = session.user_agent;
      }
      if (!session.remote_address.empty()) {
        entry["remote_address"] = session.remote_address;
      }
      if (!session.device_label.empty()) {
        entry["device_label"] = session.device_label;
      }
      response["sessions"].push_back(std::move(entry));
    }

    return create_success_response(response);
  }

  APIResponse SessionTokenAPI::revoke_session_by_hash(const std::string &session_hash) {
    if (session_hash.empty()) {
      return create_error_response("Session identifier required", StatusCode::client_error_bad_request);
    }
    if (!_session_manager.revoke_session_by_hash(session_hash)) {
      return create_error_response("Session not found", StatusCode::client_error_not_found);
    }
    nlohmann::json data;
    data["message"] = "Session revoked";
    return create_success_response(data);
  }

  bool SessionTokenAPI::validate_credentials(const std::string &username, const std::string &password) const {
    if (auto hash = util::hex(crypto::hash(password + config::sunshine.salt)).to_string();
        !boost::iequals(username, config::sunshine.username) || hash != config::sunshine.password) {
      return false;
    }
    return true;
  }

  namespace {
    std::string get_cors_origin() {
      std::uint16_t https_port = net::map_port(confighttp::PORT_HTTPS);
      return std::format("https://localhost:{}", https_port);
    }
  }  // namespace

  APIResponse SessionTokenAPI::create_success_response(const nlohmann::json &data) const {
    nlohmann::json response_body;
    response_body["status"] = true;
    for (auto &[key, value] : data.items()) {
      response_body[key] = value;
    }
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("Access-Control-Allow-Origin", get_cors_origin());
    return APIResponse(StatusCode::success_ok, response_body.dump(), headers);
  }

  APIResponse SessionTokenAPI::create_error_response(const std::string &error_message, StatusCode status_code) const {
    nlohmann::json response_body;
    response_body["status"] = false;
    response_body["error"] = error_message;
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("Access-Control-Allow-Origin", get_cors_origin());
    return APIResponse(status_code, response_body.dump(), headers);
  }

  AuthResult make_auth_error(StatusCode code, const std::string &error) {
    AuthResult result {false, code, {}, {}};
    // Always add CORS origin header for auth errors (and redirects) to satisfy UI expectations
    {
      std::uint16_t https_port = net::map_port(confighttp::PORT_HTTPS);
      std::string cors_origin = std::format("https://localhost:{}", https_port);
      result.headers.emplace("Access-Control-Allow-Origin", cors_origin);
    }
    if (!error.empty()) {
      nlohmann::json j;
      j["status"] = false;
      j["error"] = error;
      result.body = j.dump();
      result.headers.emplace("Content-Type", "application/json");
    }
    return result;
  }

  AuthResult make_basic_auth_error(const std::string &error_message = "Unauthorized") {
    AuthResult result = make_auth_error(StatusCode::client_error_unauthorized, error_message);
    result.headers.emplace("WWW-Authenticate", "Basic realm=\"Sunshine\"");
    return result;
  }

  namespace {
    std::optional<std::pair<std::string, std::string>> parse_basic_credentials(const std::string &raw_auth) {
      auto space_pos = raw_auth.find(' ');
      if (space_pos == std::string::npos) {
        return std::nullopt;
      }
      std::string scheme = raw_auth.substr(0, space_pos);
      if (!boost::iequals(scheme, "Basic")) {
        return std::nullopt;
      }
      std::string encoded = raw_auth.substr(space_pos + 1);
      if (encoded.empty()) {
        return std::nullopt;
      }
      try {
        std::string decoded = SimpleWeb::Crypto::Base64::decode(encoded);
        auto colon_pos = decoded.find(':');
        if (colon_pos == std::string::npos) {
          return std::nullopt;
        }
        return std::make_pair(decoded.substr(0, colon_pos), decoded.substr(colon_pos + 1));
      } catch (...) {
        return std::nullopt;
      }
    }

    bool validate_basic_credentials(const std::string &username, const std::string &password) {
      if (config::sunshine.username.empty()) {
        return false;
      }
      auto hashed = util::hex(crypto::hash(password + config::sunshine.salt)).to_string();
      return boost::iequals(username, config::sunshine.username) && hashed == config::sunshine.password;
    }
  }  // namespace

  AuthResult check_basic_auth(const std::string &raw_auth) {
    if (auto credentials = parse_basic_credentials(raw_auth);
        credentials && validate_basic_credentials(credentials->first, credentials->second)) {
      return {true, StatusCode::success_ok, {}, {}};
    }
    return make_basic_auth_error("Unauthorized");
  }

  AuthResult check_bearer_auth(const std::string &raw_auth, const std::string &path, const std::string &method) {
    if (!api_token_manager.authenticate_bearer(raw_auth, path, method)) {
      return make_auth_error(StatusCode::client_error_forbidden, "Forbidden: Token does not have permission for this path/method.");
    }
    return {true, StatusCode::success_ok, {}, {}};
  }

  AuthResult check_session_auth(const std::string &raw_auth) {
    if (raw_auth.rfind("Session ", 0) != 0) {
      return make_auth_error(StatusCode::client_error_unauthorized, "Invalid session token format");
    }

    std::string token = raw_auth.substr(8);

    if (APIResponse api_response = session_token_api.validate_session(token); api_response.status_code == StatusCode::success_ok) {
      return {true, StatusCode::success_ok, {}, {}};
    }

    return make_auth_error(StatusCode::client_error_unauthorized, "Invalid or expired session token");
  }

  bool is_html_request(const std::string &path) {
    // API requests start with /api/
    if (path.rfind("/api/", 0) == 0) {
      return false;
    }

    // Asset requests in known directories
    if (path.rfind("/assets/", 0) == 0 || path.rfind("/images/", 0) == 0) {
      return false;
    }

    // Static file extensions should not be treated as HTML
    {
      std::string ext = std::filesystem::path(path).extension().string();
      boost::algorithm::to_lower(ext);
      static const std::vector<std::string> non_html_ext = {".js", ".css", ".map", ".json", ".woff", ".woff2", ".ttf", ".eot", ".ico", ".png", ".jpg", ".jpeg", ".svg"};
      if (std::ranges::find(non_html_ext, ext) != non_html_ext.end()) {
        return false;
      }
    }

    // Everything else is likely an HTML page request
    return true;
  }

  AuthResult check_auth(const std::string &remote_address, const std::string &auth_header, const std::string &path, const std::string &method) {
    // Strip query string from path for matching
    auto base_path = path;
    if (auto qpos = base_path.find('?'); qpos != std::string::npos) {
      base_path.resize(qpos);
    }

    // Allow welcome page without authentication
    if (base_path == "/welcome" || base_path == "/welcome/") {
      return {true, StatusCode::success_ok, {}, {}};
    }

    if (auto ip_type = net::from_address(remote_address); ip_type > http::origin_web_ui_allowed) {
      BOOST_LOG(info) << "Web UI: ["sv << remote_address << "] -- denied"sv;
      return make_auth_error(StatusCode::client_error_forbidden, "Forbidden");
    }

    // If no username configured yet, allow unauthenticated access so SPA can drive setup (except protected APIs later)
    bool credentials_configured = !config::sunshine.username.empty();

    // Only protect /api/ endpoints (except auth endpoints) for SPA model; all other paths (HTML shell, assets) are always allowed
    bool is_api = base_path.rfind("/api/", 0) == 0;
    bool is_auth_api = (base_path == "/api/auth/login" || base_path == "/api/auth/logout");
    if (!is_api) {
      return {true, StatusCode::success_ok, {}, {}};  // public content served; SPA handles routing and will trigger API calls
    }
    if (is_auth_api) {
      return {true, StatusCode::success_ok, {}, {}};  // login/logout endpoints public (logout will no-op if no token)
    }

    // From here on: /api/ (non-auth) endpoints must have credentials configured and valid session or bearer token
    if (!credentials_configured) {
      return make_auth_error(StatusCode::client_error_unauthorized, "Credentials not configured");
    }

    if (auth_header.empty()) {
      return make_auth_error(StatusCode::client_error_unauthorized, "Unauthorized");
    }

    if (auth_header.rfind("Bearer ", 0) == 0) {
      return check_bearer_auth(auth_header, path, method);
    }

    if (auth_header.rfind("Session ", 0) == 0) {
      {
        auto session_res = check_session_auth(auth_header);
        if (!session_res.ok) {
          return make_auth_error(StatusCode::client_error_unauthorized, "Invalid or expired session token");
        }
        return session_res;
      }
    }

    if (auto scheme_end = auth_header.find(' ');
        scheme_end != std::string::npos && boost::iequals(auth_header.substr(0, scheme_end), "Basic")) {
      return check_basic_auth(auth_header);
    }

    // Default: unauthorized
    return make_auth_error(StatusCode::client_error_unauthorized, "Unauthorized");
  }

  std::string extract_session_token_from_cookie(const SimpleWeb::CaseInsensitiveMultimap &headers) {
    if (auto cookie_it = headers.find("Cookie"); cookie_it != headers.end()) {
      const std::string &cookies = cookie_it->second;
      const std::string prefix = std::string(session_cookie_name) + "=";
      auto pos = cookies.find(prefix);
      if (pos != std::string::npos) {
        pos += prefix.size();
        auto end = cookies.find(';', pos);
        // Decode percent-encoded session token
        auto raw = cookies.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        return http::cookie_unescape(raw);
      }
    }
    return {};
  }

}  // namespace confighttp
