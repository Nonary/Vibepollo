#include "tools/playnite_launcher/cleanup.h"

#include "src/logging.h"
#include "src/platform/windows/ipc/misc_utils.h"
#include "tools/playnite_launcher/focus_utils.h"
#include "tools/playnite_launcher/playnite_process.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <TlHelp32.h>
#include <vector>
#include <windows.h>

using namespace std::chrono_literals;

namespace playnite_launcher::cleanup {
  namespace {

    using WindowFn = std::function<void(HWND)>;

    void send_message_timeout(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
      SendMessageTimeoutW(hwnd, msg, wParam, lParam, SMTO_ABORTIFHUNG, 5000, nullptr);
    }

    void enumerate_top_windows(DWORD pid, const WindowFn &fn) {
      struct Ctx {
        DWORD target;
        const WindowFn *fn;
      } ctx {pid, &fn};

      auto proc = [](HWND hwnd, LPARAM param) -> BOOL {
        auto *ctx = reinterpret_cast<Ctx *>(param);
        DWORD owner = 0;
        GetWindowThreadProcessId(hwnd, &owner);
        if (owner == ctx->target) {
          (*ctx->fn)(hwnd);
        }
        return TRUE;
      };
      EnumWindows(proc, reinterpret_cast<LPARAM>(&ctx));
    }

    HANDLE create_thread_snapshot() {
      HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      return snap == INVALID_HANDLE_VALUE ? nullptr : snap;
    }

    void enumerate_pid_threads(DWORD pid, const std::function<void(DWORD)> &fn) {
      HANDLE snap = create_thread_snapshot();
      if (!snap) {
        return;
      }
      THREADENTRY32 te {sizeof(te)};
      if (!Thread32First(snap, &te)) {
        CloseHandle(snap);
        return;
      }
      do {
        if (te.th32OwnerProcessID == pid) {
          fn(te.th32ThreadID);
        }
      } while (Thread32Next(snap, &te));
      CloseHandle(snap);
    }

    void enumerate_thread_windows(DWORD thread_id, const WindowFn &fn) {
      auto proc = [](HWND hwnd, LPARAM param) -> BOOL {
        auto *f = reinterpret_cast<const WindowFn *>(param);
        (*f)(hwnd);
        return TRUE;
      };
      EnumThreadWindows(thread_id, proc, reinterpret_cast<LPARAM>(&fn));
    }

    void for_each_thread_window(DWORD pid, const WindowFn &fn) {
      enumerate_pid_threads(pid, [&](DWORD thread_id) {
        enumerate_thread_windows(thread_id, fn);
      });
    }

    void post_close_messages(DWORD pid) {
      enumerate_top_windows(pid, [&](HWND hwnd) {
        send_message_timeout(hwnd, WM_CLOSE, 0, 0);
      });
    }

    void post_logoff_messages(DWORD pid) {
      enumerate_top_windows(pid, [&](HWND hwnd) {
        send_message_timeout(hwnd, WM_QUERYENDSESSION, TRUE, ENDSESSION_CLOSEAPP);
      });
      enumerate_top_windows(pid, [&](HWND hwnd) {
        send_message_timeout(hwnd, WM_ENDSESSION, TRUE, 0);
      });
    }

    void post_quit_messages(DWORD pid) {
      for_each_thread_window(pid, [&](HWND hwnd) {
        PostMessageW(hwnd, WM_QUIT, 0, 0);
      });
    }

    void signal_console_threads(DWORD pid) {
      enumerate_pid_threads(pid, [&](DWORD thread_id) {
        HANDLE thread = OpenThread(THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, thread_id);
        if (thread) {
          GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, thread_id);
          CloseHandle(thread);
        }
      });
    }

    bool ensure_window_minimized(HWND hwnd, std::chrono::milliseconds timeout) {
      if (!hwnd) {
        return false;
      }
      auto deadline = std::chrono::steady_clock::now() + timeout;
      while (std::chrono::steady_clock::now() < deadline) {
        send_message_timeout(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
        ShowWindow(hwnd, SW_RESTORE);
        send_message_timeout(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
        ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
        if (IsIconic(hwnd)) {
          return true;
        }
        std::this_thread::sleep_for(100ms);
      }
      return IsIconic(hwnd) != FALSE;
    }

    struct CleanupPlan {
      std::wstring install_dir;
      int timeout_secs;
      std::chrono::steady_clock::time_point start;
      bool sent_close = false;
      bool sent_logoff = false;
      bool sent_quit = false;
      bool logged_initial = false;
    };

    CleanupPlan make_plan(const std::wstring &install_dir, int timeout_secs) {
      CleanupPlan plan;
      plan.install_dir = install_dir;
      plan.timeout_secs = std::max(1, timeout_secs);
      plan.start = std::chrono::steady_clock::now();
      return plan;
    }

    std::vector<DWORD> collect_candidates(const std::wstring &install_dir) {
      auto pids = focus::find_pids_under_install_dir_sorted(install_dir);
      if (pids.empty()) {
        pids = focus::find_pids_under_install_dir_sorted(install_dir, false);
      }
      return pids;
    }

    double elapsed_fraction(const CleanupPlan &plan) {
      auto now = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - plan.start).count();
      double total = static_cast<double>(plan.timeout_secs) * 1000.0;
      return std::clamp(elapsed / total, 0.0, 1.0);
    }

    void log_initial_once(CleanupPlan &plan, const std::vector<DWORD> &pids) {
      if (plan.logged_initial) {
        return;
      }
      plan.logged_initial = true;
      BOOST_LOG(info) << "Cleanup: initial candidates count=" << pids.size();
      for (auto pid : pids) {
        std::wstring img;
        focus::get_process_image_path(pid, img);
        BOOST_LOG(info) << "Cleanup: candidate PID=" << pid << " path='" << platf::dxgi::wide_to_utf8(img) << "'";
      }
    }

    void apply_stages(CleanupPlan &plan, const std::vector<DWORD> &pids) {
      double fraction = elapsed_fraction(plan);
      if (!plan.sent_close) {
        for (auto pid : pids) {
          post_close_messages(pid);
        }
        plan.sent_close = true;
        return;
      }
      if (fraction >= 0.4 && !plan.sent_logoff) {
        for (auto pid : pids) {
          post_logoff_messages(pid);
        }
        plan.sent_logoff = true;
        return;
      }
      if (fraction >= 0.7 && !plan.sent_quit) {
        for (auto pid : pids) {
          post_quit_messages(pid);
          signal_console_threads(pid);
        }
        plan.sent_quit = true;
      }
    }

    void force_terminate(const std::wstring &install_dir) {
      auto remaining = collect_candidates(install_dir);
      for (auto pid : remaining) {
        std::wstring img;
        focus::get_process_image_path(pid, img);
        BOOST_LOG(warning) << "Cleanup: forcing termination of PID=" << pid
                           << (img.empty() ? "" : (" path=" + platf::dxgi::wide_to_utf8(img)));
        focus::terminate_pid(pid);
      }
    }

    bool launch_desktop_command(const std::wstring &path, DWORD flags) {
      std::wstring cmd =
        L""
        " + path + L"
        " --startdesktop";
      std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
      cmdline.push_back(0);
      STARTUPINFOW si {};
      si.cb = sizeof(si);
      si.dwFlags |= STARTF_USESHOWWINDOW;
      si.wShowWindow = SW_HIDE;
      PROCESS_INFORMATION pi {};
      BOOL ok = CreateProcessW(path.c_str(), cmdline.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si, &pi);
      if (pi.hThread) {
        CloseHandle(pi.hThread);
      }
      if (pi.hProcess) {
        CloseHandle(pi.hProcess);
      }
      return ok;
    }

    std::wstring resolve_desktop_path() {
      try {
        std::wstring assoc = playnite::query_playnite_executable_from_assoc();
        if (assoc.empty()) {
          return {};
        }
        std::filesystem::path base(assoc);
        if (_wcsicmp(base.filename().c_str(), L"Playnite.DesktopApp.exe") == 0) {
          return assoc;
        }
        std::filesystem::path candidate = base.parent_path() / L"Playnite.DesktopApp.exe";
        if (std::filesystem::exists(candidate)) {
          return candidate.wstring();
        }
        return assoc;
      } catch (...) {
        return {};
      }
    }

    bool launch_desktop_app(const std::wstring &path) {
      if (path.empty() || !std::filesystem::exists(path)) {
        BOOST_LOG(warning) << "Cleanup fullscreen: unable to resolve Playnite.DesktopApp path";
        return false;
      }
      DWORD base_flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW | DETACHED_PROCESS;
      bool ok = launch_desktop_command(path, base_flags | CREATE_BREAKAWAY_FROM_JOB);
      if (!ok) {
        ok = launch_desktop_command(path, base_flags);
      }
      BOOST_LOG(info) << "Cleanup fullscreen: launch DesktopApp attempt result=" << (ok ? "ok" : "fail");
      return ok;
    }

    std::vector<DWORD> wait_for_process(const wchar_t *exe, std::chrono::steady_clock::time_point deadline, std::chrono::milliseconds step) {
      std::vector<DWORD> ids;
      while (std::chrono::steady_clock::now() < deadline) {
        try {
          ids = platf::dxgi::find_process_ids_by_name(exe);
        } catch (...) {
          ids.clear();
        }
        if (!ids.empty()) {
          break;
        }
        std::this_thread::sleep_for(step);
      }
      return ids;
    }

    bool minimize_desktop_once(const std::vector<DWORD> &pids) {
      auto deadline = std::chrono::steady_clock::now() + 30s;
      while (std::chrono::steady_clock::now() < deadline) {
        for (auto pid : pids) {
          HWND hwnd = focus::find_main_window_for_pid(pid);
          if (!hwnd) {
            continue;
          }
          if (!IsWindow(hwnd)) {
            return true;
          }
          if (IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
            if (!ensure_window_minimized(hwnd, 5s)) {
              BOOST_LOG(warning) << "Cleanup fullscreen: failed to confirm DesktopApp minimized";
            }
            return true;
          }
        }
        std::this_thread::sleep_for(300ms);
      }
      BOOST_LOG(info) << "Cleanup fullscreen: DesktopApp window never reported visible before timeout";
      return false;
    }

    bool wait_for_fullscreen_exit(int exit_timeout_secs) {
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(3, exit_timeout_secs));
      while (std::chrono::steady_clock::now() < deadline) {
        try {
          auto ids = platf::dxgi::find_process_ids_by_name(L"Playnite.FullscreenApp.exe");
          if (ids.empty()) {
            return true;
          }
        } catch (...) {
        }
        std::this_thread::sleep_for(250ms);
      }
      return false;
    }

  }  // namespace

  void cleanup_graceful_then_forceful_in_dir(const std::wstring &install_dir, int exit_timeout_secs) {
    if (install_dir.empty()) {
      return;
    }
    BOOST_LOG(info) << "Cleanup: begin for install_dir='" << platf::dxgi::wide_to_utf8(install_dir)
                    << "' timeout=" << exit_timeout_secs << "s";
    CleanupPlan plan = make_plan(install_dir, exit_timeout_secs);
    while (true) {
      auto pids = collect_candidates(plan.install_dir);
      log_initial_once(plan, pids);
      if (pids.empty()) {
        BOOST_LOG(info) << "Cleanup: all processes exited gracefully";
        return;
      }
      apply_stages(plan, pids);
      if (elapsed_fraction(plan) >= 1.0) {
        break;
      }
      std::this_thread::sleep_for(250ms);
    }
    force_terminate(plan.install_dir);
  }

  void cleanup_fullscreen_via_desktop(int exit_timeout_secs) {
    BOOST_LOG(info) << "Cleanup fullscreen: launching DesktopApp to close fullscreen";
    std::wstring desktop_path = resolve_desktop_path();
    launch_desktop_app(desktop_path);
    auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(3, exit_timeout_secs));
    auto desktop_pids = wait_for_process(L"Playnite.DesktopApp.exe", wait_deadline, 200ms);
    if (!desktop_pids.empty()) {
      minimize_desktop_once(desktop_pids);
    }
    if (wait_for_fullscreen_exit(exit_timeout_secs)) {
      BOOST_LOG(info) << "Cleanup fullscreen: FullscreenApp exited after desktop launch";
    }
  }

}  // namespace playnite_launcher::cleanup
