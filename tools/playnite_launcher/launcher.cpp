#include "tools/playnite_launcher/launcher.h"

#include "src/logging.h"
#include "src/platform/windows/ipc/misc_utils.h"
#include "src/platform/windows/playnite_ipc.h"
#include "src/platform/windows/playnite_protocol.h"
#include "tools/playnite_launcher/arguments.h"
#include "tools/playnite_launcher/cleanup.h"
#include "tools/playnite_launcher/focus_utils.h"
#include "tools/playnite_launcher/lossless_scaling.h"
#include "tools/playnite_launcher/playnite_process.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <ShlObj.h>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

using namespace std::chrono_literals;

namespace playnite_launcher {
  namespace detail {

    std::filesystem::path resolve_log_path();
    void ensure_playnite_open();
    int64_t steady_to_millis(std::chrono::steady_clock::time_point tp);
    std::chrono::steady_clock::time_point millis_to_steady(int64_t ms);
    std::string normalize_game_id(std::string s);

    int run_cleanup_mode(const LauncherConfig &config, const lossless::lossless_scaling_options &lossless_options);
    int run_fullscreen_mode(LauncherConfig config, const lossless::lossless_scaling_options &lossless_options);
    int run_standard_mode(LauncherConfig config, const lossless::lossless_scaling_options &lossless_options);

  }  // namespace detail

  int launcher_run(int argc, char **argv) {
    auto parsed = parse_arguments(argc, argv);
    if (!parsed.success) {
      return parsed.exit_code;
    }
    LauncherConfig config = std::move(parsed.config);

    FreeConsole();

    auto log_path = detail::resolve_log_path();
    auto log_guard = logging::init_append(2, log_path.string());
    BOOST_LOG(info) << "Playnite launcher starting; pid=" << GetCurrentProcessId();

    auto lossless_options = lossless::read_lossless_scaling_options();

    // Adjust focus_attempts to account for lossless scaling focus attempt (1 additional)
    // This ensures the game still gets the configured number of focus attempts
    if (lossless_options.enabled && config.focus_attempts > 0) {
      config.focus_attempts += 1;
      BOOST_LOG(debug) << "Lossless Scaling enabled: adjusted focus_attempts by +1 to " << config.focus_attempts;
    }

    if (config.cleanup) {
      return detail::run_cleanup_mode(config, lossless_options);
    }
    if (config.fullscreen) {
      return detail::run_fullscreen_mode(config, lossless_options);
    }
    return detail::run_standard_mode(config, lossless_options);
  }

  namespace detail {

    std::filesystem::path resolve_log_path() {
      std::wstring appdataW;
      appdataW.resize(MAX_PATH);
      if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appdataW.data()))) {
        appdataW = L".";
      }
      appdataW.resize(wcslen(appdataW.c_str()));
      std::filesystem::path logdir = std::filesystem::path(appdataW) / L"Sunshine";
      std::error_code ec;
      std::filesystem::create_directories(logdir, ec);
      return logdir / L"sunshine_playnite_launcher.log";
    }

    void ensure_playnite_open() {
      if (!playnite::is_playnite_running()) {
        BOOST_LOG(info) << "Playnite not running; opening playnite:// URI in detached mode";
        if (!playnite::launch_uri_detached_parented(L"playnite://")) {
          BOOST_LOG(warning) << "Failed to launch playnite:// via detached CreateProcess";
        }
      }
    }

    int64_t steady_to_millis(std::chrono::steady_clock::time_point tp) {
      return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
    }

    std::chrono::steady_clock::time_point millis_to_steady(int64_t ms) {
      return std::chrono::steady_clock::time_point(std::chrono::milliseconds(ms));
    }

    std::string normalize_game_id(std::string s) {
      s.erase(std::remove_if(s.begin(), s.end(), [](char c) {
                return c == '{' || c == '}';
              }),
              s.end());
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return s;
    }

    int run_cleanup_mode(const LauncherConfig &config, const lossless::lossless_scaling_options &lossless_options) {
      BOOST_LOG(info) << "Cleanup mode: starting (installDir='" << config.install_dir << "' fullscreen=" << (config.fullscreen ? 1 : 0) << ")";
      if (!config.wait_for_pid.empty()) {
        try {
          DWORD wpid = static_cast<DWORD>(std::stoul(config.wait_for_pid));
          if (wpid != 0 && wpid != GetCurrentProcessId()) {
            HANDLE hp = OpenProcess(SYNCHRONIZE, FALSE, wpid);
            if (hp) {
              BOOST_LOG(info) << "Cleanup mode: waiting for PID=" << wpid << " to exit";
              DWORD wr = WaitForSingleObject(hp, INFINITE);
              CloseHandle(hp);
              BOOST_LOG(info) << "Cleanup mode: wait result=" << wr;
            } else {
              BOOST_LOG(warning) << "Cleanup mode: unable to open PID for wait: " << wpid;
            }
          }
        } catch (...) {
          BOOST_LOG(warning) << "Cleanup mode: invalid --wait-for-pid value: '" << config.wait_for_pid << "'";
        }
      }
      std::wstring install_dir_w;
      if (!config.install_dir.empty()) {
        install_dir_w = platf::dxgi::utf8_to_wide(config.install_dir);
      }
      if (!config.fullscreen && !install_dir_w.empty()) {
        cleanup::cleanup_graceful_then_forceful_in_dir(install_dir_w, config.exit_timeout_secs);
      }
      if (config.fullscreen) {
        cleanup::cleanup_fullscreen_via_desktop(std::max(3, config.exit_timeout_secs));
      }
      if (lossless_options.enabled) {
        auto runtime = lossless::capture_lossless_scaling_state();
        if (!runtime.running_pids.empty()) {
          lossless::lossless_scaling_stop_processes(runtime);
        }
      }
      BOOST_LOG(info) << "Cleanup mode: done";
      return 0;
    }

    int run_fullscreen_mode(LauncherConfig config, const lossless::lossless_scaling_options &lossless_options) {
      BOOST_LOG(info) << "Fullscreen mode: preparing IPC connection to Playnite plugin";

      platf::playnite::IpcClient client;
      std::atomic<bool> pipe_connected {false};

      std::atomic<bool> game_start_signal {false};
      std::atomic<bool> game_stop_signal {false};
      std::atomic<bool> cleanup_spawn_signal {false};
      std::atomic<bool> active_game_flag {false};
      std::atomic<int64_t> grace_deadline_ms {steady_to_millis(std::chrono::steady_clock::now() + std::chrono::seconds(15))};

      std::mutex game_mutex;

      struct FullscreenGameState {
        std::string id_original;
        std::string id_norm;
        std::string install_dir;
        std::string exe_path;
        std::string cleanup_dir;
      } game_state;

      std::mutex cleanup_mutex;
      std::string active_cleanup_dir;
      bool game_cleanup_spawned = false;
      std::atomic<bool> watcher_spawned {false};

      lossless::lossless_scaling_profile_backup fullscreen_lossless_backup {};
      bool fullscreen_lossless_applied = false;
      bool game_started_once = false;

      auto resolve_install_dir = [&](const std::string &install_dir, const std::string &exe_path) -> std::string {
        if (!install_dir.empty()) {
          return install_dir;
        }
        try {
          if (!exe_path.empty()) {
            std::wstring wexe = platf::dxgi::utf8_to_wide(exe_path);
            std::filesystem::path p(wexe);
            auto parent = p.parent_path();
            if (!parent.empty()) {
              return platf::dxgi::wide_to_utf8(parent.wstring());
            }
          }
        } catch (...) {
        }
        return std::string();
      };

      client.set_message_handler([&](std::span<const uint8_t> bytes) {
        auto msg = platf::playnite::parse(bytes);
        using MT = platf::playnite::MessageType;
        if (msg.type != MT::Status) {
          return;
        }
        auto norm_id = normalize_game_id(msg.status_game_id);
        auto now = std::chrono::steady_clock::now();
        if (msg.status_name == "gameStarted") {
          std::string install_for_ls;
          std::string exe_for_ls;
          {
            std::lock_guard<std::mutex> lk(game_mutex);
            game_state.id_original = msg.status_game_id;
            game_state.id_norm = norm_id;
            if (!msg.status_install_dir.empty()) {
              game_state.install_dir = msg.status_install_dir;
            }
            if (!msg.status_exe.empty()) {
              game_state.exe_path = msg.status_exe;
            }
            auto resolved = resolve_install_dir(game_state.install_dir, game_state.exe_path);
            if (!resolved.empty()) {
              game_state.install_dir = resolved;
              game_state.cleanup_dir = resolved;
            } else {
              game_state.cleanup_dir.clear();
            }
            install_for_ls = game_state.install_dir;
            exe_for_ls = game_state.exe_path;
          }
          game_started_once = true;
          active_game_flag.store(true);
          game_start_signal.store(true);
          cleanup_spawn_signal.store(true);
          grace_deadline_ms.store(steady_to_millis(now + std::chrono::seconds(15)));
          if (lossless_options.enabled && !fullscreen_lossless_applied) {
            auto runtime = lossless::capture_lossless_scaling_state();
            if (!runtime.running_pids.empty()) {
              lossless::lossless_scaling_stop_processes(runtime);
            }
            lossless::lossless_scaling_profile_backup backup;
            bool changed = lossless::lossless_scaling_apply_global_profile(lossless_options, install_for_ls, exe_for_ls, backup);
            if (backup.valid) {
              fullscreen_lossless_backup = backup;
              fullscreen_lossless_applied = true;
            } else {
              fullscreen_lossless_backup = {};
            }
            lossless::lossless_scaling_restart_foreground(runtime, changed, install_for_ls, exe_for_ls);
          }
        } else if (msg.status_name == "gameStopped") {
          bool matches = false;
          {
            std::lock_guard<std::mutex> lk(game_mutex);
            if (game_state.id_norm.empty() || norm_id.empty()) {
              matches = true;
            } else {
              matches = game_state.id_norm == norm_id;
            }
            if (matches) {
              game_state.id_original.clear();
              game_state.id_norm.clear();
            }
          }
          if (matches) {
            active_game_flag.store(false);
            game_stop_signal.store(true);
            grace_deadline_ms.store(steady_to_millis(std::chrono::steady_clock::now() + std::chrono::seconds(15)));
            if (fullscreen_lossless_applied) {
              auto runtime = lossless::capture_lossless_scaling_state();
              if (!runtime.running_pids.empty()) {
                lossless::lossless_scaling_stop_processes(runtime);
              }
              bool restored = lossless::lossless_scaling_restore_global_profile(fullscreen_lossless_backup);
              lossless::lossless_scaling_restart_foreground(runtime, restored);
              fullscreen_lossless_backup = {};
              fullscreen_lossless_applied = false;
            }
          }
        }
      });

      client.set_connected_handler([&]() {
        pipe_connected.store(true);
        try {
          nlohmann::json hello;
          hello["type"] = "hello";
          hello["role"] = "launcher";
          hello["pid"] = static_cast<uint32_t>(GetCurrentProcessId());
          hello["mode"] = "fullscreen";
          client.send_json_line(hello.dump());
        } catch (...) {}
      });

      client.set_disconnected_handler([&]() {
        pipe_connected.store(false);
      });

      client.start();

      BOOST_LOG(info) << "Fullscreen mode requested; attempting to start Playnite.DesktopApp.exe --startfullscreen";
      bool started = false;
      std::string fullscreen_install_dir_utf8;
      try {
        std::wstring assocExe = playnite::query_playnite_executable_from_assoc();
        if (!assocExe.empty()) {
          std::filesystem::path assoc_path(assocExe);
          std::filesystem::path base = assoc_path.parent_path();
          fullscreen_install_dir_utf8 = platf::dxgi::wide_to_utf8(base.wstring());
          std::filesystem::path desktopExe = base / L"Playnite.DesktopApp.exe";
          std::filesystem::path targetExe = desktopExe;
          if (!std::filesystem::exists(targetExe) && std::filesystem::exists(assoc_path)) {
            targetExe = assoc_path;
          }
          if (std::filesystem::exists(targetExe)) {
            BOOST_LOG(info) << "Launching Playnite Desktop with --startfullscreen from: " << platf::dxgi::wide_to_utf8(targetExe.wstring());
            started = playnite::launch_executable_detached_parented_with_args(targetExe.wstring(), L"--startfullscreen");
          }
          if (!started) {
            std::filesystem::path fullscreenExe = base / L"Playnite.FullscreenApp.exe";
            if (std::filesystem::exists(fullscreenExe)) {
              BOOST_LOG(info) << "Desktop launch failed; falling back to FullscreenApp from: " << platf::dxgi::wide_to_utf8(fullscreenExe.wstring());
              started = playnite::launch_executable_detached_parented(fullscreenExe.wstring());
            }
          }
        }
      } catch (...) {
      }
      if (!started) {
        BOOST_LOG(info) << "Playnite executable not resolved; falling back to playnite://";
        ensure_playnite_open();
      }

      WCHAR selfPath[MAX_PATH] = {};
      GetModuleFileNameW(nullptr, selfPath, ARRAYSIZE(selfPath));
      DWORD launcher_pid = GetCurrentProcessId();
      if (playnite::spawn_cleanup_watchdog_process(selfPath, fullscreen_install_dir_utf8, config.exit_timeout_secs, true, launcher_pid)) {
        watcher_spawned.store(true);
      } else {
        BOOST_LOG(warning) << "Fullscreen mode: failed to spawn cleanup watchdog";
      }

      auto spawn_game_cleanup = [&](const std::string &dir_utf8) {
        if (dir_utf8.empty()) {
          return;
        }
        std::lock_guard<std::mutex> lk(cleanup_mutex);
        if (active_cleanup_dir != dir_utf8) {
          active_cleanup_dir = dir_utf8;
          game_cleanup_spawned = false;
        }
        if (game_cleanup_spawned) {
          return;
        }
        if (playnite::spawn_cleanup_watchdog_process(selfPath, dir_utf8, config.exit_timeout_secs, false, GetCurrentProcessId())) {
          game_cleanup_spawned = true;
        } else if (active_cleanup_dir == dir_utf8) {
          game_cleanup_spawned = false;
        }
      };

      auto wait_deadline = std::chrono::steady_clock::now() + 10s;
      while (std::chrono::steady_clock::now() < wait_deadline) {
        auto pids = platf::dxgi::find_process_ids_by_name(L"Playnite.FullscreenApp.exe");
        if (!pids.empty()) {
          break;
        }
        std::this_thread::sleep_for(300ms);
      }

      auto cancel_fullscreen_focus = [&]() {
        return active_game_flag.load();
      };
      bool focused = focus::focus_process_by_name_extended(L"Playnite.FullscreenApp.exe", config.focus_attempts, config.focus_timeout_secs, config.focus_exit_on_first, cancel_fullscreen_focus);
      BOOST_LOG(info) << (focused ? "Fullscreen focus applied" : "Fullscreen focus not confirmed");

      int fullscreen_successes_left = std::max(0, config.focus_attempts);
      bool fullscreen_focus_budget_active = fullscreen_successes_left > 0 && config.focus_timeout_secs > 0;
      auto fullscreen_focus_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(0, config.focus_timeout_secs));
      auto next_fullscreen_focus_check = std::chrono::steady_clock::now();

      int game_successes_left = 0;
      bool game_focus_budget_active = false;
      auto game_focus_deadline = std::chrono::steady_clock::now();
      auto next_game_focus_check = std::chrono::steady_clock::now();
      bool fullscreen_detected = false;

      int consecutive_missing = 0;
      auto connection_grace_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(30, config.timeout_sec));

      auto send_stop_command_if_needed = [&]() {
        if (!client.is_active()) {
          return;
        }
        std::string id_to_stop;
        {
          std::lock_guard<std::mutex> lk(game_mutex);
          id_to_stop = game_state.id_original;
        }
        if (id_to_stop.empty()) {
          return;
        }
        try {
          nlohmann::json stop;
          stop["type"] = "command";
          stop["command"] = "stop";
          stop["id"] = id_to_stop;
          client.send_json_line(stop.dump());
          BOOST_LOG(info) << "Fullscreen mode: stop command sent for id=" << id_to_stop;
        } catch (...) {
          BOOST_LOG(warning) << "Fullscreen mode: failed to send stop command";
        }
      };

      while (true) {
        bool fs_running = false;
        std::vector<DWORD> fs_pids;
        try {
          fs_pids = platf::dxgi::find_process_ids_by_name(L"Playnite.FullscreenApp.exe");
          fs_running = !fs_pids.empty();
        } catch (...) {
        }

        auto now = std::chrono::steady_clock::now();
        bool active_game_now = active_game_flag.load();

        if(!fullscreen_detected && fs_running) {
          fullscreen_detected = true;
        }

        if (fs_running || active_game_now) {
          consecutive_missing = 0;
        }
        else {
          consecutive_missing++;
        }

        // If fullscreen not running and we've detected it once before, exit
        if (consecutive_missing >= 12 || (!fs_running && fullscreen_detected)) {
          break;
        }

        if (!watcher_spawned.load() && !fullscreen_install_dir_utf8.empty()) {
          bool expected = false;
          if (watcher_spawned.compare_exchange_strong(expected, true)) {
            if (!playnite::spawn_cleanup_watchdog_process(selfPath, fullscreen_install_dir_utf8, config.exit_timeout_secs, true, launcher_pid)) {
              watcher_spawned.store(false);
            }
          }
        }

        if (cleanup_spawn_signal.exchange(false)) {
          std::string dir;
          {
            std::lock_guard<std::mutex> lk(game_mutex);
            dir = game_state.cleanup_dir;
          }
          spawn_game_cleanup(dir);
        }

        if (game_start_signal.exchange(false)) {
          game_successes_left = std::max(0, config.focus_attempts);
          game_focus_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(1, config.focus_timeout_secs));
          game_focus_budget_active = config.focus_attempts > 0 && config.focus_timeout_secs > 0;
          next_game_focus_check = std::chrono::steady_clock::now();
          fullscreen_focus_budget_active = false;
          fullscreen_successes_left = std::max(0, config.focus_attempts);
        }

        if (game_stop_signal.exchange(false)) {
          game_focus_budget_active = false;
          game_successes_left = 0;
          if (config.focus_attempts > 0 && config.focus_timeout_secs > 0) {
            fullscreen_focus_budget_active = true;
            fullscreen_focus_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(1, config.focus_timeout_secs));
            next_fullscreen_focus_check = std::chrono::steady_clock::now();
          }
        }

        if (active_game_now && game_focus_budget_active) {
          auto now_focus = std::chrono::steady_clock::now();
          if (now_focus >= next_game_focus_check) {
            int remaining_secs = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(game_focus_deadline - now_focus).count());
            if (remaining_secs <= 0) {
              game_focus_budget_active = false;
            } else {
              std::string install_dir;
              std::string exe_path;
              {
                std::lock_guard<std::mutex> lk(game_mutex);
                install_dir = game_state.install_dir;
                exe_path = game_state.exe_path;
              }
              bool applied = false;
              auto cancel = [&]() {
                return !active_game_flag.load();
              };
              int slice = std::clamp(remaining_secs, 1, 3);
              if (!install_dir.empty()) {
                try {
                  std::wstring wdir = platf::dxgi::utf8_to_wide(install_dir);
                  applied = focus::focus_by_install_dir_extended(wdir, 1, slice, true, cancel);
                } catch (...) {
                }
              }
              if (!applied && !exe_path.empty()) {
                try {
                  std::wstring wexe = platf::dxgi::utf8_to_wide(exe_path);
                  std::filesystem::path p(wexe);
                  std::wstring base = p.filename().wstring();
                  if (!base.empty()) {
                    applied = focus::focus_process_by_name_extended(base.c_str(), 1, slice, true, cancel);
                  }
                } catch (...) {
                }
              }
              if (applied) {
                if (game_successes_left > 0) {
                  game_successes_left--;
                }
                if (game_successes_left <= 0) {
                  game_focus_budget_active = false;
                }
              } else if (std::chrono::steady_clock::now() >= game_focus_deadline) {
                game_focus_budget_active = false;
              }
            }
            next_game_focus_check = std::chrono::steady_clock::now() + 1s;
          }
        }

        if (!active_game_now && fullscreen_focus_budget_active) {
          auto now_focus = std::chrono::steady_clock::now();
          if (now_focus >= next_fullscreen_focus_check) {
            bool already_fg = false;
            for (auto pid : fs_pids) {
              if (focus::confirm_foreground_pid(pid)) {
                already_fg = true;
                break;
              }
            }
            if (!already_fg) {
              int remaining_secs = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(fullscreen_focus_deadline - now_focus).count());
              if (remaining_secs <= 0) {
                fullscreen_focus_budget_active = false;
              } else if (fullscreen_successes_left > 0) {
                bool ok = focus::focus_process_by_name_extended(L"Playnite.FullscreenApp.exe", 1, std::min(2, remaining_secs), true);
                if (ok) {
                  fullscreen_successes_left--;
                }
                if (fullscreen_successes_left <= 0 || std::chrono::steady_clock::now() >= fullscreen_focus_deadline) {
                  fullscreen_focus_budget_active = false;
                }
              } else {
                fullscreen_focus_budget_active = false;
              }
            }
            next_fullscreen_focus_check = now_focus + 2s;
          }
        }

        std::this_thread::sleep_for(500ms);
      }

      if (game_started_once) {
        send_stop_command_if_needed();
      }

      {
        std::string final_cleanup_dir;
        {
          std::lock_guard<std::mutex> lk(game_mutex);
          final_cleanup_dir = game_state.cleanup_dir;
        }
        if (!final_cleanup_dir.empty()) {
          spawn_game_cleanup(final_cleanup_dir);
        }
      }

      client.stop();
      if (fullscreen_lossless_applied) {
        auto runtime = lossless::capture_lossless_scaling_state();
        if (!runtime.running_pids.empty()) {
          lossless::lossless_scaling_stop_processes(runtime);
        }
        bool restored = lossless::lossless_scaling_restore_global_profile(fullscreen_lossless_backup);
      }
      return 0;
    }

    int run_standard_mode(LauncherConfig config, const lossless::lossless_scaling_options &lossless_options) {
      BOOST_LOG(info) << "Launcher mode: preparing IPC connection to Playnite plugin";

      platf::playnite::IpcClient client;

      std::atomic<bool> should_exit {false};
      std::atomic<bool> got_started {false};
      std::atomic<bool> request_game_focus {false};
      std::atomic<bool> game_focus_confirmed {false};
      std::atomic<int64_t> focus_retry_deadline_ms {0};
      std::atomic<int64_t> next_focus_attempt_ms {std::numeric_limits<int64_t>::min()};
      std::string last_install_dir;
      std::string last_game_exe;
      bool focus_exit_on_first_flag = config.focus_exit_on_first;
      int exit_timeout_secs = config.exit_timeout_secs;

      lossless::lossless_scaling_profile_backup active_lossless_backup {};
      bool lossless_profiles_applied = false;

      std::atomic<bool> watcher_spawned {false};

      auto schedule_focus_retry = [&]() {
        if (game_focus_confirmed.load(std::memory_order_acquire)) {
          return;
        }
        if (config.focus_attempts <= 0 || config.focus_timeout_secs <= 0) {
          return;
        }
        auto now = std::chrono::steady_clock::now();
        auto deadline = now + std::chrono::seconds(std::max(1, config.focus_timeout_secs));
        focus_retry_deadline_ms.store(steady_to_millis(deadline), std::memory_order_relaxed);
        next_focus_attempt_ms.store(std::numeric_limits<int64_t>::min(), std::memory_order_relaxed);
        request_game_focus.store(true, std::memory_order_release);
      };

      client.set_message_handler([&](std::span<const uint8_t> bytes) {
        auto msg = platf::playnite::parse(bytes);
        using MT = platf::playnite::MessageType;
        if (msg.type != MT::Status) {
          return;
        }
        auto norm = [&](std::string s) {
          return normalize_game_id(std::move(s));
        };
        if (!msg.status_game_id.empty() && norm(msg.status_game_id) == norm(config.game_id)) {
          if (!msg.status_install_dir.empty()) {
            bool changed = last_install_dir != msg.status_install_dir;
            last_install_dir = msg.status_install_dir;
            if (!watcher_spawned.load()) {
              bool expected = false;
              if (watcher_spawned.compare_exchange_strong(expected, true)) {
                WCHAR selfPath[MAX_PATH] = {};
                GetModuleFileNameW(nullptr, selfPath, ARRAYSIZE(selfPath));
                if (!playnite::spawn_cleanup_watchdog_process(selfPath, last_install_dir, exit_timeout_secs, false, GetCurrentProcessId())) {
                  watcher_spawned.store(false);
                }
              }
            }
            if (changed) {
              schedule_focus_retry();
            }
          }
          if (!msg.status_exe.empty()) {
            bool changed = last_game_exe != msg.status_exe;
            last_game_exe = msg.status_exe;
            if (changed) {
              schedule_focus_retry();
            }
          }
          if (msg.status_name == "gameStarted") {
            got_started.store(true);
            schedule_focus_retry();
            // Wait for user to unlock if they launched the game while locked
            bool was_locked = false;
            while (platf::dxgi::is_secure_desktop_active()) {
              if (!was_locked) {
                BOOST_LOG(info) << "Secure desktop detected (user locked screen). Waiting for unlock before applying Lossless Scaling and autofocus...";
                was_locked = true;
              }
              if (should_exit.load()) {
                BOOST_LOG(info) << "Exit requested while waiting for unlock";
                return;
              }
              std::this_thread::sleep_for(500ms);
            }
            if (was_locked) {
              BOOST_LOG(info) << "User unlocked. Proceeding with Lossless Scaling and autofocus.";
            }
            if (lossless_options.enabled && !lossless_profiles_applied) {
              auto runtime = lossless::capture_lossless_scaling_state();
              if (!runtime.running_pids.empty()) {
                lossless::lossless_scaling_stop_processes(runtime);
              }
              lossless::lossless_scaling_profile_backup backup;
              bool changed = lossless::lossless_scaling_apply_global_profile(lossless_options, last_install_dir, last_game_exe, backup);
              if (backup.valid) {
                active_lossless_backup = backup;
                lossless_profiles_applied = true;
              } else {
                active_lossless_backup = {};
              }
              lossless::lossless_scaling_restart_foreground(runtime, changed, last_install_dir, last_game_exe);
            }
          }
          if (msg.status_name == "gameStopped") {
            should_exit.store(true);
            request_game_focus.store(false, std::memory_order_release);
            game_focus_confirmed.store(false, std::memory_order_release);
            focus_retry_deadline_ms.store(0, std::memory_order_relaxed);
            next_focus_attempt_ms.store(std::numeric_limits<int64_t>::min(), std::memory_order_relaxed);
            if (lossless_profiles_applied) {
              auto runtime = lossless::capture_lossless_scaling_state();
              if (!runtime.running_pids.empty()) {
                lossless::lossless_scaling_stop_processes(runtime);
              }
              bool restored = lossless::lossless_scaling_restore_global_profile(active_lossless_backup);
              active_lossless_backup = {};
              lossless_profiles_applied = false;
            }
          }
        }
      });

      client.set_connected_handler([&]() {
        try {
          nlohmann::json hello;
          hello["type"] = "hello";
          hello["role"] = "launcher";
          hello["pid"] = static_cast<uint32_t>(GetCurrentProcessId());
          hello["mode"] = "standard";
          if (!config.game_id.empty()) {
            hello["gameId"] = config.game_id;
          }
          client.send_json_line(hello.dump());
        } catch (...) {}
      });

      client.start();

      if (!config.game_id.empty()) {
        ensure_playnite_open();
      }

      auto start_deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
      while (!client.is_active() && std::chrono::steady_clock::now() < start_deadline) {
        std::this_thread::sleep_for(50ms);
      }
      if (!client.is_active()) {
        BOOST_LOG(error) << "IPC did not become active; exiting";
        client.stop();
        return 3;
      }

      nlohmann::json j;
      j["type"] = "command";
      j["command"] = "launch";
      j["id"] = config.game_id;
      client.send_json_line(j.dump());
      BOOST_LOG(info) << "Launch command sent for id=" << config.game_id;

      // Wait for user to unlock if they launched the game while locked
      bool was_initially_locked = false;
      while (platf::dxgi::is_secure_desktop_active()) {
        if (!was_initially_locked) {
          BOOST_LOG(info) << "Secure desktop detected at launch (user locked screen). Waiting for unlock before proceeding...";
          was_initially_locked = true;
        }
        if (should_exit.load()) {
          BOOST_LOG(info) << "Exit requested while waiting for unlock at launch";
          client.stop();
          return 0;
        }
        std::this_thread::sleep_for(500ms);
      }
      if (was_initially_locked) {
        BOOST_LOG(info) << "User unlocked. Proceeding with game launch.";
      }

      if (config.focus_attempts > 0 && config.focus_timeout_secs > 0) {
        auto start_wait = std::chrono::steady_clock::now() + 5s;
        while (!got_started.load() && std::chrono::steady_clock::now() < start_wait) {
          std::this_thread::sleep_for(200ms);
        }
        bool focused = false;
        auto overall_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(1, config.focus_timeout_secs));
        if (!focused && !last_install_dir.empty()) {
          try {
            std::wstring wdir = platf::dxgi::utf8_to_wide(last_install_dir);
            BOOST_LOG(info) << "Autofocus: trying installDir=" << last_install_dir;
            int remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(overall_deadline - std::chrono::steady_clock::now()).count());
            if (remaining > 0) {
              focused = focus::focus_by_install_dir_extended(wdir, config.focus_attempts, remaining, focus_exit_on_first_flag, [&]() {
                return should_exit.load();
              });
            }
          } catch (...) {
          }
        }
        if (!focused && !last_game_exe.empty()) {
          try {
            std::wstring wexe = platf::dxgi::utf8_to_wide(last_game_exe);
            std::filesystem::path p = wexe;
            std::wstring base = p.filename().wstring();
            if (!base.empty()) {
              int remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(overall_deadline - std::chrono::steady_clock::now()).count());
              if (remaining > 0) {
                focused = focus::focus_process_by_name_extended(base.c_str(), config.focus_attempts, remaining, focus_exit_on_first_flag, [&]() {
                  return should_exit.load();
                });
              }
            }
          } catch (...) {
          }
        }
        if (!focused && !got_started.load() && !game_focus_confirmed.load(std::memory_order_acquire)) {
          int remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(overall_deadline - std::chrono::steady_clock::now()).count());
          if (remaining > 0) {
            focused = focus::focus_process_by_name_extended(L"Playnite.DesktopApp.exe", config.focus_attempts, remaining, focus_exit_on_first_flag, [&]() {
              return should_exit.load() || got_started.load();
            });
          }
        }
        if (!focused && !got_started.load() && !game_focus_confirmed.load(std::memory_order_acquire)) {
          int remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(overall_deadline - std::chrono::steady_clock::now()).count());
          if (remaining > 0) {
            focused = focus::focus_process_by_name_extended(L"Playnite.FullscreenApp.exe", config.focus_attempts, remaining, focus_exit_on_first_flag, [&]() {
              return should_exit.load() || got_started.load();
            });
          }
        }
        BOOST_LOG(info) << (focused ? "Applied focus after launch" : "Focus not applied after launch");
      }

      auto focus_game_after_start = [&]() {
        if (!request_game_focus.load(std::memory_order_acquire)) {
          return;
        }
        if (config.focus_attempts <= 0 || config.focus_timeout_secs <= 0) {
          request_game_focus.store(false, std::memory_order_release);
          return;
        }
        auto now = std::chrono::steady_clock::now();
        if (game_focus_confirmed.load(std::memory_order_acquire)) {
          request_game_focus.store(false, std::memory_order_release);
          return;
        }
        int64_t deadline_ms = focus_retry_deadline_ms.load(std::memory_order_relaxed);
        if (deadline_ms != 0) {
          auto deadline = millis_to_steady(deadline_ms);
          if (now >= deadline) {
            request_game_focus.store(false, std::memory_order_release);
            return;
          }
        }
        int64_t next_ms = next_focus_attempt_ms.load(std::memory_order_relaxed);
        if (next_ms != std::numeric_limits<int64_t>::min()) {
          auto next_time = millis_to_steady(next_ms);
          if (now < next_time) {
            return;
          }
        }
        bool expected = true;
        if (!request_game_focus.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
          return;
        }
        int remaining = config.focus_timeout_secs;
        if (deadline_ms != 0) {
          auto deadline = millis_to_steady(deadline_ms);
          remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(deadline - now).count());
        }
        remaining = std::max(1, remaining);
        bool focused = false;
        auto cancel = [&]() {
          return should_exit.load();
        };
        if (!focused && !last_install_dir.empty()) {
          try {
            std::wstring wdir = platf::dxgi::utf8_to_wide(last_install_dir);
            focused = focus::focus_by_install_dir_extended(wdir, config.focus_attempts, remaining, focus_exit_on_first_flag, cancel);
          } catch (...) {
          }
        }
        if (!focused && !last_game_exe.empty()) {
          try {
            std::wstring wexe = platf::dxgi::utf8_to_wide(last_game_exe);
            std::filesystem::path p = wexe;
            std::wstring base = p.filename().wstring();
            if (!base.empty()) {
              focused = focus::focus_process_by_name_extended(base.c_str(), config.focus_attempts, remaining, focus_exit_on_first_flag, cancel);
            }
          } catch (...) {
          }
        }
        BOOST_LOG(info) << (focused ? "Autofocus: foreground confirmed for launched game" : "Autofocus: unable to confirm game foreground after start");
        if (focused) {
          game_focus_confirmed.store(true, std::memory_order_release);
          next_focus_attempt_ms.store(std::numeric_limits<int64_t>::min(), std::memory_order_relaxed);
          focus_retry_deadline_ms.store(0, std::memory_order_relaxed);
        } else {
          auto retry_at = now + 750ms;
          next_focus_attempt_ms.store(steady_to_millis(retry_at), std::memory_order_relaxed);
          if (deadline_ms == 0 || retry_at < millis_to_steady(deadline_ms)) {
            request_game_focus.store(true, std::memory_order_release);
          }
        }
      };

      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(config.timeout_sec);
      while (!should_exit.load()) {
        focus_game_after_start();
        if (!got_started.load() && std::chrono::steady_clock::now() >= deadline) {
          break;
        }
        if (got_started.load()) {
          auto d = platf::dxgi::find_process_ids_by_name(L"Playnite.DesktopApp.exe");
          auto f = platf::dxgi::find_process_ids_by_name(L"Playnite.FullscreenApp.exe");
          if (d.empty() && f.empty()) {
            BOOST_LOG(warning) << "Playnite process appears to have exited; proceeding to cleanup";
            should_exit.store(true);
            break;
          }
        }
        std::this_thread::sleep_for(250ms);
      }

      if (!should_exit.load()) {
        BOOST_LOG(warning) << (got_started.load() ? "Timeout after start unexpectedly; exiting" : "Timeout waiting for game start; exiting");
      }

      BOOST_LOG(info) << "Playnite reported gameStopped or timeout; scheduling cleanup and exiting";
      if (!last_install_dir.empty()) {
        WCHAR selfPath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, selfPath, ARRAYSIZE(selfPath));
        static_cast<void>(playnite::spawn_cleanup_watchdog_process(selfPath, last_install_dir, exit_timeout_secs, false, std::nullopt));
      }
      if (lossless_profiles_applied) {
        auto runtime = lossless::capture_lossless_scaling_state();
        if (!runtime.running_pids.empty()) {
          lossless::lossless_scaling_stop_processes(runtime);
        }
        bool restored = lossless::lossless_scaling_restore_global_profile(active_lossless_backup);
      }
      int exit_code = should_exit.load() ? 0 : 4;
      client.stop();
      return exit_code;
    }

  }  // namespace detail
}  // namespace playnite_launcher
