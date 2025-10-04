/**
 * @file src/confighttp_playnite.cpp
 * @brief Playnite-specific HTTP endpoints and helpers (Windows-only).
 */

#ifdef _WIN32

  // standard includes
  #include <filesystem>
  #include <fstream>
  #include <sstream>
  #include <string>
  #include <string_view>
  #include <unordered_set>

  // third-party includes
  #include <nlohmann/json.hpp>
  #include <Simple-Web-Server/server_https.hpp>

  // local includes
  #include "config_playnite.h"
  #include "confighttp.h"
  #include "logging.h"
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

  // Build a minimal ZIP (stored, no compression) from name->data entries
  static std::string build_zip_from_entries(const std::vector<std::pair<std::string, std::string>> &entries) {
    std::string out;

    struct CdEnt {
      std::string name;
      uint32_t crc;
      uint32_t size;
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
      uint32_t size = static_cast<uint32_t>(data.size());
      uint32_t off = static_cast<uint32_t>(out.size());
      // Local file header
      write_le32(out, 0x04034b50u);  // signature
      write_le16(out, 20);  // version needed
      write_le16(out, 0);  // flags
      write_le16(out, 0);  // method: store
      write_le16(out, dostime);  // mod time
      write_le16(out, dosdate);  // mod date
      write_le32(out, crc32);  // crc32
      write_le32(out, size);  // comp size
      write_le32(out, size);  // uncomp size
      write_le16(out, static_cast<uint16_t>(name.size()));  // name len
      write_le16(out, 0);  // extra len
      out.append(name.data(), name.size());
      out.append(data.data(), data.size());
      cd.push_back(CdEnt {name, crc32, size, off, dostime, dosdate});
    }
    uint32_t cd_start = static_cast<uint32_t>(out.size());
    uint32_t cd_size = 0;
    for (const auto &e : cd) {
      std::string rec;
      write_le32(rec, 0x02014b50u);  // central dir header sig
      write_le16(rec, 20);  // version made by
      write_le16(rec, 20);  // version needed
      write_le16(rec, 0);  // flags
      write_le16(rec, 0);  // method: store
      write_le16(rec, e.dostime);  // time
      write_le16(rec, e.dosdate);  // date
      write_le32(rec, e.crc);  // crc32
      write_le32(rec, e.size);  // comp size
      write_le32(rec, e.size);  // uncomp size
      write_le16(rec, static_cast<uint16_t>(e.name.size()));  // name len
      write_le16(rec, 0);  // extra len
      write_le16(rec, 0);  // comment len
      write_le16(rec, 0);  // disk start
      write_le16(rec, 0);  // int attrs
      write_le32(rec, 0);  // ext attrs
      write_le32(rec, e.offset);  // rel offset local header
      rec.append(e.name.data(), e.name.size());
      cd_size += static_cast<uint32_t>(rec.size());
      out.append(rec);
    }
    // End of central directory
    write_le32(out, 0x06054b50u);
    write_le16(out, 0);  // disk num
    write_le16(out, 0);  // disk start
    write_le16(out, static_cast<uint16_t>(cd.size()));  // entries this disk
    write_le16(out, static_cast<uint16_t>(cd.size()));  // total entries
    write_le32(out, cd_size);  // size of central directory
    write_le32(out, cd_start);  // offset of central directory
    write_le16(out, 0);  // comment length
    return out;
  }

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

  void downloadPlayniteLogs(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);
    try {
      std::vector<std::pair<std::string, std::string>> entries;  // name, data

      // sunshine.log (configured location)
      try {
        std::string data;
        if (read_file_if_exists(config::sunshine.log_file, data)) {
          entries.emplace_back("sunshine.log", std::move(data));
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
        // Try active user's LocalAppData\Temp
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
        // Fallback to current process TEMP
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

      // Playnite's own logs (prefer active user's Roaming/Local AppData)
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
      // Fallback to current process' profile
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

      // Launcher/helper logs
      try {
        // Prefer active user's Roaming/Local AppData
        try {
          platf::dxgi::safe_token user_token;
          user_token.reset(platf::dxgi::retrieve_users_token(false));
          auto add_user_sunshine_logs = [&](REFKNOWNFOLDERID id) {
            PWSTR baseW = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(id, 0, user_token.get(), &baseW)) && baseW) {
              std::filesystem::path base = std::filesystem::path(baseW) / L"Sunshine";
              CoTaskMemFree(baseW);
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
                // Windows Display Helper (tools/sunshine_display_helper.exe)
                std::filesystem::path p = base / L"sunshine_display_helper.log";
                std::string data;
                if (read_file_if_exists(p, data)) {
                  entries.emplace_back(p.filename().string(), std::move(data));
                }
              }
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
          // Preferred new name
          {
            std::filesystem::path p = base / L"sunshine_playnite_launcher.log";
            std::string data;
            if (read_file_if_exists(p, data)) {
              entries.emplace_back(p.filename().string(), std::move(data));
            }
          }
          // Legacy/alternate name
          {
            std::filesystem::path p = base / L"sunshine_launcher.log";
            std::string data;
            if (read_file_if_exists(p, data)) {
              entries.emplace_back(p.filename().string(), std::move(data));
            }
          }
          // Display helper log
          {
            std::filesystem::path p = base / L"sunshine_display_helper.log";
            std::string data;
            if (read_file_if_exists(p, data)) {
              entries.emplace_back(p.filename().string(), std::move(data));
            }
          }
        };
        // Roaming AppData\Sunshine and LocalAppData\Sunshine
        try_add_sunshine_logs(CSIDL_APPDATA);
        try_add_sunshine_logs(CSIDL_LOCAL_APPDATA);
        // Fallback: check Sunshine config directory (next to sunshine.exe) for sunshine_launcher.log
        try {
          std::filesystem::path cfg = platf::appdata();
          std::filesystem::path p = cfg / "sunshine_launcher.log";
          std::string data;
          if (read_file_if_exists(p, data)) {
            entries.emplace_back(p.filename().string(), std::move(data));
          }
        } catch (...) {}
      } catch (...) {}

      // Deduplicate by entry name to avoid duplicate playnite helper logs
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

      // Build ZIP (may be empty if nothing found)
      std::string zip = build_zip_from_entries(entries);

      // Filename with timestamp
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

}  // namespace confighttp

#endif  // _WIN32
