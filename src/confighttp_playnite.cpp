/**
 * @file src/confighttp_playnite.cpp
 * @brief Playnite-specific HTTP endpoints and helpers (Windows-only).
 */

#ifdef _WIN32

  // standard includes
  #include <algorithm>
  #include <array>
  #include <chrono>
  #include <cstdint>
  #include <filesystem>
  #include <fstream>
  #include <limits>
  #include <mutex>
  #include <optional>
  #include <sstream>
  #include <string>
  #include <string_view>
  #include <unordered_set>
  #include <vector>
  #include <cwctype>

  // third-party includes
  #include <nlohmann/json.hpp>
  #include <Simple-Web-Server/server_https.hpp>
  #include <zlib.h>
  #include <boost/property_tree/json_parser.hpp>
  #include <boost/property_tree/ptree.hpp>

  // local includes
  #include "config_playnite.h"
  #include "confighttp.h"
  #include "logging.h"
  #include "state_storage.h"
  #include "src/platform/windows/ipc/misc_utils.h"
  #include "src/platform/windows/playnite_integration.h"

  // Windows headers
  #include <KnownFolders.h>
  #include <ShlObj.h>
  #include <windows.h>

  // boost
  #include <boost/crc.hpp>

namespace confighttp {

  // Bring request/response types into scope to match confighttp.cpp usage
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;

  // Forward declarations for helpers defined in confighttp.cpp
  bool authenticate(resp_https_t response, req_https_t request);
  void print_req(const req_https_t &request);
  void send_response(resp_https_t response, const nlohmann::json &output_tree);
  void bad_request(resp_https_t response, req_https_t request, const std::string &error_message = "Bad Request");
  bool check_content_type(resp_https_t response, req_https_t request, const std::string_view &contentType);

  // Helper: check if the Sunshine Playnite plugin is installed (by presence of files)
  static bool is_plugin_installed() {
    try {
      std::string destPath;
      if (!platf::playnite::get_extension_target_dir(destPath)) {
        return false;
      }
      std::filesystem::path dest = destPath;
      return std::filesystem::exists(dest / "extension.yaml") && std::filesystem::exists(dest / "SunshinePlaynite.psm1");
    } catch (...) {
      return false;
    }
  }

  // Enhance app JSON with a Playnite-derived cover path when applicable.
  void enhance_app_with_playnite_cover(nlohmann::json &input_tree) {
    try {
      if ((!input_tree.contains("image-path") || (input_tree["image-path"].is_string() && input_tree["image-path"].get<std::string>().empty())) &&
          input_tree.contains("playnite-id") && input_tree["playnite-id"].is_string()) {
        std::string cover;
        if (platf::playnite::get_cover_png_for_playnite_game(input_tree["playnite-id"].get<std::string>(), cover)) {
          input_tree["image-path"] = cover;
        }
      }
    } catch (...) {
      // Best-effort only
    }
  }

  // No longer needed: old fallback path resolver removed with AssocQueryString-based detection

  void getPlayniteStatus(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);
    nlohmann::json out;
    // Active reflects current pipe/server connection only
    out["active"] = platf::playnite::is_active();
    // Deprecated fields removed: playnite_running, installed_unknown
    std::string destPath;
    std::filesystem::path dest;
    // Session requirement removed: IPC is available during RDP/lock; rely on Playnite process presence instead.
    // Resolve the user's Playnite extensions directory via URL association.
    // Requires user impersonation when running as SYSTEM.
    if (platf::playnite::get_extension_target_dir(destPath)) {
      dest = destPath;
      bool installed = std::filesystem::exists(dest / "extension.yaml") && std::filesystem::exists(dest / "SunshinePlaynite.psm1");
      out["installed"] = installed;
      out["extensions_dir"] = dest.string();
    } else {
      out["installed"] = false;
      out["extensions_dir"] = std::string();
    }
    // Version info and update flag
    auto normalize_ver = [](std::string s) {
      // strip leading 'v' and whitespace
      while (!s.empty() && (s[0] == ' ' || s[0] == '\t')) {
        s.erase(s.begin());
      }
      if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) {
        s.erase(s.begin());
      }
      return s;
    };
    auto semver_cmp = [&](const std::string &a, const std::string &b) {
      auto to_parts = [](const std::string &s) {
        std::vector<int> parts;
        int cur = 0;
        bool have = false;
        for (size_t i = 0; i <= s.size(); ++i) {
          if (i == s.size() || s[i] == '.') {
            parts.push_back(have ? cur : 0);
            cur = 0;
            have = false;
          } else if (s[i] >= '0' && s[i] <= '9') {
            have = true;
            cur = cur * 10 + (s[i] - '0');
          } else {
            // stop at first non-digit/non-dot
            break;
          }
        }
        while (!parts.empty() && parts.back() == 0) {
          parts.pop_back();
        }
        return parts;
      };
      auto pa = to_parts(normalize_ver(a));
      auto pb = to_parts(normalize_ver(b));
      size_t n = std::max(pa.size(), pb.size());
      pa.resize(n, 0);
      pb.resize(n, 0);
      for (size_t i = 0; i < n; ++i) {
        if (pa[i] < pb[i]) {
          return -1;
        }
        if (pa[i] > pb[i]) {
          return 1;
        }
      }
      return 0;
    };
    std::string installed_ver, packaged_ver;
    bool have_installed = platf::playnite::get_installed_plugin_version(installed_ver);
    bool have_packaged = platf::playnite::get_packaged_plugin_version(packaged_ver);
    if (have_installed) {
      out["installed_version"] = installed_ver;
    }
    if (have_packaged) {
      out["packaged_version"] = packaged_ver;
    }
    bool update_available = false;
    if (out["installed"].get<bool>() && have_installed && have_packaged) {
      update_available = semver_cmp(installed_ver, packaged_ver) < 0;
    }
    out["update_available"] = update_available;
    // No session readiness flag; IPC works through RDP/lock. Frontend derives readiness from installed/active.
    // Reduce verbosity: this endpoint can be polled frequently by the UI.
    // Log at debug level instead of info to avoid log spam while still
    // keeping the line available when debugging.
    BOOST_LOG(debug) << "Playnite status: active=" << out["active"]
                     << ", dir=" << (dest.empty() ? std::string("(unknown)") : dest.string())
                     << ", installed_version=" << (have_installed ? installed_ver : std::string(""))
                     << ", packaged_version=" << (have_packaged ? packaged_ver : std::string(""))
                     << ", update_available=" << (update_available ? "true" : "false");
    send_response(response, out);
  }

  void getPlayniteGames(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);
    try {
      if (!is_plugin_installed()) {
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        headers.emplace("X-Frame-Options", "DENY");
        headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
        response->write(SimpleWeb::StatusCode::success_ok, "[]", headers);
        return;
      }
      std::string json;
      if (!platf::playnite::get_games_list_json(json)) {
        // return empty array if not available
        json = "[]";
      }
      BOOST_LOG(debug) << "Playnite games: json length=" << json.size();
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      headers.emplace("X-Frame-Options", "DENY");
      headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
      response->write(SimpleWeb::StatusCode::success_ok, json, headers);
    } catch (std::exception &e) {
      bad_request(response, request, e.what());
    }
  }

  void getPlayniteCategories(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);
    try {
      if (!is_plugin_installed()) {
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        headers.emplace("X-Frame-Options", "DENY");
        headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
        response->write(SimpleWeb::StatusCode::success_ok, "[]", headers);
        return;
      }
      std::string json;
      if (!platf::playnite::get_categories_list_json(json)) {
        // return empty array if not available
        json = "[]";
      }
      BOOST_LOG(debug) << "Playnite categories: json length=" << json.size();
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      headers.emplace("X-Frame-Options", "DENY");
      headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
      response->write(SimpleWeb::StatusCode::success_ok, json, headers);
    } catch (std::exception &e) {
      bad_request(response, request, e.what());
    }
  }

  void getPlaynitePlugins(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);
    try {
      if (!is_plugin_installed()) {
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        headers.emplace("X-Frame-Options", "DENY");
        headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
        response->write(SimpleWeb::StatusCode::success_ok, "[]", headers);
        return;
      }
      std::string json;
      if (!platf::playnite::get_plugins_list_json(json)) {
        json = "[]";
      }
      BOOST_LOG(debug) << "Playnite plugins: json length=" << json.size();
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      headers.emplace("X-Frame-Options", "DENY");
      headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
      response->write(SimpleWeb::StatusCode::success_ok, json, headers);
    } catch (std::exception &e) {
      bad_request(response, request, e.what());
    }
  }

  void installPlaynite(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);
    std::string err;
    nlohmann::json out;
    bool request_restart = false;
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      if (ss.rdbuf()->in_avail() > 0) {
        auto in = nlohmann::json::parse(ss);
        request_restart = in.value("restart", false);
      }
    } catch (...) {
      // ignore body parse errors; treat as no-restart
    }
    // Prefer same resolved dir as status
    std::string target;
    bool have_target = platf::playnite::get_extension_target_dir(target);
    bool ok = false;
    if (have_target) {
      ok = platf::playnite::install_plugin_to(target, err);
    } else {
      ok = platf::playnite::install_plugin(err);
    }
    std::ostringstream log_msg;
    log_msg << "Playnite install: " << (ok ? "success" : "failed");
    if (have_target) {
      log_msg << " target=" << target;
    }
    log_msg << " restart=" << (request_restart ? "true" : "false");
    if (!ok && !err.empty()) {
      log_msg << " error=" << err;
    }
    BOOST_LOG(info) << log_msg.str();
    out["status"] = ok;
    if (!ok) {
      out["error"] = err;
    }
    // Optionally close and restart Playnite to pick up the new plugin
    if (ok && request_restart) {
      bool restarted = platf::playnite::restart_playnite();
      out["restarted"] = restarted;
    }
    send_response(response, out);
  }

  void uninstallPlaynite(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);
    std::string err;
    nlohmann::json out;
    bool request_restart = false;
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      if (ss.rdbuf()->in_avail() > 0) {
        auto in = nlohmann::json::parse(ss);
        request_restart = in.value("restart", false);
      }
    } catch (...) {
      // ignore body parse errors; treat as no-restart
    }
    bool ok = platf::playnite::uninstall_plugin(err);
    {
      std::ostringstream log_msg;
      log_msg << "Playnite uninstall: " << (ok ? "success" : "failed")
              << " restart=" << (request_restart ? "true" : "false");
      if (!ok && !err.empty()) {
        log_msg << " error=" << err;
      }
      BOOST_LOG(info) << log_msg.str();
    }
    out["status"] = ok;
    if (!ok) {
      out["error"] = err;
    }
    if (ok && request_restart) {
      bool restarted = platf::playnite::restart_playnite();
      out["restarted"] = restarted;
    }
    send_response(response, out);
  }

  void postPlayniteForceSync(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);
    nlohmann::json out;
    bool ok = platf::playnite::force_sync();
    out["status"] = ok;
    send_response(response, out);
  }

  void postPlayniteLaunch(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);
    nlohmann::json out;
    // Use unified restart path: will start Playnite if not running
    bool ok = platf::playnite::restart_playnite();
    out["status"] = ok;
    send_response(response, out);
  }

  // --- Collect Playnite-related logs into a ZIP and stream to browser ---
  static inline void write_le16(std::string &out, uint16_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
  }

  static inline void write_le32(std::string &out, uint32_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
  }

  static inline void current_dos_datetime(uint16_t &dos_time, uint16_t &dos_date) {
    std::time_t tt = std::time(nullptr);
    std::tm tm {};
  #ifdef _WIN32
    localtime_s(&tm, &tt);
  #else
    localtime_r(&tt, &tm);
  #endif
    dos_time = static_cast<uint16_t>(((tm.tm_hour & 0x1F) << 11) | ((tm.tm_min & 0x3F) << 5) | ((tm.tm_sec / 2) & 0x1F));
    int year = tm.tm_year + 1900;
    if (year < 1980) {
      year = 1980;
    }
    if (year > 2107) {
      year = 2107;
    }
    dos_date = static_cast<uint16_t>(((year - 1980) << 9) | (((tm.tm_mon + 1) & 0x0F) << 5) | (tm.tm_mday & 0x1F));
  }

  


  static bool deflate_buffer(const char *data, std::size_t size, std::string &out) {
    z_stream zs {};
    if (deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
      deflateEnd(&zs);
      return false;
    }
    std::array<unsigned char, 16384> buf {};
    out.clear();
    std::size_t offset = 0;
    int ret = Z_OK;
    do {
      if (zs.avail_in == 0 && offset < size) {
        std::size_t chunk = std::min<std::size_t>(size - offset, static_cast<std::size_t>(std::numeric_limits<uInt>::max()));
        zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data + offset));
        zs.avail_in = static_cast<uInt>(chunk);
        offset += chunk;
      }
      zs.next_out = buf.data();
      zs.avail_out = static_cast<uInt>(buf.size());
      int flush = (offset >= size && zs.avail_in == 0) ? Z_FINISH : Z_NO_FLUSH;
      ret = deflate(&zs, flush);
      if (ret == Z_STREAM_ERROR) {
        deflateEnd(&zs);
        return false;
      }
      std::size_t produced = buf.size() - zs.avail_out;
      if (produced > 0) {
        out.append(reinterpret_cast<char *>(buf.data()), produced);
      }
    } while (ret != Z_STREAM_END);
    deflateEnd(&zs);
    return true;
  }

  static std::string build_zip_from_entries(const std::vector<std::pair<std::string, std::string>> &entries) {
    std::string out;

    struct CdEnt {
      std::string name;
      uint32_t crc;
      uint32_t comp_size;
      uint32_t uncomp_size;
      uint16_t method;
      uint32_t offset;
      uint16_t dostime;
      uint16_t dosdate;
    };

    std::vector<CdEnt> cd;
    uint16_t dostime = 0, dosdate = 0;
    current_dos_datetime(dostime, dosdate);
    for (const auto &e : entries) {
      const std::string &name = e.first;
      const std::string &data = e.second;
      boost::crc_32_type crc;
      crc.process_bytes(data.data(), data.size());
      uint32_t crc32 = crc.checksum();
      uint32_t uncomp_size = static_cast<uint32_t>(data.size());
      std::string compressed;
      bool use_deflate = deflate_buffer(data.data(), data.size(), compressed) && compressed.size() < data.size();
      const std::string &payload = use_deflate ? compressed : data;
      uint16_t method = use_deflate ? 8 : 0;
      uint32_t comp_size = static_cast<uint32_t>(payload.size());
      uint32_t offset = static_cast<uint32_t>(out.size());
      write_le32(out, 0x04034b50u);
      write_le16(out, 20);
      write_le16(out, 0);
      write_le16(out, method);
      write_le16(out, dostime);
      write_le16(out, dosdate);
      write_le32(out, crc32);
      write_le32(out, comp_size);
      write_le32(out, uncomp_size);
      write_le16(out, static_cast<uint16_t>(name.size()));
      write_le16(out, 0);
      out.append(name.data(), name.size());
      out.append(payload.data(), payload.size());
      cd.push_back(CdEnt {name, crc32, comp_size, uncomp_size, method, offset, dostime, dosdate});
    }

    uint32_t cd_start = static_cast<uint32_t>(out.size());
    uint32_t cd_size = 0;
    for (const auto &e : cd) {
      std::string rec;
      write_le32(rec, 0x02014b50u);
      write_le16(rec, 20);
      write_le16(rec, 20);
      write_le16(rec, 0);
      write_le16(rec, e.method);
      write_le16(rec, e.dostime);
      write_le16(rec, e.dosdate);
      write_le32(rec, e.crc);
      write_le32(rec, e.comp_size);
      write_le32(rec, e.uncomp_size);
      write_le16(rec, static_cast<uint16_t>(e.name.size()));
      write_le16(rec, 0);
      write_le16(rec, 0);
      write_le16(rec, 0);
      write_le16(rec, 0);
      write_le32(rec, 0);
      write_le32(rec, e.offset);
      rec.append(e.name.data(), e.name.size());
      cd_size += static_cast<uint32_t>(rec.size());
      out.append(rec);
    }

    write_le32(out, 0x06054b50u);
    write_le16(out, 0);
    write_le16(out, 0);
    write_le16(out, static_cast<uint16_t>(cd.size()));
    write_le16(out, static_cast<uint16_t>(cd.size()));
    write_le32(out, cd_size);
    write_le32(out, cd_start);
    write_le16(out, 0);
    return out;
  }


  namespace {
    namespace fs = std::filesystem;
    namespace pt = boost::property_tree;

    pt::ptree &ensure_pt_root(pt::ptree &tree) {
      auto it = tree.find("root");
      if (it == tree.not_found()) {
        auto inserted = tree.insert(tree.end(), std::make_pair(std::string("root"), pt::ptree {}));
        return inserted->second;
      }
      return it->second;
    }

    struct CrashDismissalState {
      std::string filename;
      std::string captured_at;
      std::string dismissed_at;
    };

    std::optional<CrashDismissalState> load_crash_dismissal_state() {
      statefile::migrate_recent_state_keys();
      const std::string &path_str = statefile::vibeshine_state_path();
      if (path_str.empty()) {
        return std::nullopt;
      }
      std::lock_guard<std::mutex> lock(statefile::state_mutex());
      fs::path path(path_str);
      if (!fs::exists(path)) {
        return std::nullopt;
      }
      pt::ptree tree;
      try {
        pt::read_json(path.string(), tree);
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Crash dismissal: failed to read state file: " << e.what();
        return std::nullopt;
      }
      auto root_it = tree.find("root");
      if (root_it == tree.not_found()) {
        return std::nullopt;
      }
      auto dismissal_it = root_it->second.find("crashdump_dismissal");
      if (dismissal_it == root_it->second.not_found()) {
        return std::nullopt;
      }
      CrashDismissalState state;
      state.filename = dismissal_it->second.get<std::string>("filename", "");
      state.captured_at = dismissal_it->second.get<std::string>("captured_at", "");
      state.dismissed_at = dismissal_it->second.get<std::string>("dismissed_at", "");
      if (state.filename.empty()) {
        return std::nullopt;
      }
      return state;
    }

    bool save_crash_dismissal_state(const CrashDismissalState &state) {
      statefile::migrate_recent_state_keys();
      const std::string &path_str = statefile::vibeshine_state_path();
      if (path_str.empty()) {
        return false;
      }
      std::lock_guard<std::mutex> lock(statefile::state_mutex());
      fs::path path(path_str);
      pt::ptree tree;
      if (fs::exists(path)) {
        try {
          pt::read_json(path.string(), tree);
        } catch (const std::exception &e) {
          BOOST_LOG(warning) << "Crash dismissal: failed to read existing state file: " << e.what();
          tree = {};
        }
      }
      auto &root = ensure_pt_root(tree);
      pt::ptree node;
      node.put("filename", state.filename);
      node.put("captured_at", state.captured_at);
      node.put("dismissed_at", state.dismissed_at);
      root.put_child("crashdump_dismissal", node);
      try {
        fs::path dir = path.parent_path();
        if (!dir.empty() && !fs::exists(dir)) {
          fs::create_directories(dir);
        }
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Crash dismissal: failed to prepare directories: " << e.what();
      }
      try {
        pt::write_json(path.string(), tree);
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Crash dismissal: failed to write state file: " << e.what();
        return false;
      }
      return true;
    }
  }  // namespace

  static bool read_file_if_exists(const std::filesystem::path &p, std::string &out) {
    std::error_code ec {};
    if (!std::filesystem::exists(p, ec) || std::filesystem::is_directory(p, ec)) {
      return false;
    }
    try {
      std::ifstream f(p, std::ios::binary);
      if (!f) {
        return false;
      }
      std::ostringstream ss;
      ss << f.rdbuf();
      out = ss.str();
      return true;
    } catch (...) {
      return false;
    }
  }

  static std::vector<std::pair<std::string, std::string>> collect_support_logs() {
    std::vector<std::pair<std::string, std::string>> entries;

    // Sunshine log directory (session logging)
    try {
      bool collected_directory = false;
      if (auto log_dir = logging::session_log_directory()) {
        std::error_code ec;
        for (std::filesystem::directory_iterator it(*log_dir, ec); it != std::filesystem::directory_iterator(); ++it) {
          if (ec) {
            break;
          }
          std::error_code file_ec;
          if (!it->is_regular_file(file_ec)) {
            continue;
          }
          std::string data;
          if (read_file_if_exists(it->path(), data)) {
            entries.emplace_back(it->path().filename().string(), std::move(data));
            collected_directory = true;
          }
        }
      }
      if (!collected_directory) {
        auto current_log = logging::current_log_file();
        if (!current_log.empty()) {
          std::string data;
          if (read_file_if_exists(current_log, data)) {
            entries.emplace_back(current_log.filename().string(), std::move(data));
          }
        }
      }
    } catch (...) {}

    // Playnite plugin log (Roaming\Sunshine\sunshine_playnite.log)
    try {
      platf::dxgi::safe_token user_token;
      user_token.reset(platf::dxgi::retrieve_users_token(false));
      PWSTR roamingW = nullptr;
      if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, user_token.get(), &roamingW)) && roamingW) {
        std::filesystem::path p = std::filesystem::path(roamingW) / L"Sunshine" / L"sunshine_playnite.log";
        CoTaskMemFree(roamingW);
        std::string data;
        if (read_file_if_exists(p, data)) {
          entries.emplace_back(p.filename().string(), std::move(data));
        }
      }
    } catch (...) {}

    // Plugin fallback log: try user's LocalAppData\Temp then process TEMP
    try {
      platf::dxgi::safe_token user_token;
      user_token.reset(platf::dxgi::retrieve_users_token(false));
      PWSTR localW = nullptr;
      if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, user_token.get(), &localW)) && localW) {
        std::filesystem::path p = std::filesystem::path(localW) / L"Temp" / L"sunshine_playnite.log";
        CoTaskMemFree(localW);
        std::string data;
        if (read_file_if_exists(p, data)) {
          entries.emplace_back(p.filename().string(), std::move(data));
        }
      }
    } catch (...) {}
    try {
      wchar_t tmpPathW[MAX_PATH] = {};
      DWORD n = GetTempPathW(_countof(tmpPathW), tmpPathW);
      if (n > 0 && n < _countof(tmpPathW)) {
        std::filesystem::path p = std::filesystem::path(tmpPathW) / L"sunshine_playnite.log";
        std::string data;
        if (read_file_if_exists(p, data)) {
          entries.emplace_back(p.filename().string(), std::move(data));
        }
      }
    } catch (...) {}

    auto add_playnite_from_base = [&](const std::filesystem::path &base) {
      bool any = false;
      {
        std::string data;
        auto p = base / L"playnite.log";
        if (read_file_if_exists(p, data)) {
          entries.emplace_back(p.filename().string(), std::move(data));
          any = true;
        }
      }
      {
        std::string data;
        auto p = base / L"extensions.log";
        if (read_file_if_exists(p, data)) {
          entries.emplace_back(p.filename().string(), std::move(data));
          any = true;
        }
      }
      {
        std::string data;
        auto p = base / L"launcher.log";
        if (read_file_if_exists(p, data)) {
          entries.emplace_back(p.filename().string(), std::move(data));
          any = true;
        }
      }
      return any;
    };

    bool got_playnite_logs = false;
    try {
      platf::dxgi::safe_token user_token;
      user_token.reset(platf::dxgi::retrieve_users_token(false));
      auto add_from_known = [&](REFKNOWNFOLDERID id) {
        PWSTR pathW = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, user_token.get(), &pathW)) && pathW) {
          std::filesystem::path base = std::filesystem::path(pathW) / L"Playnite";
          CoTaskMemFree(pathW);
          if (add_playnite_from_base(base)) {
            got_playnite_logs = true;
          }
        }
      };
      add_from_known(FOLDERID_RoamingAppData);
      add_from_known(FOLDERID_LocalAppData);
    } catch (...) {}

    if (!got_playnite_logs) {
      try {
        wchar_t buf[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf))) {
          got_playnite_logs |= add_playnite_from_base(std::filesystem::path(buf) / L"Playnite");
        }
      } catch (...) {}
      try {
        wchar_t buf[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf))) {
          got_playnite_logs |= add_playnite_from_base(std::filesystem::path(buf) / L"Playnite");
        }
      } catch (...) {}
    }

    auto add_session_logs_with_prefix = [&](const std::filesystem::path &dir, const std::string &prefix) {
      std::error_code ec;
      for (std::filesystem::directory_iterator it(dir, ec); it != std::filesystem::directory_iterator(); ++it) {
        if (ec) {
          break;
        }
        std::error_code file_ec;
        if (!it->is_regular_file(file_ec)) {
          continue;
        }
        const auto filename = it->path().filename().string();
        if (filename.rfind(prefix, 0) != 0) {
          continue;
        }
        std::string data;
        if (read_file_if_exists(it->path(), data)) {
          entries.emplace_back(filename, std::move(data));
        }
      }
    };

    auto add_user_helper_logs = [&](const std::filesystem::path &base) {
      // Legacy single-file helper logs (kept for backwards compatibility).
      {
        std::filesystem::path p = base / L"sunshine_playnite.log";
        std::string data;
        if (read_file_if_exists(p, data)) {
          entries.emplace_back(p.filename().string(), std::move(data));
        }
      }
      {
        std::filesystem::path p = base / L"sunshine_playnite_launcher.log";
        std::string data;
        if (read_file_if_exists(p, data)) {
          entries.emplace_back(p.filename().string(), std::move(data));
        }
      }
      {
        std::filesystem::path p = base / L"sunshine_launcher.log";
        std::string data;
        if (read_file_if_exists(p, data)) {
          entries.emplace_back(p.filename().string(), std::move(data));
        }
      }
      {
        std::filesystem::path p = base / L"sunshine_display_helper.log";
        std::string data;
        if (read_file_if_exists(p, data)) {
          entries.emplace_back(p.filename().string(), std::move(data));
        }
      }
      {
        std::filesystem::path p = base / L"sunshine_wgc_helper.log";
        std::string data;
        if (read_file_if_exists(p, data)) {
          entries.emplace_back(p.filename().string(), std::move(data));
        }
      }

      // Session-mode helper logs live under Roaming/LocalAppData\\Sunshine\\logs.
      const auto log_dir = base / L"logs";
      add_session_logs_with_prefix(log_dir, "sunshine_playnite-");
      add_session_logs_with_prefix(log_dir, "sunshine_playnite_launcher-");
      add_session_logs_with_prefix(log_dir, "sunshine_launcher-");
      add_session_logs_with_prefix(log_dir, "sunshine_display_helper-");
      add_session_logs_with_prefix(log_dir, "sunshine_wgc_helper-");
    };

    try {
      auto add_user_sunshine_logs = [&](REFKNOWNFOLDERID id) {
        platf::dxgi::safe_token user_token;
        user_token.reset(platf::dxgi::retrieve_users_token(false));
        PWSTR baseW = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, user_token.get(), &baseW)) && baseW) {
          std::filesystem::path base = std::filesystem::path(baseW) / L"Sunshine";
          CoTaskMemFree(baseW);
          add_user_helper_logs(base);
        }
      };
      add_user_sunshine_logs(FOLDERID_RoamingAppData);
      add_user_sunshine_logs(FOLDERID_LocalAppData);
    } catch (...) {}

    auto try_add_sunshine_logs = [&](int csidl) {
      wchar_t baseW[MAX_PATH] = {};
      if (!SUCCEEDED(SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, baseW))) {
        return;
      }
      std::filesystem::path base = std::filesystem::path(baseW) / L"Sunshine";
      add_user_helper_logs(base);
    };

    try_add_sunshine_logs(CSIDL_APPDATA);
    try_add_sunshine_logs(CSIDL_LOCAL_APPDATA);

    try {
      std::filesystem::path cfg = platf::appdata();
      std::filesystem::path p = cfg / "sunshine_launcher.log";
      std::string data;
      if (read_file_if_exists(p, data)) {
        entries.emplace_back(p.filename().string(), std::move(data));
      }
    } catch (...) {}

    {
      std::vector<std::pair<std::string, std::string>> dedup;
      std::unordered_set<std::string> seen;
      dedup.reserve(entries.size());
      for (auto &e : entries) {
        if (seen.insert(e.first).second) {
          dedup.emplace_back(std::move(e));
        }
      }
      entries.swap(dedup);
    }

    return entries;
  }

  void downloadPlayniteLogs(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);
    try {
      auto entries = collect_support_logs();
      std::string zip = build_zip_from_entries(entries);

      char fname[64];
      std::time_t tt = std::time(nullptr);
      std::tm tm {};
      localtime_s(&tm, &tt);
      std::snprintf(fname, sizeof(fname), "vibeshine_logs-%04d%02d%02d-%02d%02d%02d.zip", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/zip");
      std::string cd = std::string("attachment; filename=\"") + fname + "\"";
      headers.emplace("Content-Disposition", cd);
      headers.emplace("X-Frame-Options", "DENY");
      headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
      response->write(SimpleWeb::StatusCode::success_ok, zip, headers);
    } catch (std::exception &e) {
      bad_request(response, request, e.what());
    }
  }

  struct CrashDumpInfo {
    std::filesystem::path path;
    std::filesystem::file_time_type write_time;
    std::uint64_t size;
  };

  struct ZipFileEntry {
    std::string name;
    std::filesystem::path path;
    std::filesystem::file_time_type write_time;
    std::uint64_t size;
  };

  constexpr std::uint64_t kMinCrashDumpSizeBytes = 10ull * 1024ull * 1024ull;

  static std::filesystem::path crash_dump_directory() {
    wchar_t sysDir[MAX_PATH] = {};
    UINT len = GetSystemDirectoryW(sysDir, _countof(sysDir));
    if (len == 0 || len >= _countof(sysDir)) {
      return {};
    }
    std::filesystem::path base(sysDir);
    return base / L"config" / L"systemprofile" / L"AppData" / L"Local" / L"CrashDumps";
  }

  static std::chrono::system_clock::time_point file_time_to_system_clock(std::filesystem::file_time_type ft) {
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(ft - decltype(ft)::clock::now() + std::chrono::system_clock::now());
  }

  static void to_dos_datetime(std::chrono::system_clock::time_point tp, uint16_t &dos_time, uint16_t &dos_date) {
    std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm {};
    localtime_s(&tm, &tt);
    dos_time = static_cast<uint16_t>(((tm.tm_hour & 0x1F) << 11) | ((tm.tm_min & 0x3F) << 5) | ((tm.tm_sec / 2) & 0x1F));
    int year = tm.tm_year + 1900;
    if (year < 1980) {
      year = 1980;
    }
    if (year > 2107) {
      year = 2107;
    }
    dos_date = static_cast<uint16_t>(((year - 1980) << 9) | (((tm.tm_mon + 1) & 0x0F) << 5) | (tm.tm_mday & 0x1F));
  }

  static std::string to_iso8601(const std::chrono::system_clock::time_point &tp) {
    std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm {};
    gmtime_s(&tm, &tt);
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
      return {};
    }
    return std::string(buf);
  }

  static std::optional<CrashDumpInfo> find_recent_crash_dump(std::chrono::hours max_age) {
    auto root = crash_dump_directory();
    if (root.empty()) {
      return std::nullopt;
    }
    std::error_code ec {};
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
      return std::nullopt;
    }

    CrashDumpInfo best {};
    std::filesystem::file_time_type best_time {};
    bool have = false;
    for (const auto &entry : std::filesystem::directory_iterator(root, ec)) {
      if (ec) {
        return std::nullopt;
      }
      if (!entry.is_regular_file(ec)) {
        continue;
      }
      auto filename = entry.path().filename().wstring();
      std::wstring lower = filename;
      std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t ch) {
        return std::towlower(ch);
      });
      if (lower.rfind(L"sunshine.exe.", 0) != 0) {
        continue;
      }
      auto write_time = entry.last_write_time(ec);
      if (ec) {
        ec.clear();
        continue;
      }
      std::error_code size_ec {};
      auto file_size_raw = std::filesystem::file_size(entry.path(), size_ec);
      if (size_ec) {
        continue;
      }
      if (file_size_raw < kMinCrashDumpSizeBytes) {
        continue;
      }
      if (file_size_raw > std::numeric_limits<std::uint64_t>::max()) {
        continue;
      }
      const std::uint64_t file_size = static_cast<std::uint64_t>(file_size_raw);
      if (!have || write_time > best_time) {
        best_time = write_time;
        best.path = entry.path();
        best.write_time = write_time;
        best.size = file_size;
        have = true;
      }
    }
    if (!have) {
      return std::nullopt;
    }
    auto sys_time = file_time_to_system_clock(best.write_time);
    auto now = std::chrono::system_clock::now();
    if (sys_time + max_age < now) {
      return std::nullopt;
    }
    return best;
  }

  static inline void write_le16(std::ostream &out, uint16_t v) {
    out.put(static_cast<char>(v & 0xFF));
    out.put(static_cast<char>((v >> 8) & 0xFF));
  }

  static inline void write_le32(std::ostream &out, uint32_t v) {
    out.put(static_cast<char>(v & 0xFF));
    out.put(static_cast<char>((v >> 8) & 0xFF));
    out.put(static_cast<char>((v >> 16) & 0xFF));
    out.put(static_cast<char>((v >> 24) & 0xFF));
  }

  
  static bool write_zip_bundle_to_path(const std::filesystem::path &dest,
                                       const std::vector<std::pair<std::string, std::string>> &data_entries,
                                       const std::vector<ZipFileEntry> &file_entries,
                                       std::string &error) {
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) {
      error = "Failed to create crash bundle";
      return false;
    }

    struct CdEntry {
      std::string name;
      uint32_t crc;
      uint32_t comp_size;
      uint32_t uncomp_size;
      uint32_t offset;
      uint16_t method;
      uint16_t dostime;
      uint16_t dosdate;
    };
    std::vector<CdEntry> cd;

    auto add_entry = [&](const std::string &name, uint16_t method, uint32_t crc, uint32_t comp_size, uint32_t uncomp_size, uint16_t dostime, uint16_t dosdate, const std::string &payload) -> bool {
      auto pos = out.tellp();
      if (pos < 0 || static_cast<unsigned long long>(pos) > std::numeric_limits<uint32_t>::max()) {
        error = "ZIP entry offset overflow";
        return false;
      }
      uint32_t offset = static_cast<uint32_t>(pos);
      write_le32(out, 0x04034b50u);
      write_le16(out, 20);
      write_le16(out, 0);
      write_le16(out, method);
      write_le16(out, dostime);
      write_le16(out, dosdate);
      write_le32(out, crc);
      write_le32(out, comp_size);
      write_le32(out, uncomp_size);
      write_le16(out, static_cast<uint16_t>(name.size()));
      write_le16(out, 0);
      out.write(name.data(), name.size());
      out.write(payload.data(), payload.size());
      if (!out) {
        error = "Failed writing ZIP entry";
        return false;
      }
      cd.push_back(CdEntry {name, crc, comp_size, uncomp_size, offset, method, dostime, dosdate});
      return true;
    };

    uint16_t now_time = 0, now_date = 0;
    current_dos_datetime(now_time, now_date);
    for (const auto &entry : data_entries) {
      const std::string &name = entry.first;
      const std::string &data = entry.second;
      boost::crc_32_type crc;
      crc.process_bytes(data.data(), data.size());
      uint32_t checksum = crc.checksum();
      std::string compressed;
      bool use_deflate = deflate_buffer(data.data(), data.size(), compressed) && compressed.size() < data.size();
      const std::string &payload = use_deflate ? compressed : data;
      uint16_t method = use_deflate ? 8 : 0;
      uint32_t comp_size = static_cast<uint32_t>(payload.size());
      uint32_t uncomp_size = static_cast<uint32_t>(data.size());
      if (!add_entry(name, method, checksum, comp_size, uncomp_size, now_time, now_date, payload)) {
        return false;
      }
    }

    for (const auto &entry : file_entries) {
      std::error_code ec {};
      if (!std::filesystem::exists(entry.path, ec) || !std::filesystem::is_regular_file(entry.path, ec)) {
        error = "Crash dump no longer exists";
        return false;
      }
      if (entry.size > std::numeric_limits<uint32_t>::max()) {
        error = "Crash dump too large (over 4 GiB)";
        return false;
      }
      std::ifstream in(entry.path, std::ios::binary);
      if (!in) {
        error = "Failed to open crash dump";
        return false;
      }
      std::string raw;
      raw.resize(static_cast<std::size_t>(entry.size));
      in.read(raw.data(), static_cast<std::streamsize>(raw.size()));
      if (in.gcount() != static_cast<std::streamsize>(raw.size())) {
        error = "Failed to read crash dump";
        return false;
      }
      boost::crc_32_type crc;
      crc.process_bytes(raw.data(), raw.size());
      uint32_t checksum = crc.checksum();
      std::string compressed;
      bool use_deflate = deflate_buffer(raw.data(), raw.size(), compressed) && compressed.size() < raw.size();
      const std::string &payload = use_deflate ? compressed : raw;
      uint16_t method = use_deflate ? 8 : 0;
      uint32_t comp_size = static_cast<uint32_t>(payload.size());
      uint32_t uncomp_size = static_cast<uint32_t>(raw.size());
      uint16_t dostime = 0, dosdate = 0;
      to_dos_datetime(file_time_to_system_clock(entry.write_time), dostime, dosdate);
      if (!add_entry(entry.name, method, checksum, comp_size, uncomp_size, dostime, dosdate, payload)) {
        return false;
      }
    }

    auto cd_start_pos = out.tellp();
    if (cd_start_pos < 0 || static_cast<unsigned long long>(cd_start_pos) > std::numeric_limits<uint32_t>::max()) {
      error = "ZIP central directory offset overflow";
      return false;
    }
    uint32_t cd_start = static_cast<uint32_t>(cd_start_pos);

    for (const auto &e : cd) {
      write_le32(out, 0x02014b50u);
      write_le16(out, 20);
      write_le16(out, 20);
      write_le16(out, 0);
      write_le16(out, e.method);
      write_le16(out, e.dostime);
      write_le16(out, e.dosdate);
      write_le32(out, e.crc);
      write_le32(out, e.comp_size);
      write_le32(out, e.uncomp_size);
      write_le16(out, static_cast<uint16_t>(e.name.size()));
      write_le16(out, 0);
      write_le16(out, 0);
      write_le16(out, 0);
      write_le16(out, 0);
      write_le32(out, 0);
      write_le32(out, e.offset);
      out.write(e.name.data(), e.name.size());
      if (!out) {
        error = "Failed writing ZIP central directory";
        return false;
      }
    }

    auto cd_end_pos = out.tellp();
    if (cd_end_pos < 0 || static_cast<unsigned long long>(cd_end_pos) > std::numeric_limits<uint32_t>::max()) {
      error = "ZIP central directory size overflow";
      return false;
    }
    uint32_t cd_size = static_cast<uint32_t>(cd_end_pos - cd_start_pos);

    write_le32(out, 0x06054b50u);
    write_le16(out, 0);
    write_le16(out, 0);
    write_le16(out, static_cast<uint16_t>(cd.size()));
    write_le16(out, static_cast<uint16_t>(cd.size()));
    write_le32(out, cd_size);
    write_le32(out, cd_start);
    write_le16(out, 0);
    out.flush();
    if (!out.good()) {
      error = "Failed finalizing crash bundle";
      return false;
    }
    return true;
  }

  void getCrashDumpStatus(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);
    try {
      auto info = find_recent_crash_dump(std::chrono::hours(24 * 7));
      nlohmann::json out;
      if (!info) {
        out["available"] = false;
        out["dismissed"] = false;
      } else {
        auto captured = file_time_to_system_clock(info->write_time);
        std::string captured_iso = to_iso8601(captured);
        out["available"] = true;
        out["path"] = info->path.string();
        out["filename"] = info->path.filename().string();
        out["size_bytes"] = info->size;
        out["captured_at"] = captured_iso;
        auto age = std::chrono::system_clock::now() - captured;
        out["age_seconds"] = std::chrono::duration_cast<std::chrono::seconds>(age).count();
        out["age_hours"] = std::chrono::duration_cast<std::chrono::hours>(age).count();
        if (auto dismissal = load_crash_dismissal_state()) {
          bool matches = dismissal->filename == info->path.filename().string();
          out["dismissed"] = matches;
          if (matches && !dismissal->dismissed_at.empty()) {
            out["dismissed_at"] = dismissal->dismissed_at;
          }
          if (matches && dismissal->captured_at.empty()) {
            CrashDismissalState updated = *dismissal;
            updated.captured_at = captured_iso;
            save_crash_dismissal_state(updated);
          }
        } else {
          out["dismissed"] = false;
        }
      }
      send_response(response, out);
    } catch (const std::exception &e) {
      bad_request(response, request, e.what());
    }
  }

  void postCrashDumpDismiss(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);
    try {
      const std::string body = request->content.string();
      if (body.empty()) {
        bad_request(response, request, "Missing request body");
        return;
      }
      auto payload = nlohmann::json::parse(body, nullptr, true, true);
      std::string filename = payload.value("filename", "");
      std::string captured_at = payload.value("captured_at", "");
      if (filename.empty()) {
        bad_request(response, request, "Missing filename");
        return;
      }
      auto info = find_recent_crash_dump(std::chrono::hours(24 * 7));
      if (!info) {
        bad_request(response, request, "No recent Sunshine crash dumps found (within last 7 days)");
        return;
      }
      std::string current_iso = to_iso8601(file_time_to_system_clock(info->write_time));
      if (filename != info->path.filename().string()) {
        bad_request(response, request, "Crash dump metadata mismatch");
        return;
      }
      CrashDismissalState state;
      state.filename = std::move(filename);
      state.captured_at = captured_at.empty() ? current_iso : std::move(captured_at);
      state.dismissed_at = to_iso8601(std::chrono::system_clock::now());
      if (!save_crash_dismissal_state(state)) {
        bad_request(response, request, "Failed to persist crash dismissal");
        return;
      }
      nlohmann::json out;
      out["status"] = true;
      out["dismissed_at"] = state.dismissed_at;
      send_response(response, out);
    } catch (const std::exception &e) {
      bad_request(response, request, e.what());
    }
  }

  void downloadCrashBundle(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);
    try {
      auto info = find_recent_crash_dump(std::chrono::hours(24 * 7));
      if (!info) {
        bad_request(response, request, "No recent Sunshine crash dumps found (within last 7 days)");
        return;
      }
      auto entries = collect_support_logs();
      std::vector<ZipFileEntry> files;
      files.push_back(ZipFileEntry {info->path.filename().string(), info->path, info->write_time, info->size});

      wchar_t tmpDir[MAX_PATH] = {};
      wchar_t tmpFile[MAX_PATH] = {};
      if (GetTempPathW(_countof(tmpDir), tmpDir) == 0) {
        bad_request(response, request, "Failed to resolve temporary directory");
        return;
      }
      if (GetTempFileNameW(tmpDir, L"SNC", 0, tmpFile) == 0) {
        bad_request(response, request, "Failed to create temporary file");
        return;
      }
      std::filesystem::path bundle_path(tmpFile);
      std::string error;
      if (!write_zip_bundle_to_path(bundle_path, entries, files, error)) {
        std::error_code ec {};
        std::filesystem::remove(bundle_path, ec);
        if (error.empty()) {
          error = "Failed to build crash bundle";
        }
        bad_request(response, request, error);
        return;
      }

      std::ifstream in(bundle_path, std::ios::binary);
      if (!in) {
        std::error_code ec {};
        std::filesystem::remove(bundle_path, ec);
        bad_request(response, request, "Failed to open crash bundle");
        return;
      }

      char fname[80];
      std::time_t tt = std::time(nullptr);
      std::tm tm {};
      localtime_s(&tm, &tt);
      std::snprintf(fname, sizeof(fname), "sunshine_crashbundle-%04d%02d%02d-%02d%02d%02d.zip", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/zip");
      headers.emplace("Content-Disposition", std::string("attachment; filename=\"") + fname + "\"");
      headers.emplace("X-Frame-Options", "DENY");
      headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
      response->write(SimpleWeb::StatusCode::success_ok, in, headers);
      in.close();
      std::error_code ec {};
      std::filesystem::remove(bundle_path, ec);
    } catch (const std::exception &e) {
      bad_request(response, request, e.what());
    }
  }

}  // namespace confighttp

#endif  // _WIN32
