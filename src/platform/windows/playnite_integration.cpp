/**
 * @file src/platform/windows/playnite_integration.cpp
 * @brief Playnite integration lifecycle and message handling.
 */

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include "playnite_integration.h"

#include "src/config.h"
#include "src/config_playnite.h"
#include "src/confighttp.h"
#include "src/file_handler.h"
#include "src/logging.h"
#include "src/platform/windows/image_convert.h"
#include "src/platform/windows/ipc/misc_utils.h"
#include "src/platform/windows/misc.h"
#include "src/platform/windows/playnite_ipc.h"
#include "src/platform/windows/playnite_protocol.h"
#include "src/platform/windows/playnite_sync.h"
#include "src/process.h"
#include "src/utility.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <KnownFolders.h>
#include <mutex>
#include <nlohmann/json.hpp>
#include <shellapi.h>
#include <ShlObj.h>
#include <Shlwapi.h>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <UserEnv.h>
#include <vector>
#include <Windows.h>
#include <winrt/base.h>
#include <WtsApi32.h>
// boost filesystem for process launch helpers
#include <boost/filesystem.hpp>

namespace platf::playnite {

  // Time parsing helper moved to platf::playnite::sync

  struct playnite_session_tracker_t {
    std::mutex mtx;
    std::string last_started_id;
    bool seen_started {false};

    void on_started(const std::string &id) {
      std::scoped_lock lk(mtx);
      last_started_id = id;
      seen_started = true;
    }

    bool allow_stop(const std::string &id) {
      std::scoped_lock lk(mtx);
      if (!seen_started) {
        return false;
      }
      if (!id.empty() && !last_started_id.empty() && id != last_started_id) {
        return false;
      }
      seen_started = false;
      last_started_id.clear();
      return true;
    }

    void reset() {
      std::scoped_lock lk(mtx);
      seen_started = false;
      last_started_id.clear();
    }
  };

  playnite_session_tracker_t &playnite_session_tracker() {
    static playnite_session_tracker_t tracker;
    return tracker;
  }

  // Acquire a primary user token suitable for per-user operations (HKCU view, KNOWNFOLDER paths, launching)
  // Preference order:
  // 1) Token from a running Playnite process (Desktop or Fullscreen)
  // 2) Any WTSActive session's user token (RDP or console)
  // 3) Fallback: console session token via platf::dxgi::retrieve_users_token(false)
  static HANDLE acquire_preferred_user_token_for_playnite() {
    // 1) If Playnite is running, use its process token
    try {
      auto d = platf::dxgi::find_process_ids_by_name(L"Playnite.DesktopApp.exe");
      auto f = platf::dxgi::find_process_ids_by_name(L"Playnite.FullscreenApp.exe");
      std::vector<DWORD> pids;
      pids.reserve(d.size() + f.size());
      pids.insert(pids.end(), d.begin(), d.end());
      pids.insert(pids.end(), f.begin(), f.end());
      for (DWORD pid : pids) {
        winrt::handle hProc {OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
        if (!hProc) {
          continue;
        }
        HANDLE raw = nullptr;
        if (!OpenProcessToken(hProc.get(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY, &raw)) {
          continue;
        }
        // Duplicate to a primary token to ensure broad compatibility (CreateProcessAsUser, registry overrides)
        HANDLE dup = nullptr;
        if (DuplicateTokenEx(raw, TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_IMPERSONATE | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID, nullptr, SecurityImpersonation, TokenPrimary, &dup)) {
          CloseHandle(raw);
          return dup;  // caller must CloseHandle
        }
        CloseHandle(raw);
      }
    } catch (...) {}

    // 2) Any active interactive session (RDP or console)
    WTS_SESSION_INFO *infos = nullptr;
    DWORD count = 0;
    if (WTSEnumerateSessions(WTS_CURRENT_SERVER_HANDLE, 0, 1, &infos, &count)) {
      for (DWORD i = 0; i < count; ++i) {
        if (infos[i].State == WTSActive) {
          HANDLE tok = nullptr;
          if (WTSQueryUserToken(infos[i].SessionId, &tok)) {
            // tok is a primary token
            WTSFreeMemory(infos);
            return tok;  // caller must CloseHandle
          }
        }
      }
      WTSFreeMemory(infos);
    }

    // 3) Fallback to console session
    return platf::dxgi::retrieve_users_token(false);
  }

  // Launch specified executable under the provided user token (primary)
  static bool launch_exe_as_token(HANDLE user_token, const std::wstring &exe_full_path, const std::wstring &start_dir) {
    if (!user_token || exe_full_path.empty()) {
      return false;
    }
    std::error_code ec;
    // We are not inserting the child into a job here, so pass nullptr (do not
    // provide a dummy HANDLE pointer or PROC_THREAD_ATTRIBUTE_JOB_LIST will be
    // populated with an invalid null handle causing CreateProcessAsUser to fail).
    STARTUPINFOEXW si = platf::create_startup_info(nullptr, nullptr, ec);
    if (ec) {
      return false;
    }
    auto free_list = util::fail_guard([&]() {
      platf::free_proc_thread_attr_list(si.lpAttributeList);
    });

    // Build user environment block
    LPVOID env_block = nullptr;
    if (!CreateEnvironmentBlock(&env_block, user_token, FALSE)) {
      env_block = nullptr;  // proceed without custom env
    }
    auto free_env = util::fail_guard([&]() {
      if (env_block) {
        DestroyEnvironmentBlock(env_block);
      }
    });

    std::wstring cmd = L"\"" + exe_full_path + L"\"";
    std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back(L'\0');

    DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE | CREATE_BREAKAWAY_FROM_JOB;

    BOOL ok = FALSE;
    PROCESS_INFORMATION pi {};
    auto run = [&]() {
      ok = CreateProcessAsUserW(user_token, nullptr, cmd_buf.data(), nullptr, nullptr, FALSE, flags, env_block, start_dir.empty() ? nullptr : start_dir.c_str(), reinterpret_cast<LPSTARTUPINFOW>(&si), &pi);
      if (!ok) {
        DWORD err = GetLastError();
        BOOST_LOG(warning) << "Playnite restart: CreateProcessAsUser failed, error=" << err;
      }
    };
    // Impersonate to ensure profile access/network shares during launch
    (void) platf::impersonate_current_user(user_token, run);
    if (ok) {
      // We don't track the child here; close handles to prevent leaks
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
    }
    return ok == TRUE;
  }

  // Forward declaration: refresh config id/name fields using latest snapshots
  static void refresh_config_id_name_fields(const std::vector<platf::playnite::Category> &cats,
                                            const std::vector<platf::playnite::Game> &games,
                                            const std::vector<platf::playnite::Plugin> &plugins);

  class deinit_t_impl;  // forward
  static std::atomic<deinit_t_impl *> g_instance {nullptr};

  static bool is_plugin_installed() {
    try {
      std::string dir;
      if (!get_extension_target_dir(dir)) {
        return false;
      }
      std::filesystem::path d(dir);
      return std::filesystem::exists(d / "extension.yaml") && std::filesystem::exists(d / "SunshinePlaynite.psm1");
    } catch (...) {
      return false;
    }
  }

  class deinit_t_impl: public ::platf::deinit_t {
  public:
    deinit_t_impl() {
      BOOST_LOG(info) << "Playnite integration: manager starting";
      g_instance.store(this, std::memory_order_release);
      // If plugin installed at startup, start immediately; otherwise wait for manager loop
      if (is_plugin_installed()) {
        BOOST_LOG(info) << "Playnite integration: plugin installed; starting IPC client";
        client_ = std::make_unique<platf::playnite::IpcClient>();
        client_->set_message_handler([this](std::span<const uint8_t> bytes) {
          handle_message(bytes);
        });
        client_->set_connected_handler([this]() {
          try {
            nlohmann::json hello;
            hello["type"] = "hello";
            hello["role"] = "sunshine";
            hello["pid"] = static_cast<uint32_t>(GetCurrentProcessId());
            client_->send_json_line(hello.dump());
          } catch (...) {}
        });
        client_->start();
        new_snapshot_ = true;
      } else {
        BOOST_LOG(info) << "Playnite integration: plugin not installed; client idle";
      }
      // Start manager loop to hot-apply enablement state
      stop_flag_.store(false, std::memory_order_release);
      manager_ = std::thread([this]() {
        this->manager_loop();
      });
    }

    ~deinit_t_impl() override {
      // Stop manager thread
      try {
        stop_flag_.store(true, std::memory_order_release);
        if (manager_.joinable()) {
          manager_.join();
        }
      } catch (...) {}
      if (client_) {
        client_->stop();
        client_.reset();
      }
      g_instance.store(nullptr, std::memory_order_release);
    }

    bool is_server_active() const {
      return client_ && client_->is_active();
    }

    bool send_cmd_json_line(const std::string &s) {
      return client_ && client_->send_json_line(s);
    }

    void trigger_sync() {
      auto stats = sync_apps_metadata();
      BOOST_LOG(info) << "Playnite: manual library sync " << sync_summary(stats);
    }

    void snapshot_games(std::vector<platf::playnite::Game> &out) {
      std::scoped_lock lk(mutex_);
      out = last_games_;
    }

    void snapshot_categories(std::vector<platf::playnite::Category> &out) {
      std::scoped_lock lk(mutex_);
      out = last_categories_;
    }

    void snapshot_plugins(std::vector<platf::playnite::Plugin> &out) {
      std::scoped_lock lk(mutex_);
      out = last_plugins_;
    }

    // Hot-toggle helpers: stop or start the IPC client without destroying the instance
    void stop_client() {
      try {
        if (client_) {
          BOOST_LOG(info) << "Playnite: stopping IPC client (hot-toggle)";
          client_->stop();
          client_.reset();
        }
        // Clear cached snapshots so UI doesn't falsely show data as connected
        try {
          std::scoped_lock lk(mutex_);
          last_games_.clear();
          game_ids_.clear();
          last_categories_.clear();
          last_plugins_.clear();
          new_snapshot_ = true;
        } catch (...) {}
        {
          std::scoped_lock lk(progress_mutex_);
          snapshot_progress_ = {};
        }
      } catch (...) {}
    }

    void ensure_started() {
      // Avoid hot-toggling: if a server exists and is already running (even if not
      // yet connected), do not tear it down and recreate it. This prevents rapid
      // restarts during the handshake window.
      if (client_ && (client_->is_active() || client_->is_started())) {
        return;
      }
      BOOST_LOG(info) << "Playnite: starting IPC client (hot-toggle)";
      client_ = std::make_unique<platf::playnite::IpcClient>();
      client_->set_message_handler([this](std::span<const uint8_t> bytes) {
        handle_message(bytes);
      });
      client_->set_connected_handler([this]() {
        try {
          nlohmann::json hello;
          hello["type"] = "hello";
          hello["role"] = "sunshine";
          hello["pid"] = static_cast<uint32_t>(GetCurrentProcessId());
          client_->send_json_line(hello.dump());
        } catch (...) {}
      });
      client_->start();
      new_snapshot_ = true;
      {
        std::scoped_lock lk(progress_mutex_);
        snapshot_progress_ = {};
      }
    }

    void manager_loop() {
      using namespace std::chrono_literals;
      // simple periodic reconciliation loop
      while (!stop_flag_.load(std::memory_order_acquire)) {
        const bool want = is_plugin_installed();
        if (want) {
          ensure_started();
        } else {
          stop_client();
        }
        emit_snapshot_summary_if_ready();
        std::this_thread::sleep_for(1500ms);
      }
    }

  private:
    struct SyncStats {
      bool success = false;
      bool changed = false;
      std::size_t matched = 0;
      std::size_t file_size = 0;
      std::string error;
    };

    static std::string sync_summary(const SyncStats &stats) {
      std::ostringstream os;
      os << "status=" << (stats.success ? (stats.changed ? "updated" : "unchanged") : "failed");
      if (stats.success) {
        os << " matched=" << stats.matched;
        if (stats.file_size != 0) {
          os << " apps_bytes=" << stats.file_size;
        }
        const auto &excluded = config::playnite.exclude_categories;
        if (!excluded.empty()) {
          os << " excluded_categories=[";
          std::size_t shown = 0;
          for (const auto &name : excluded) {
            if (shown >= 5) {
              break;
            }
            if (shown > 0) {
              os << ',';
            }
            std::string sanitized = name;
            for (auto &ch : sanitized) {
              if (ch == '\n' || ch == '\r') {
                ch = ' ';
              } else if (ch == '"') {
                ch = '\'';
              }
            }
            os << '"' << sanitized << '"';
            ++shown;
          }
          if (excluded.size() > shown) {
            os << ",+" << (excluded.size() - shown) << " more";
          }
          os << ']';
        }
      } else if (!stats.error.empty()) {
        std::string sanitized = stats.error;
        for (auto &ch : sanitized) {
          if (ch == '\n' || ch == '\r') {
            ch = ' ';
          }
        }
        os << " error=" << sanitized;
      }
      return os.str();
    }

    void handle_message(std::span<const uint8_t> bytes) {
      BOOST_LOG(debug) << "Playnite: handling message, bytes=" << bytes.size();
      auto msg = platf::playnite::parse(bytes);
      using MT = platf::playnite::MessageType;
      if (msg.type == MT::Categories) {
        BOOST_LOG(debug) << "Playnite: received " << msg.categories.size() << " categories";
        // Cache distinct categories (by id when available) and treat as the start of a new snapshot for games
        {
          std::scoped_lock lk(mutex_);
          // Prefer id for uniqueness; fall back to name when id is missing
          std::unordered_set<std::string> seen;
          last_categories_.clear();
          for (const auto &c : msg.categories) {
            std::string key = !c.id.empty() ? ("id:" + c.id) : ("name:" + c.name);
            if (seen.insert(key).second) {
              last_categories_.push_back(c);
            }
          }
          std::sort(last_categories_.begin(), last_categories_.end(), [](const auto &a, const auto &b) {
            return a.name < b.name;
          });
          new_snapshot_ = true;
        }
        // Best-effort: refresh persisted names (categories) using latest snapshot
        {
          std::vector<platf::playnite::Category> cats_copy;
          std::vector<platf::playnite::Game> games_copy;
          std::vector<platf::playnite::Plugin> plugins_copy;
          {
            std::scoped_lock lk(mutex_);
            cats_copy = last_categories_;
            games_copy = last_games_;
            plugins_copy = last_plugins_;
          }
          refresh_config_id_name_fields(cats_copy, games_copy, plugins_copy);
        }
        {
          std::scoped_lock lk(progress_mutex_);
          snapshot_progress_ = {};
        }
      } else if (msg.type == MT::Plugins) {
        BOOST_LOG(debug) << "Playnite: received " << msg.plugins.size() << " plugins";
        {
          std::scoped_lock lk(mutex_);
          std::unordered_set<std::string> seen;
          last_plugins_.clear();
          for (const auto &p : msg.plugins) {
            std::string key;
            if (!p.id.empty()) {
              key = platf::playnite::sync::to_lower_copy(p.id);
            } else if (!p.name.empty()) {
              key = "name:" + platf::playnite::sync::to_lower_copy(p.name);
            }
            if (key.empty()) {
              continue;
            }
            if (seen.insert(key).second) {
              last_plugins_.push_back(p);
            }
          }
          std::sort(last_plugins_.begin(), last_plugins_.end(), [](const auto &a, const auto &b) {
            return a.name < b.name;
          });
        }
        {
          std::vector<platf::playnite::Category> cats_copy;
          std::vector<platf::playnite::Game> games_copy;
          std::vector<platf::playnite::Plugin> plugins_copy;
          {
            std::scoped_lock lk(mutex_);
            cats_copy = last_categories_;
            games_copy = last_games_;
            plugins_copy = last_plugins_;
          }
          refresh_config_id_name_fields(cats_copy, games_copy, plugins_copy);
        }
      } else if (msg.type == MT::Games) {
        size_t added = 0;
        size_t skipped = 0;
        size_t before = 0;
        {
          std::scoped_lock lk(mutex_);
          if (new_snapshot_) {
            // Beginning a new snapshot accumulation.
            last_games_.clear();
            game_ids_.clear();
            new_snapshot_ = false;
          }
          before = last_games_.size();
          for (const auto &g : msg.games) {
            if (g.id.empty()) {
              skipped++;
              continue;
            }
            auto [it, ins] = game_ids_.insert(g.id);
            if (!ins) {
              skipped++;
              continue;
            }
            last_games_.push_back(g);
            added++;
          }
        }
        SyncStats sync_stats;
        bool attempted_sync = false;
        // Best-effort: refresh persisted names (games) using latest snapshot so UI has names offline
        {
          std::vector<platf::playnite::Category> cats_copy;
          std::vector<platf::playnite::Game> games_copy;
          std::vector<platf::playnite::Plugin> plugins_copy;
          {
            std::scoped_lock lk(mutex_);
            cats_copy = last_categories_;
            games_copy = last_games_;
            plugins_copy = last_plugins_;
          }
          refresh_config_id_name_fields(cats_copy, games_copy, plugins_copy);
        }
        if (config::playnite.auto_sync) {
          sync_stats = sync_apps_metadata();
          attempted_sync = true;
        }
        std::ostringstream line;
        line << "Playnite: library update games=" << msg.games.size()
             << " added=" << added
             << " skipped=" << skipped
             << " total=" << (before + added);
        if (attempted_sync) {
          line << " auto_sync " << sync_summary(sync_stats);
        } else {
          line << " auto_sync disabled";
        }
        BOOST_LOG(debug) << line.str();
        {
          std::scoped_lock lk(progress_mutex_);
          snapshot_progress_.batches += 1;
          snapshot_progress_.received += msg.games.size();
          snapshot_progress_.added += added;
          snapshot_progress_.skipped += skipped;
          snapshot_progress_.total_unique = before + added;
          if (attempted_sync) {
            snapshot_progress_.has_sync = true;
            snapshot_progress_.last_sync = sync_stats;
          }
          snapshot_progress_.pending_info = true;
          snapshot_progress_.last_update = std::chrono::steady_clock::now();
        }
      } else if (msg.type == MT::Status) {
        BOOST_LOG(debug) << "Playnite: status '" << msg.status_name
                         << "' id='" << msg.status_game_id
                         << "' exe='" << msg.status_exe
                         << "' installDir='" << msg.status_install_dir << "'";
        if (msg.status_name == "gameStarted") {
          playnite_session_tracker().on_started(msg.status_game_id);
        } else if (msg.status_name == "gameStopped") {
          auto guard = proc::proc.active_session_guard();
          if (!guard.has_active_app || !guard.uses_playnite) {
            BOOST_LOG(debug) << "Playnite: ignoring gameStopped because no active Playnite-backed app";
            return;
          }
          if (!msg.status_game_id.empty() && !guard.playnite_id.empty() && msg.status_game_id != guard.playnite_id) {
            BOOST_LOG(debug) << "Playnite: ignoring gameStopped for id='" << msg.status_game_id
                             << "' (active Playnite id='" << guard.playnite_id << "')";
            return;
          }
          if (!playnite_session_tracker().allow_stop(msg.status_game_id)) {
            BOOST_LOG(debug) << "Playnite: ignoring gameStopped because no prior gameStarted for this session";
            return;
          }
          auto now = std::chrono::steady_clock::now();
          if (guard.launch_started_at.time_since_epoch().count() != 0 &&
              now - guard.launch_started_at < std::chrono::seconds(2)) {
            BOOST_LOG(debug) << "Playnite: ignoring gameStopped within session guard window";
            return;
          }
          BOOST_LOG(debug) << "Playnite: received gameStopped; terminating active process";
          proc::proc.terminate();
        }
      } else {
        // Truncate and log a preview of the raw message for diagnostics
        std::string preview;
        preview.assign((const char *) bytes.data(), std::min<size_t>(bytes.size(), 256));
        for (auto &ch : preview) {
          if (ch == '\n' || ch == '\r') {
            ch = ' ';
          }
        }
        BOOST_LOG(warning) << "Playnite: unrecognized message; size=" << bytes.size() << " preview='" << preview << "'";
      }
    }

    SyncStats sync_apps_metadata() try {
      using nlohmann::json;
      const std::string path = config::stream.file_apps;
      SyncStats stats;
      std::string content = file_handler::read_file(path.c_str());
      stats.file_size = content.size();
      json root = json::parse(content);
      if (!root.contains("apps") || !root["apps"].is_array()) {
        BOOST_LOG(warning) << "apps.json has no 'apps' array";
        stats.error = "missing apps array";
        return stats;
      }

      // Build all games snapshot and reconcile with apps.json via helper
      std::vector<platf::playnite::Game> all;
      {
        std::scoped_lock lk(mutex_);
        all = last_games_;
      }
      int recentN = std::max(0, config::playnite.recent_games);
      int recent_age_days = std::max(0, config::playnite.recent_max_age_days);
      int delete_after_days = std::max(0, config::playnite.autosync_delete_after_days);
      bool changed = false;
      std::size_t matched = 0;
      platf::playnite::sync::autosync_reconcile(root,
                                                all,
                                                recentN,
                                                recent_age_days,
                                                delete_after_days,
                                                config::playnite.autosync_require_replacement,
                                                config::playnite.sync_all_installed,
                                                config::playnite.sync_categories,
                                                config::playnite.sync_plugins,
                                                config::playnite.exclude_categories,
                                                config::playnite.exclude_games,
                                                config::playnite.exclude_plugins,
                                                config::playnite.autosync_remove_uninstalled,
                                                changed,
                                                matched);
      if (changed) {
        platf::playnite::sync::write_and_refresh_apps(root, config::stream.file_apps);
      }
      stats.success = true;
      stats.changed = changed;
      stats.matched = matched;
      return stats;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Playnite sync failed for '" << config::stream.file_apps << "': " << e.what();
      SyncStats stats;
      stats.error = e.what();
      return stats;
    } catch (...) {
      BOOST_LOG(error) << "Playnite sync failed: unknown error reading/parsing '" << config::stream.file_apps << "'";
      SyncStats stats;
      stats.error = "unknown error";
      return stats;
    }

    std::vector<platf::playnite::Game> last_games_;
    std::vector<platf::playnite::Plugin> last_plugins_;
    std::mutex mutex_;

    struct SnapshotProgress {
      std::size_t batches = 0;
      std::size_t received = 0;
      std::size_t added = 0;
      std::size_t skipped = 0;
      std::size_t total_unique = 0;
      bool has_sync = false;
      SyncStats last_sync;
      bool pending_info = false;
      std::chrono::steady_clock::time_point last_update {};
    };

    void emit_snapshot_summary_if_ready() {
      SnapshotProgress snapshot;
      bool should_log = false;
      {
        std::scoped_lock lk(progress_mutex_);
        if (snapshot_progress_.pending_info && snapshot_progress_.last_update.time_since_epoch().count() != 0) {
          auto now = std::chrono::steady_clock::now();
          if (now - snapshot_progress_.last_update > std::chrono::seconds(2)) {
            snapshot = snapshot_progress_;
            snapshot_progress_.pending_info = false;
            should_log = true;
          }
        }
      }
      if (!should_log) {
        return;
      }
      std::ostringstream line;
      line << "Playnite: library snapshot completed batches=" << snapshot.batches
           << " received=" << snapshot.received
           << " added=" << snapshot.added
           << " skipped=" << snapshot.skipped
           << " total=" << snapshot.total_unique;
      if (snapshot.has_sync) {
        line << " auto_sync " << sync_summary(snapshot.last_sync);
      } else {
        line << " auto_sync disabled";
      }
      BOOST_LOG(info) << line.str();
    }

    std::unique_ptr<platf::playnite::IpcClient> client_;
    std::atomic<bool> stop_flag_ {false};
    std::thread manager_;
    bool new_snapshot_ = true;  // Indicates next games message starts a new accumulation
    std::unordered_set<std::string> game_ids_;  // Track unique IDs during accumulation
    std::vector<platf::playnite::Category> last_categories_;  // Last known categories (id+name)
    SnapshotProgress snapshot_progress_;
    std::mutex progress_mutex_;
  };

  std::unique_ptr<::platf::deinit_t> start() {
    return std::make_unique<deinit_t_impl>();
  }

  bool is_active() {
    auto inst = g_instance.load(std::memory_order_acquire);
    if (inst) {
      return inst->is_server_active();
    }
    return false;
  }

  // Consolidated helper: query association for 'playnite' to resolve executable path
  static bool query_assoc_for_playnite(std::wstring &outExe) {
    HANDLE user_token = nullptr;
    if (platf::dxgi::is_running_as_system()) {
      user_token = acquire_preferred_user_token_for_playnite();
    }
    std::wstring exe;
    {
      static std::mutex per_user_key_mutex;
      auto lg = std::lock_guard(per_user_key_mutex);
      if (!platf::override_per_user_predefined_keys(user_token)) {
        BOOST_LOG(debug) << "Playnite: per-user registry override failed (no active session?)";
        if (user_token) {
          CloseHandle(user_token);
        }
        return false;
      }

      // Try ASSOCSTR_EXECUTABLE first
      std::array<WCHAR, 4096> exe_buf {};
      DWORD out_len = static_cast<DWORD>(exe_buf.size());
      HRESULT hr = AssocQueryStringW(ASSOCF_NOTRUNCATE, ASSOCSTR_EXECUTABLE, L"playnite", nullptr, exe_buf.data(), &out_len);
      if (hr == S_OK) {
        exe.assign(exe_buf.data());
      }

      // Fallback to ASSOCSTR_COMMAND and parse
      if (exe.empty()) {
        std::array<WCHAR, 4096> cmd_buf {};
        out_len = static_cast<DWORD>(cmd_buf.size());
        hr = AssocQueryStringW(ASSOCF_NOTRUNCATE, ASSOCSTR_COMMAND, L"playnite", L"open", cmd_buf.data(), &out_len);
        if (hr == S_OK) {
          int argc = 0;
          auto argv = CommandLineToArgvW(cmd_buf.data(), &argc);
          if (argv && argc >= 1) {
            exe.assign(argv[0]);
            LocalFree(argv);
          } else {
            std::wstring s {cmd_buf.data()};
            if (!s.empty() && s.front() == L'"') {
              auto p = s.find(L'"', 1);
              if (p != std::wstring::npos) {
                exe = s.substr(1, p - 1);
              }
            } else {
              auto p = s.find(L' ');
              exe = (p == std::wstring::npos) ? s : s.substr(0, p);
            }
          }
        }
      }

      platf::override_per_user_predefined_keys(nullptr);
    }
    if (user_token) {
      CloseHandle(user_token);
    }
    if (exe.empty() || !std::filesystem::exists(exe)) {
      return false;
    }
    outExe = exe;
    return true;
  }

  // Resolve the Playnite Extensions/SunshinePlaynite directory via the "playnite" URL association.
  // Uses per-user registry views and impersonates the active user before calling AssocQueryString.
  static bool resolve_extensions_dir_via_assoc(std::filesystem::path &destOut) {
    std::wstring exe_path_w;
    if (!query_assoc_for_playnite(exe_path_w)) {
      return false;
    }
    std::filesystem::path exePath = exe_path_w;
    std::filesystem::path base = exePath.parent_path();
    destOut = base / L"Extensions" / L"SunshinePlaynite";
    return true;
  }

  // Resolve Playnite executable via 'playnite' URL association (per-user), falling back to command parsing.
  static bool resolve_playnite_exe_via_assoc(std::wstring &exe_out) {
    return query_assoc_for_playnite(exe_out);
  }

  bool get_extension_target_dir(std::string &out) {
    std::filesystem::path dest;
    if (!resolve_extensions_dir_via_assoc(dest)) {
      return false;
    }
    out = dest.string();
    return true;
  }

  bool launch_game(const std::string &playnite_id) {
    auto inst = g_instance.load(std::memory_order_acquire);
    if (!inst) {
      return false;
    }
    // Build a simple command JSON that the plugin reads line-delimited
    nlohmann::json j;
    j["type"] = "command";
    j["command"] = "launch";
    j["id"] = playnite_id;
    std::string s = j.dump();
    return inst->send_cmd_json_line(s);
  }

  bool announce_launcher(uint32_t pid, const std::string &game_id) {
    auto inst = g_instance.load(std::memory_order_acquire);
    if (!inst) {
      return false;
    }
    nlohmann::json j;
    j["type"] = "launcher";
    j["command"] = "announce";
    if (pid != 0) {
      j["pid"] = pid;
    }
    if (!game_id.empty()) {
      j["gameId"] = game_id;
    }
    return inst->send_cmd_json_line(j.dump());
  }

  bool get_games_list_json(std::string &out_json) {
    auto inst = g_instance.load(std::memory_order_acquire);
    if (!inst) {
      return false;
    }
    nlohmann::json arr = nlohmann::json::array();
    std::vector<platf::playnite::Game> copy;
    inst->snapshot_games(copy);
    try {
      auto &vec = arr.get_ref<nlohmann::json::array_t &>();
      vec.reserve(copy.size());
    } catch (...) {}
    for (const auto &g : copy) {
      nlohmann::json j;
      j["id"] = g.id;
      j["name"] = g.name;
      j["categories"] = g.categories;
      j["installed"] = g.installed;
      j["pluginId"] = g.plugin_id;
      j["pluginName"] = g.plugin_name;
      arr.push_back(std::move(j));
    }
    out_json = arr.dump();
    return true;
  }

  bool get_categories_list_json(std::string &out_json) {
    auto inst = g_instance.load(std::memory_order_acquire);
    if (!inst) {
      return false;
    }
    std::vector<platf::playnite::Category> cats;
    inst->snapshot_categories(cats);
    if (cats.empty()) {
      std::vector<platf::playnite::Game> copy;
      inst->snapshot_games(copy);
      // Build object list with names only (id unknown)
      std::unordered_set<std::string> uniq;
      std::vector<platf::playnite::Category> tmp;
      for (const auto &g : copy) {
        for (const auto &cname : g.categories) {
          if (!cname.empty() && uniq.insert(cname).second) {
            tmp.push_back(platf::playnite::Category {std::string(), cname});
          }
        }
      }
      std::sort(tmp.begin(), tmp.end(), [](const auto &a, const auto &b) {
        return a.name < b.name;
      });
      cats = std::move(tmp);
    }
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &c : cats) {
      nlohmann::json j;
      j["id"] = c.id;
      j["name"] = c.name;
      arr.push_back(std::move(j));
    }
    out_json = arr.dump();
    return true;
  }

  bool get_plugins_list_json(std::string &out_json) {
    auto inst = g_instance.load(std::memory_order_acquire);
    if (!inst) {
      return false;
    }
    std::vector<platf::playnite::Plugin> plugins;
    inst->snapshot_plugins(plugins);
    if (plugins.empty()) {
      std::vector<platf::playnite::Game> games;
      inst->snapshot_games(games);
      std::unordered_map<std::string, std::string> by_id;
      for (const auto &g : games) {
        if (!g.plugin_id.empty()) {
          if (!by_id.count(g.plugin_id) || by_id[g.plugin_id].empty()) {
            by_id[g.plugin_id] = g.plugin_name;
          }
        }
      }
      plugins.reserve(by_id.size());
      for (const auto &kv : by_id) {
        platf::playnite::Plugin p;
        p.id = kv.first;
        p.name = kv.second;
        plugins.push_back(std::move(p));
      }
      std::sort(plugins.begin(), plugins.end(), [](const auto &a, const auto &b) {
        return a.name < b.name;
      });
    }
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &p : plugins) {
      nlohmann::json j;
      j["id"] = p.id;
      j["name"] = p.name;
      arr.push_back(std::move(j));
    }
    out_json = arr.dump();
    return true;
  }

  // Reconcile persisted config names for categories/exclusions using latest snapshots
  static void refresh_config_id_name_fields(const std::vector<platf::playnite::Category> &cats,
                                            const std::vector<platf::playnite::Game> &games,
                                            const std::vector<platf::playnite::Plugin> &plugins) {
    try {
      // Build lookup maps
      std::unordered_map<std::string, std::string> cat_by_id;  // id->name
      std::unordered_map<std::string, std::string> cat_id_by_name;  // name->id (best effort)
      for (const auto &c : cats) {
        if (!c.id.empty()) {
          cat_by_id[c.id] = c.name;
        }
        if (!c.name.empty()) {
          cat_id_by_name[c.name] = c.id;
        }
      }
      std::unordered_map<std::string, std::string> game_name_by_id;
      std::unordered_map<std::string, std::string> plugin_name_by_id;
      std::unordered_map<std::string, std::string> plugin_id_by_name;
      for (const auto &g : games) {
        if (!g.id.empty()) {
          game_name_by_id[g.id] = g.name;
        }
      }
      for (const auto &p : plugins) {
        if (!p.id.empty()) {
          plugin_name_by_id[p.id] = p.name;
        }
        if (!p.name.empty()) {
          plugin_id_by_name[p.name] = p.id;
        }
      }

      // Load config
      auto current = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
      bool changed = false;

      auto parse_any = [](const std::string &raw) -> nlohmann::json {
        try {
          return nlohmann::json::parse(raw);
        } catch (...) {
          return nlohmann::json();
        }
      };

      auto update_array = [&](const char *key, bool treat_strings_as_ids, const auto &resolver) {
        auto it = current.find(key);
        if (it == current.end()) {
          return;
        }
        nlohmann::json j = parse_any(it->second);
        std::vector<nlohmann::json> out;
        bool local_changed = false;
        auto push_obj = [&](const std::string &id, const std::string &name) {
          nlohmann::json o;
          o["id"] = id;
          o["name"] = name;
          out.push_back(std::move(o));
        };
        if (j.is_array()) {
          for (auto &el : j) {
            std::string id, name;
            if (el.is_object()) {
              id = el.value("id", std::string());
              name = el.value("name", std::string());
            } else if (el.is_string()) {
              if (treat_strings_as_ids) {
                id = el.get<std::string>();
              } else {
                name = el.get<std::string>();
              }
            }
            // Resolve missing/mismatched pieces
            std::string rid = id, rname = name;
            resolver(rid, rname);
            if (rid != id || rname != name) {
              local_changed = true;
            }
            push_obj(rid, rname);
          }
        } else {
          // CSV fallback
          std::stringstream ss(it->second);
          std::string item;
          while (std::getline(ss, item, ',')) {
            item.erase(item.begin(), std::find_if(item.begin(), item.end(), [](unsigned char ch) {
                         return !std::isspace(ch);
                       }));
            item.erase(std::find_if(item.rbegin(), item.rend(), [](unsigned char ch) {
                         return !std::isspace(ch);
                       }).base(),
                       item.end());
            if (item.empty()) {
              continue;
            }
            std::string rid, rname;
            if (treat_strings_as_ids) {
              rid = item;
            } else {
              rname = item;
            }
            resolver(rid, rname);
            push_obj(rid, rname);
            local_changed = true;  // converted to objects
          }
        }
        if (local_changed) {
          nlohmann::json arr = nlohmann::json::array();
          for (auto &o : out) {
            arr.push_back(std::move(o));
          }
          current[key] = arr.dump();
          changed = true;
        }
      };

      // Categories: complete id/name using snapshot
      update_array("playnite_sync_categories", /*treat_strings_as_ids=*/false, [&](std::string &id, std::string &name) {
        if (!id.empty() && cat_by_id.count(id)) {
          name = cat_by_id[id];
          return;
        }
        if (!name.empty() && cat_id_by_name.count(name)) {
          id = cat_id_by_name[name];
          return;
        }
        // Not resolvable: leave as-is
      });
      // Excluded categories: mirror resolution behavior so offline labels stay fresh
      update_array("playnite_exclude_categories", /*treat_strings_as_ids=*/false, [&](std::string &id, std::string &name) {
        if (!id.empty() && cat_by_id.count(id)) {
          name = cat_by_id[id];
          return;
        }
        if (!name.empty() && cat_id_by_name.count(name)) {
          id = cat_id_by_name[name];
          return;
        }
      });
      // Included plugins: ensure id/name pairs stay synchronized
      update_array("playnite_sync_plugins", /*treat_strings_as_ids=*/true, [&](std::string &id, std::string &name) {
        if (!id.empty() && plugin_name_by_id.count(id)) {
          name = plugin_name_by_id[id];
          return;
        }
        if (!name.empty() && plugin_id_by_name.count(name)) {
          id = plugin_id_by_name[name];
        }
      });
      // Excluded games: ensure names match latest snapshot
      update_array("playnite_exclude_games", /*treat_strings_as_ids=*/true, [&](std::string &id, std::string &name) {
        if (!id.empty() && game_name_by_id.count(id)) {
          name = game_name_by_id[id];
        }
      });
      // Excluded plugins: ensure names match latest snapshot
      update_array("playnite_exclude_plugins", /*treat_strings_as_ids=*/true, [&](std::string &id, std::string &name) {
        if (!id.empty() && plugin_name_by_id.count(id)) {
          name = plugin_name_by_id[id];
        }
      });

      if (changed) {
        std::stringstream config_stream;
        for (const auto &kv : current) {
          config_stream << kv.first << " = " << kv.second << std::endl;
        }
        file_handler::write_file(config::sunshine.config_file.c_str(), config_stream.str());
        BOOST_LOG(info) << "Playnite: refreshed id/name fields in config";
      }
    } catch (...) {
      // best-effort; ignore errors
    }
  }

  bool stop_game(const std::string &playnite_id) {
    auto inst = g_instance.load(std::memory_order_acquire);
    if (!inst) {
      return false;
    }
    nlohmann::json j;
    j["type"] = "command";
    j["command"] = "stop";
    if (!playnite_id.empty()) {
      j["id"] = playnite_id;
    }
    return inst->send_cmd_json_line(j.dump());
  }

  bool force_sync() {
    auto inst = g_instance.load(std::memory_order_acquire);
    if (!inst) {
      return false;
    }
    inst->trigger_sync();
    return true;
  }

  bool get_cover_png_for_playnite_game(const std::string &playnite_id, std::string &out_path) {
    auto inst = g_instance.load(std::memory_order_acquire);
    if (!inst) {
      return false;
    }
    // Snapshot games
    std::vector<platf::playnite::Game> copy;
    inst->snapshot_games(copy);
    const platf::playnite::Game *gptr = nullptr;
    for (const auto &g : copy) {
      if (g.id == playnite_id) {
        gptr = &g;
        break;
      }
    }
    if (!gptr || gptr->box_art_path.empty()) {
      return false;
    }

    try {
      std::filesystem::path src = gptr->box_art_path;
      std::filesystem::path dstDir = platf::appdata() / "covers";
      file_handler::make_directory(dstDir.string());
      std::filesystem::path dst = dstDir / ("playnite_" + playnite_id + ".png");
      bool ok = false;
      std::error_code ec1, ec2;
      if (std::filesystem::exists(dst)) {
        auto dstTime = std::filesystem::last_write_time(dst, ec1);
        auto srcTime = std::filesystem::last_write_time(src, ec2);
        if (!ec1 && !ec2 && dstTime >= srcTime) {
          ok = true;
        }
      }
      if (!ok) {
        std::wstring wsrc = src.wstring();
        std::wstring wdst = dst.wstring();
        ok = platf::img::convert_to_png_96dpi(wsrc, wdst);
      }
      if (ok) {
        out_path = dst.generic_string();
        return true;
      }
    } catch (...) {}
    return false;
  }

  // Helper: gather running Playnite PIDs and capture any discovered exe path
  static void collect_playnite_state(std::vector<DWORD> &pids, std::wstring &exe_path_out) {
    try {
      auto d = platf::dxgi::find_process_ids_by_name(L"Playnite.DesktopApp.exe");
      auto f = platf::dxgi::find_process_ids_by_name(L"Playnite.FullscreenApp.exe");
      pids = d;
      pids.insert(pids.end(), f.begin(), f.end());
      for (DWORD pid : pids) {
        HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hp) {
          continue;
        }
        std::wstring buf;
        buf.resize(32768);
        DWORD size = static_cast<DWORD>(buf.size());
        if (QueryFullProcessImageNameW(hp, 0, buf.data(), &size)) {
          buf.resize(size);
          exe_path_out = buf;
          CloseHandle(hp);
          break;
        }
        CloseHandle(hp);
      }
    } catch (...) {
      // best-effort
    }
  }

  // Helper: attempt to resolve a Playnite Desktop exe path if not running
  static bool resolve_playnite_exe_path(std::wstring &exe_path_out) {
    // Try the active user's LocalAppData path
    try {
      HANDLE user_token = acquire_preferred_user_token_for_playnite();
      if (user_token) {
        PWSTR local = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, user_token, &local)) && local) {
          std::filesystem::path p = std::filesystem::path(local) / L"Playnite" / L"Playnite.DesktopApp.exe";
          CoTaskMemFree(local);
          if (std::filesystem::exists(p)) {
            CloseHandle(user_token);
            exe_path_out = p.wstring();
            return true;
          }
        }
        CloseHandle(user_token);
      }
    } catch (...) {}

    // Fall back to current process' LocalAppData
    try {
      wchar_t buf[MAX_PATH] = {};
      if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf))) {
        std::filesystem::path p = std::filesystem::path(buf) / L"Playnite" / L"Playnite.DesktopApp.exe";
        if (std::filesystem::exists(p)) {
          exe_path_out = p.wstring();
          return true;
        }
      }
    } catch (...) {}

    return false;
  }

  // Close by posting WM_CLOSE to windows owned by process name, then kill leftovers
  static void close_then_kill_by_name(const wchar_t *exeName) {
    struct Ctx {
      const wchar_t *name;
      std::vector<DWORD> pids;
    } ctx {exeName, {}};

    // Build a set of target PIDs by name
    try {
      auto ids = platf::dxgi::find_process_ids_by_name(exeName);
      ctx.pids.insert(ctx.pids.end(), ids.begin(), ids.end());
    } catch (...) {}

    if (!ctx.pids.empty()) {
      BOOST_LOG(debug) << "Playnite: posting WM_CLOSE to " << ctx.pids.size() << " window(s) for '" << platf::to_utf8(std::wstring(exeName)) << "'";
      EnumWindows([](HWND hwnd, LPARAM lparam) -> BOOL {
        auto *c = reinterpret_cast<Ctx *>(lparam);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid) {
          return TRUE;
        }
        for (DWORD p : c->pids) {
          if (p == pid) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            break;
          }
        }
        return TRUE;
      },
                  reinterpret_cast<LPARAM>(&ctx));
    }

    Sleep(1200);

    // Kill any remaining processes by name
    try {
      auto ids = platf::dxgi::find_process_ids_by_name(exeName);
      if (!ids.empty()) {
        BOOST_LOG(debug) << "Playnite: terminating remaining processes for '" << platf::to_utf8(std::wstring(exeName)) << "' count=" << ids.size();
      }
      for (DWORD pid : ids) {
        HANDLE hp = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hp) {
          continue;
        }
        DWORD code = 0;
        if (!GetExitCodeProcess(hp, &code) || code == STILL_ACTIVE) {
          TerminateProcess(hp, 1);
        }
        CloseHandle(hp);
      }
    } catch (...) {}
  }

  bool restart_playnite() {
    // 1) Collect current state and attempt graceful-then-force close from the user's session
    std::vector<DWORD> pids;
    std::wstring running_exe;
    collect_playnite_state(pids, running_exe);
    // Gracefully request close then kill stragglers, native
    close_then_kill_by_name(L"Playnite.DesktopApp.exe");
    close_then_kill_by_name(L"Playnite.FullscreenApp.exe");

    // 2) Determine exe path to start
    std::wstring exe;
    if (!running_exe.empty()) {
      exe = running_exe;
    } else {
      // Prefer URL association (per-user) to determine the Playnite executable
      if (!resolve_playnite_exe_via_assoc(exe) && !resolve_playnite_exe_path(exe)) {
        BOOST_LOG(warning) << "Playnite restart: could not resolve Playnite executable path";
        // Even if we couldn't resolve path, treat close attempt as success
        return false;
      }
    }

    // 3) Launch Playnite (impersonates active user when running as SYSTEM)
    std::filesystem::path exePath = exe;
    std::filesystem::path startDir = exePath.parent_path();
    std::string cmd = platf::to_utf8(exe);
    std::error_code ec_launch;
    // platf::run_command expects a boost::filesystem::path&
    boost::filesystem::path boostStartDir = boost::filesystem::path(startDir.wstring());
    if (platf::dxgi::is_running_as_system()) {
      HANDLE tok = acquire_preferred_user_token_for_playnite();
      if (tok) {
        bool ok = launch_exe_as_token(tok, exe, startDir.wstring());
        CloseHandle(tok);
        if (!ok) {
          BOOST_LOG(warning) << "Playnite restart: CreateProcessAsUser failed";
          return false;
        }
        BOOST_LOG(info) << "Playnite restart: launched (token) " << cmd;
        return true;
      }
      BOOST_LOG(warning) << "Playnite restart: no suitable user token found; falling back";
    }

    // Non-SYSTEM or fallback path
    {
      auto env = bp::this_process::env();
      auto child2 = platf::run_command(false, true, cmd, boostStartDir, env, nullptr, ec_launch, nullptr);
      if (ec_launch) {
        BOOST_LOG(warning) << "Playnite restart: launch failed: " << ec_launch.message();
        return false;
      }
      child2.detach();
      BOOST_LOG(info) << "Playnite restart: launched " << cmd;
      return true;
    }
  }

  // explicit launch-only helper removed; use restart_playnite()

  static bool do_install_plugin_impl(const std::string &dest_override, std::string &error_out) {
    try {
      // Determine source directory: alongside the Sunshine executable under plugins/playnite/SunshinePlaynite
      std::wstring exePath;
      exePath.resize(MAX_PATH);
      GetModuleFileNameW(nullptr, exePath.data(), (DWORD) exePath.size());
      exePath.resize(wcslen(exePath.c_str()));
      std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
      std::filesystem::path srcDir = exeDir / L"plugins" / L"playnite" / L"SunshinePlaynite";
      BOOST_LOG(debug) << "Playnite installer: srcDir=" << srcDir.string();
      BOOST_LOG(debug) << "Playnite installer: src exists? " << (std::filesystem::exists(srcDir) ? "yes" : "no");
      BOOST_LOG(debug) << "Playnite installer: src file(extension.yaml) exists? " << (std::filesystem::exists(srcDir / L"extension.yaml") ? "yes" : "no");
      BOOST_LOG(debug) << "Playnite installer: src file(SunshinePlaynite.psm1) exists? " << (std::filesystem::exists(srcDir / L"SunshinePlaynite.psm1") ? "yes" : "no");
      if (!std::filesystem::exists(srcDir)) {
        error_out = "Plugin source not found: " + srcDir.string();
        return false;
      }

      // Determine destination directory (support SYSTEM context and running Playnite)
      std::filesystem::path destDir;
      if (!dest_override.empty()) {
        destDir = std::filesystem::path(dest_override);
        BOOST_LOG(debug) << "Playnite installer: using API override destDir=" << destDir.string();
      } else {
        // Prefer the same resolution used by status API
        std::string resolved;
        if (!platf::playnite::get_extension_target_dir(resolved)) {
          error_out = "Could not resolve Playnite Extensions directory (and no override provided).";
          return false;
        }
        destDir = std::filesystem::path(resolved);
        BOOST_LOG(debug) << "Playnite installer: using resolved target dir from API=" << destDir.string();
      }
      std::error_code ec;
      if (!std::filesystem::create_directories(destDir, ec) && ec) {
        error_out = std::string("Failed to create destination directory: ") + destDir.string() + " (" + ec.message() + ")";
        return false;
      }

      auto copy_one = [&](const wchar_t *name) {
        ec.clear();
        auto src = srcDir / name;
        auto dst = destDir / name;
        std::wstring wname(name);
        std::string sname(wname.begin(), wname.end());
        BOOST_LOG(debug) << "Playnite installer: copying " << sname << " from " << src.string() << " to " << dst.string();
        std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
        return !ec;
      };

      if (!copy_one(L"extension.yaml") || !copy_one(L"SunshinePlaynite.psm1")) {
        error_out = "Failed to copy plugin files to " + destDir.string();
        return false;
      }
      BOOST_LOG(info) << "Playnite installer: deployed plugin to " << destDir.string();
      return true;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Playnite installer: exception: " << e.what();
      error_out = e.what();
      return false;
    }
  }

  bool install_plugin(std::string &error) {
    return do_install_plugin_impl(std::string(), error);
  }

  bool install_plugin_to(const std::string &dest_dir, std::string &error) {
    return do_install_plugin_impl(dest_dir, error);
  }

  static bool do_uninstall_plugin_impl(std::string &error) {
    try {
      std::string target;
      if (!platf::playnite::get_extension_target_dir(target)) {
        // If we cannot resolve the directory, consider it already uninstalled.
        BOOST_LOG(warning) << "Playnite uninstaller: could not resolve Extensions directory; assuming uninstalled";
        return true;
      }
      std::filesystem::path destDir = std::filesystem::path(target);
      if (!std::filesystem::exists(destDir)) {
        BOOST_LOG(info) << "Playnite uninstaller: target does not exist; nothing to do";
        return true;
      }
      std::error_code ec;
      auto removed = std::filesystem::remove_all(destDir, ec);
      if (ec) {
        error = std::string("Failed to remove plugin directory: ") + destDir.string() + " (" + ec.message() + ")";
        BOOST_LOG(warning) << "Playnite uninstaller: remove_all failed: " << ec.message();
        return false;
      }
      BOOST_LOG(info) << "Playnite uninstaller: removed files count=" << removed << " path=" << destDir.string();
      return true;
    } catch (const std::exception &e) {
      error = e.what();
      BOOST_LOG(warning) << "Playnite uninstaller: exception: " << e.what();
      return false;
    } catch (...) {
      error = "Unknown error";
      BOOST_LOG(warning) << "Playnite uninstaller: unknown exception";
      return false;
    }
  }

  bool uninstall_plugin(std::string &error) {
    return do_uninstall_plugin_impl(error);
  }

  // --- Version helpers ---
  static bool parse_yaml_version_from_file(const std::filesystem::path &p, std::string &out) {
    try {
      if (!std::filesystem::exists(p)) {
        return false;
      }
      std::ifstream ifs(p, std::ios::in | std::ios::binary);
      if (!ifs) {
        return false;
      }
      std::string line;
      while (std::getline(ifs, line)) {
        // Trim leading spaces
        size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
          i++;
        }
        // Case-insensitive startswith("Version:")
        const char *needle = "version:";
        if (line.size() >= i + 8) {
          bool match = true;
          for (size_t k = 0; k < 8; ++k) {
            char c = line[i + k];
            if (c >= 'A' && c <= 'Z') {
              c = char(c - 'A' + 'a');
            }
            if (c != needle[k]) {
              match = false;
              break;
            }
          }
          if (match) {
            size_t pos = i + 8;
            while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
              pos++;
            }
            if (pos < line.size() && line[pos] == ' ') {
              pos++;
            }
            // Optional whitespace after colon handled above
            // Extract remainder and trim
            std::string val = line.substr(pos);
            // Trim potential quotes and whitespace
            auto ltrim = [](std::string &s) {
              size_t j = 0;
              while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) {
                j++;
              }
              s.erase(0, j);
            };
            auto rtrim = [](std::string &s) {
              size_t j = s.size();
              while (j > 0 && (s[j - 1] == ' ' || s[j - 1] == '\t' || s[j - 1] == '\r')) {
                j--;
              }
              s.erase(j);
            };
            ltrim(val);
            rtrim(val);
            if (!val.empty() && (val.front() == '"' || val.front() == '\'')) {
              val.erase(val.begin());
            }
            if (!val.empty() && (val.back() == '"' || val.back() == '\'')) {
              val.pop_back();
            }
            out = val;
            return !out.empty();
          }
        }
      }
    } catch (...) {}
    return false;
  }

  bool get_packaged_plugin_version(std::string &out) {
    try {
      std::wstring exePath;
      exePath.resize(MAX_PATH);
      GetModuleFileNameW(nullptr, exePath.data(), (DWORD) exePath.size());
      exePath.resize(wcslen(exePath.c_str()));
      std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
      std::filesystem::path src = exeDir / L"plugins" / L"playnite" / L"SunshinePlaynite" / L"extension.yaml";
      std::string ver;
      if (!parse_yaml_version_from_file(src, ver)) {
        return false;
      }
      out = ver;
      return true;
    } catch (...) {
      return false;
    }
  }

  bool get_installed_plugin_version(std::string &out) {
    try {
      std::string dir;
      if (!get_extension_target_dir(dir)) {
        return false;
      }
      std::filesystem::path p = std::filesystem::path(dir) / "extension.yaml";
      std::string ver;
      if (!parse_yaml_version_from_file(p, ver)) {
        return false;
      }
      out = ver;
      return true;
    } catch (...) {
      return false;
    }
  }

}  // namespace platf::playnite
