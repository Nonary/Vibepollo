/**
 * @file tools/playnite_launcher.cpp
 * @brief Standalone Playnite launcher helper. Hosts a per-launch IPC server with a
 *        GUID-derived control pipe name and commands Playnite to start a specific game.
 *
 * Usage:
 *   playnite-launcher --game-id <GUID> [--public-guid <GUID>] [--timeout <seconds>]
 *   playnite-launcher --fullscreen [--focus-attempts N] [--focus-timeout S] [--focus-exit-on-first]
 *
 * Behavior:
 *   - Initializes logging to sunshine_playnite_launcher.log in appdata
 *   - Chooses a public GUID (either provided or generated) and hosts a control pipe named
 *     "sunshine_playnite_ctl_<GUID>". The Playnite plugin connects to this, performs our
 *     anonymous handshake, then switches to a private data pipe.
 *   - Once the data pipe is active, sends a launch command for the requested Playnite game id.
 *   - Remains alive, listening for status messages, and exits when it receives
 *     status.gameStopped for the same game id (or on timeout).
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "src/logging.h"
#include "src/platform/windows/ipc/misc_utils.h"
#include "src/platform/windows/playnite_ipc.h"
#include "src/platform/windows/playnite_protocol.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <Shlwapi.h>
#include <string>
#include <string_view>
#include <thread>
#include <tlhelp32.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <windows.h>

#ifndef PROC_THREAD_ATTRIBUTE_PARENT_PROCESS
#define PROC_THREAD_ATTRIBUTE_PARENT_PROCESS ProcThreadAttributeValue(0, FALSE, TRUE, FALSE)
#endif

using namespace std::chrono_literals;

// Fallback declaration for CommandLineToArgvW if headers don't provide it
#ifdef _WIN32
#ifndef HAVE_CommandLineToArgvW_DECL
extern "C" __declspec(dllimport) LPWSTR *WINAPI CommandLineToArgvW(LPCWSTR lpCmdLine, int *pNumArgs);
#endif
#endif

namespace {

  // Returns true if either Playnite Desktop or Fullscreen is running
  static bool is_playnite_running() {
    try {
      auto d = platf::dxgi::find_process_ids_by_name(L"Playnite.DesktopApp.exe");
      if (!d.empty()) {
        return true;
      }
      auto f = platf::dxgi::find_process_ids_by_name(L"Playnite.FullscreenApp.exe");
      if (!f.empty()) {
        return true;
      }
    } catch (...) {}
    return false;
  }

  static std::wstring get_explorer_path() {
    // Prefer %WINDIR%\\explorer.exe
    WCHAR winDir[MAX_PATH] = {};
    if (GetWindowsDirectoryW(winDir, ARRAYSIZE(winDir)) > 0) {
      std::filesystem::path p(winDir);
      p /= L"explorer.exe";
      if (std::filesystem::exists(p)) {
        return p.wstring();
      }
    }
    // Fallback: SearchPathW
    WCHAR out[MAX_PATH] = {};
    DWORD rc = SearchPathW(nullptr, L"explorer.exe", nullptr, ARRAYSIZE(out), out, nullptr);
    if (rc > 0 && rc < ARRAYSIZE(out)) {
      return std::wstring(out);
    }
    return L"explorer.exe";  // last resort; CreateProcessW may still find it
  }

  static HANDLE open_explorer_parent_handle() {
    // Try shell window PID first (most reliable parent in current session)
    DWORD pid = 0;
    HWND shell = GetShellWindow();
    if (shell) {
      GetWindowThreadProcessId(shell, &pid);
    }
    if (!pid) {
      // Fallback: try Shell_TrayWnd (taskbar)
      HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
      if (tray) {
        GetWindowThreadProcessId(tray, &pid);
      }
    }
    // Fallback: find by name, prefer same session
    if (!pid) {
      DWORD curSession = 0;
      ProcessIdToSessionId(GetCurrentProcessId(), &curSession);
      auto pids = platf::dxgi::find_process_ids_by_name(L"explorer.exe");
      for (DWORD cand : pids) {
        DWORD sess = 0;
        ProcessIdToSessionId(cand, &sess);
        if (sess == curSession) {
          pid = cand;
          break;
        }
      }
      if (!pid && !pids.empty()) {
        pid = pids.front();
      }
    }
    if (!pid) {
      return nullptr;
    }

    // Required rights: PROCESS_CREATE_PROCESS for parent attribute
    HANDLE h = OpenProcess(PROCESS_CREATE_PROCESS | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_DUP_HANDLE, FALSE, pid);
    return h;  // may be null on failure
  }

  // Launch a URI by starting explorer.exe as a detached, breakaway, parented child
  static bool launch_uri_detached_parented(const std::wstring &uri) {
    auto parent = open_explorer_parent_handle();
    if (!parent) {
      BOOST_LOG(warning) << "Unable to open explorer.exe as parent; proceeding without parent override";
    }

    STARTUPINFOEXW si {};
    si.StartupInfo.cb = sizeof(si);
    LPPROC_THREAD_ATTRIBUTE_LIST attrList = nullptr;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, parent ? 1 : 0, 0, &size);
    if (parent) {
      attrList = (LPPROC_THREAD_ATTRIBUTE_LIST) HeapAlloc(GetProcessHeap(), 0, size);
      if (!attrList) {
        CloseHandle(parent);
        parent = nullptr;
      } else if (!InitializeProcThreadAttributeList(attrList, 1, 0, &size)) {
        HeapFree(GetProcessHeap(), 0, attrList);
        attrList = nullptr;
        CloseHandle(parent);
        parent = nullptr;
      }
      if (attrList && parent) {
        if (!UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, &parent, sizeof(parent), nullptr, nullptr)) {
          DeleteProcThreadAttributeList(attrList);
          HeapFree(GetProcessHeap(), 0, attrList);
          attrList = nullptr;
          CloseHandle(parent);
          parent = nullptr;
        } else {
          si.lpAttributeList = attrList;
        }
      }
    }

    std::wstring exe = get_explorer_path();
    std::wstring cmd = L"\"" + exe + L"\" " + uri;

    DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
                  CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW | DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB;
    PROCESS_INFORMATION pi {};
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si.StartupInfo, &pi);

    if (attrList) {
      DeleteProcThreadAttributeList(attrList);
      HeapFree(GetProcessHeap(), 0, attrList);
    }
    if (parent) {
      CloseHandle(parent);
    }
    if (ok) {
      if (pi.hThread) {
        CloseHandle(pi.hThread);
      }
      if (pi.hProcess) {
        CloseHandle(pi.hProcess);
      }
      return true;
    }
    auto winerr = GetLastError();
    BOOST_LOG(warning) << "CreateProcessW(explorer uri) failed: " << winerr;
    return false;
  }

  static std::wstring query_playnite_executable_from_assoc() {
    std::array<wchar_t, 4096> buf {};
    DWORD sz = static_cast<DWORD>(buf.size());
    std::wstring exe;
    HRESULT hr = AssocQueryStringW(ASSOCF_NOTRUNCATE, ASSOCSTR_EXECUTABLE, L"playnite", nullptr, buf.data(), &sz);
    if (hr == S_OK && buf[0] != L'\0') {
      exe.assign(buf.data());
      return exe;
    }
    // Fallback to COMMAND and parse out executable
    sz = static_cast<DWORD>(buf.size());
    hr = AssocQueryStringW(ASSOCF_NOTRUNCATE, ASSOCSTR_COMMAND, L"playnite", L"open", buf.data(), &sz);
    if (hr == S_OK && buf[0] != L'\0') {
      int argc = 0;
      auto argv = CommandLineToArgvW(buf.data(), &argc);
      if (argv && argc >= 1) {
        exe.assign(argv[0]);
        LocalFree(argv);
        return exe;
      }
      std::wstring s(buf.data());
      if (!s.empty() && s.front() == L'"') {
        auto p = s.find(L'"', 1);
        if (p != std::wstring::npos) {
          exe = s.substr(1, p - 1);
          return exe;
        }
      }
      auto p = s.find(L' ');
      exe = (p == std::wstring::npos) ? s : s.substr(0, p);
    }
    return exe;
  }

  static bool launch_executable_detached_parented(const std::wstring &exe_full_path) {
    auto parent = open_explorer_parent_handle();
    STARTUPINFOEXW si {};
    si.StartupInfo.cb = sizeof(si);
    LPPROC_THREAD_ATTRIBUTE_LIST attrList = nullptr;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, parent ? 1 : 0, 0, &size);
    if (parent) {
      attrList = (LPPROC_THREAD_ATTRIBUTE_LIST) HeapAlloc(GetProcessHeap(), 0, size);
      if (attrList && InitializeProcThreadAttributeList(attrList, 1, 0, &size)) {
        if (!UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, &parent, sizeof(parent), nullptr, nullptr)) {
          DeleteProcThreadAttributeList(attrList);
          HeapFree(GetProcessHeap(), 0, attrList);
          attrList = nullptr;
        } else {
          si.lpAttributeList = attrList;
        }
      }
    }

    std::wstring cmd = L"\"" + exe_full_path + L"\"";
    DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
                  CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW | DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB;
    PROCESS_INFORMATION pi {};
    BOOL ok = CreateProcessW(exe_full_path.c_str(), cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si.StartupInfo, &pi);

    if (attrList) {
      DeleteProcThreadAttributeList(attrList);
      HeapFree(GetProcessHeap(), 0, attrList);
    }
    if (parent) {
      CloseHandle(parent);
    }
    if (ok) {
      if (pi.hThread) {
        CloseHandle(pi.hThread);
      }
      if (pi.hProcess) {
        CloseHandle(pi.hProcess);
      }
      return true;
    }
    BOOST_LOG(warning) << "CreateProcessW(executable) failed: " << GetLastError();
    return false;
  }

  static bool spawn_cleanup_watchdog_process(const std::wstring &self_path, const std::string &install_dir_utf8, int exit_timeout_secs, bool fullscreen_flag, std::optional<DWORD> wait_for_pid) {
    try {
      std::wstring wcmd = L"\"" + self_path + L"\" --do-cleanup";
      if (!install_dir_utf8.empty()) {
        wcmd += L" --install-dir \"" + platf::dxgi::utf8_to_wide(install_dir_utf8) + L"\"";
      }
      if (exit_timeout_secs > 0) {
        wcmd += L" --exit-timeout " + std::to_wstring(exit_timeout_secs);
      }
      if (fullscreen_flag) {
        wcmd += L" --fullscreen";
      }
      if (wait_for_pid) {
        wcmd += L" --wait-for-pid " + std::to_wstring(*wait_for_pid);
      }

      BOOST_LOG(info) << "Spawning cleanup watcher (fullscreen=" << fullscreen_flag << ", installDir='" << install_dir_utf8 << "' waitPid="
                      << (wait_for_pid ? std::to_string(*wait_for_pid) : std::string("none")) << ")";

      STARTUPINFOW si {};
      si.cb = sizeof(si);
      si.dwFlags |= STARTF_USESHOWWINDOW;
      si.wShowWindow = SW_HIDE;
      PROCESS_INFORMATION pi {};
      std::wstring cmdline = wcmd;
      DWORD flags_base = CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW | DETACHED_PROCESS;
      DWORD flags_try = flags_base | CREATE_BREAKAWAY_FROM_JOB;
      BOOL ok = CreateProcessW(self_path.c_str(), cmdline.data(), nullptr, nullptr, FALSE, flags_try, nullptr, nullptr, &si, &pi);
      if (!ok) {
        ok = CreateProcessW(self_path.c_str(), cmdline.data(), nullptr, nullptr, FALSE, flags_base, nullptr, nullptr, &si, &pi);
      }
      if (!ok) {
        BOOST_LOG(warning) << "Cleanup watcher spawn failed (fullscreen=" << fullscreen_flag << ") error=" << GetLastError();
        return false;
      }
      BOOST_LOG(info) << "Cleanup watcher spawned (fullscreen=" << fullscreen_flag << ", pid=" << pi.dwProcessId << ")";
      if (pi.hThread) {
        CloseHandle(pi.hThread);
      }
      if (pi.hProcess) {
        CloseHandle(pi.hProcess);
      }
      return true;
    } catch (...) {
      BOOST_LOG(warning) << "Exception launching cleanup watcher";
      return false;
    }
  }

  bool parse_arg(std::span<char *> args, std::string_view name, std::string &out) {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
      if (name == args[i]) {
        out = args[i + 1];
        return true;
      }
      // Support --key=value
      if (std::string_view(args[i]).rfind(std::string(name) + "=", 0) == 0) {
        out = std::string(args[i] + name.size() + 1);
        return true;
      }
    }
    return false;
  }

  bool parse_flag(std::span<char *> args, std::string_view name) {
    for (size_t i = 0; i < args.size(); ++i) {
      if (name == args[i]) {
        return true;
      }
      if (std::string_view(args[i]) == (std::string(name) + "=true")) {
        return true;
      }
    }
    return false;
  }

  // Enumerate top-level windows and return the first HWND whose owning PID matches
  static HWND find_main_window_for_pid(DWORD pid) {
    struct Ctx {
      DWORD pid;
      HWND hwnd;
    } ctx {pid, nullptr};

    auto enum_proc = [](HWND hwnd, LPARAM lparam) -> BOOL {
      auto *c = reinterpret_cast<Ctx *>(lparam);
      DWORD wpid = 0;
      GetWindowThreadProcessId(hwnd, &wpid);
      if (wpid != c->pid) {
        return TRUE;
      }
      if (!IsWindowVisible(hwnd)) {
        return TRUE;
      }
      HWND owner = GetWindow(hwnd, GW_OWNER);
      if (owner != nullptr) {
        return TRUE;  // skip owned tool windows
      }
      c->hwnd = hwnd;
      return FALSE;
    };
    EnumWindows(enum_proc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.hwnd;
  }

  static bool try_focus_hwnd(HWND hwnd) {
    if (!hwnd) {
      return false;
    }
    // Try to restore and bring to foreground using a more robust approach
    HWND fg = GetForegroundWindow();
    DWORD fg_tid = 0;
    if (fg) {
      fg_tid = GetWindowThreadProcessId(fg, nullptr);
    }
    DWORD cur_tid = GetCurrentThreadId();

    // Attach input to the foreground thread to increase SetForegroundWindow success
    if (fg && fg_tid != 0 && fg_tid != cur_tid) {
      AttachThreadInput(cur_tid, fg_tid, TRUE);
    }

    // Restore if minimized and bring to top-most temporarily
    ShowWindow(hwnd, SW_RESTORE);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    BOOL ok = SetForegroundWindow(hwnd);
    // Undo top-most
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

    if (fg && fg_tid != 0 && fg_tid != cur_tid) {
      AttachThreadInput(cur_tid, fg_tid, FALSE);
    }
    return ok != FALSE;
  }

  // Enumerate all top-level windows belonging to PID and invoke cb(hwnd)
  template<typename F>
  static void for_each_top_level_window_of_pid(DWORD pid, F &&cb) {
    using Fn = std::decay_t<F>;
    Fn *fn = &cb;

    struct Ctx {
      DWORD pid;
      Fn *fn;
    } ctx {pid, fn};

    auto enum_proc = [](HWND hwnd, LPARAM lparam) -> BOOL {
      auto *c = reinterpret_cast<Ctx *>(lparam);
      DWORD wpid = 0;
      GetWindowThreadProcessId(hwnd, &wpid);
      if (wpid != c->pid) {
        return TRUE;
      }
      // Consider all top-level windows (owner may be null); don't require IsWindowVisible
      if (GetWindow(hwnd, GW_OWNER) != nullptr) {
        return TRUE;
      }
      (*c->fn)(hwnd);
      return TRUE;
    };
    EnumWindows(enum_proc, reinterpret_cast<LPARAM>(&ctx));
  }

  // Enumerate all thread windows of PID and invoke cb(hwnd)
  template<typename F>
  static void for_each_thread_window_of_pid(DWORD pid, F &&cb) {
    using Fn = std::decay_t<F>;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
      return;
    }
    THREADENTRY32 te {};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
      do {
        if (te.th32OwnerProcessID != pid) {
          continue;
        }
        EnumThreadWindows(te.th32ThreadID, [](HWND hwnd, LPARAM lp) -> BOOL {
          auto *f = reinterpret_cast<Fn *>(lp);
          (*f)(hwnd);
          return TRUE;
        },
                          reinterpret_cast<LPARAM>(&cb));
      } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
  }

  static void send_message_timeout(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DWORD_PTR result = 0;
    SendMessageTimeoutW(hwnd, msg, wParam, lParam, SMTO_ABORTIFHUNG | SMTO_NORMAL, 200, &result);
  }

  // Stage 1: Polite close (SC_CLOSE + WM_CLOSE) to all windows of PID
  static void stage_close_windows_for_pid(DWORD pid) {
    int top_count = 0, thread_count = 0;
    auto send = [&](HWND hwnd) {
      send_message_timeout(hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
      send_message_timeout(hwnd, WM_CLOSE, 0, 0);
      // We can't tell from here if this is top-level or thread window; counts are approximate
    };
    for_each_top_level_window_of_pid(pid, [&](HWND hwnd) {
      ++top_count;
      send(hwnd);
    });
    for_each_thread_window_of_pid(pid, [&](HWND hwnd) {
      ++thread_count;
      send(hwnd);
    });
    BOOST_LOG(info) << "Cleanup: stage1 sent SC_CLOSE/WM_CLOSE to PID=" << pid
                    << " topWindows=" << top_count << " threadWindows=" << thread_count;
  }

  // Stage 2: Logoff-style close (QUERYENDSESSION/ENDSESSION)
  static void stage_logoff_for_pid(DWORD pid) {
    int top_count = 0, thread_count = 0;
    auto send = [&](HWND hwnd) {
      send_message_timeout(hwnd, WM_QUERYENDSESSION, TRUE, 0);
      // Per request: follow with WM_ENDSESSION (FALSE) to hint a close without full logoff
      send_message_timeout(hwnd, WM_ENDSESSION, FALSE, 0);
    };
    for_each_top_level_window_of_pid(pid, [&](HWND hwnd) {
      ++top_count;
      send(hwnd);
    });
    for_each_thread_window_of_pid(pid, [&](HWND hwnd) {
      ++thread_count;
      send(hwnd);
    });
    BOOST_LOG(info) << "Cleanup: stage2 sent QUERY/ENDSESSION to PID=" << pid
                    << " topWindows=" << top_count << " threadWindows=" << thread_count;
  }

  // Stage 3: Post WM_QUIT to (approx) main thread and try console CTRL events
  static void stage_quit_thread_or_console(DWORD pid) {
    // Choose the smallest thread ID for the process as an approximation for the main thread
    DWORD main_tid = 0xFFFFFFFF;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
      THREADENTRY32 te {};
      te.dwSize = sizeof(te);
      if (Thread32First(snap, &te)) {
        do {
          if (te.th32OwnerProcessID != pid) {
            continue;
          }
          if (te.th32ThreadID < main_tid) {
            main_tid = te.th32ThreadID;
          }
        } while (Thread32Next(snap, &te));
      }
      CloseHandle(snap);
    }
    if (main_tid != 0xFFFFFFFF) {
      BOOST_LOG(info) << "Cleanup: stage3 posting WM_QUIT to TID=" << main_tid << " (PID=" << pid << ")";
      PostThreadMessageW(main_tid, WM_QUIT, 0, 0);
    } else {
      BOOST_LOG(info) << "Cleanup: stage3 no thread found to post WM_QUIT (PID=" << pid << ")";
    }
    // Best-effort console CTRL event
    if (AttachConsole(pid)) {
      BOOST_LOG(info) << "Cleanup: stage3 attached console; sending CTRL_BREAK (PID=" << pid << ")";
      SetConsoleCtrlHandler(nullptr, TRUE);
      GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0);
      FreeConsole();
    }
  }

  static bool ensure_window_minimized(HWND hwnd, std::chrono::milliseconds timeout) {
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

  static void cleanup_fullscreen_via_desktop(int exit_timeout_secs) {
    BOOST_LOG(info) << "Cleanup fullscreen: launching DesktopApp to close fullscreen";
    std::wstring desktop_path;
    try {
      std::wstring assocExe = query_playnite_executable_from_assoc();
      if (!assocExe.empty()) {
        std::filesystem::path assoc_path(assocExe);
        if (_wcsicmp(assoc_path.filename().c_str(), L"Playnite.DesktopApp.exe") == 0) {
          desktop_path = assocExe;
        } else {
          std::filesystem::path candidate = assoc_path.parent_path() / L"Playnite.DesktopApp.exe";
          if (std::filesystem::exists(candidate)) {
            desktop_path = candidate.wstring();
          } else {
            desktop_path = assocExe;  // last resort
          }
        }
      }
    } catch (...) {
      desktop_path.clear();
    }

    bool launch_success = false;
    if (!desktop_path.empty() && std::filesystem::exists(desktop_path)) {
      std::wstring cmd = L"\"" + desktop_path + L"\" --startdesktop";
      STARTUPINFOW si {};
      si.cb = sizeof(si);
      si.dwFlags |= STARTF_USESHOWWINDOW;
      si.wShowWindow = SW_HIDE;
      PROCESS_INFORMATION pi {};
      std::wstring cmdline = cmd;
      DWORD flags_base = CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW | DETACHED_PROCESS;
      DWORD flags_try = flags_base | CREATE_BREAKAWAY_FROM_JOB;
      launch_success = CreateProcessW(desktop_path.c_str(), cmdline.data(), nullptr, nullptr, FALSE, flags_try, nullptr, nullptr, &si, &pi);
      if (!launch_success) {
        launch_success = CreateProcessW(desktop_path.c_str(), cmdline.data(), nullptr, nullptr, FALSE, flags_base, nullptr, nullptr, &si, &pi);
      }
      if (launch_success) {
        if (pi.hThread) {
          CloseHandle(pi.hThread);
        }
        if (pi.hProcess) {
          CloseHandle(pi.hProcess);
        }
      }
      BOOST_LOG(info) << "Cleanup fullscreen: launch DesktopApp attempt result=" << (launch_success ? "ok" : "fail");
    } else {
      BOOST_LOG(warning) << "Cleanup fullscreen: unable to resolve Playnite.DesktopApp path";
    }

    auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(3, exit_timeout_secs));
    std::vector<DWORD> desktop_pids;
    while (std::chrono::steady_clock::now() < wait_deadline) {
      try {
        desktop_pids = platf::dxgi::find_process_ids_by_name(L"Playnite.DesktopApp.exe");
      } catch (...) {
        desktop_pids.clear();
      }
      if (!desktop_pids.empty()) {
        break;
      }
      std::this_thread::sleep_for(200ms);
    }
    if (desktop_pids.empty()) {
      BOOST_LOG(warning) << "Cleanup fullscreen: DesktopApp did not appear after launch";
    }

    auto monitor_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool minimized_once = false;
    while (std::chrono::steady_clock::now() < monitor_deadline) {
      HWND desktop_hwnd = nullptr;
      for (auto pid : desktop_pids) {
        desktop_hwnd = find_main_window_for_pid(pid);
        if (desktop_hwnd) {
          break;
        }
      }
      if (!desktop_hwnd) {
        std::this_thread::sleep_for(300ms);
        continue;
      }
      if (!IsWindow(desktop_hwnd)) {
        BOOST_LOG(info) << "Cleanup fullscreen: DesktopApp window closed before minimize";
        break;
      }
      if (IsWindowVisible(desktop_hwnd) && !IsIconic(desktop_hwnd)) {
        BOOST_LOG(info) << "Cleanup fullscreen: DesktopApp visible; minimizing";
        if (ensure_window_minimized(desktop_hwnd, std::chrono::seconds(5))) {
          BOOST_LOG(info) << "Cleanup fullscreen: DesktopApp minimized";
        } else {
          BOOST_LOG(warning) << "Cleanup fullscreen: failed to confirm DesktopApp minimized";
        }
        minimized_once = true;
        break;
      }
      std::this_thread::sleep_for(300ms);
    }
    if (!minimized_once) {
      BOOST_LOG(info) << "Cleanup fullscreen: DesktopApp window never reported visible before timeout";
    }

    auto fullscreen_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(3, exit_timeout_secs));
    bool fullscreen_gone = false;
    std::vector<DWORD> fullscreen_pids;
    while (std::chrono::steady_clock::now() < fullscreen_deadline) {
      try {
        fullscreen_pids = platf::dxgi::find_process_ids_by_name(L"Playnite.FullscreenApp.exe");
      } catch (...) {
        fullscreen_pids.clear();
      }
      if (fullscreen_pids.empty()) {
        fullscreen_gone = true;
        break;
      }
      std::this_thread::sleep_for(250ms);
    }
    if (fullscreen_gone) {
      BOOST_LOG(info) << "Cleanup fullscreen: FullscreenApp exited after desktop launch";
    }

    // No further minimize attempts after the first detection; exit watcher.
  }

  static bool confirm_foreground_pid(DWORD pid) {
    HWND fg = GetForegroundWindow();
    DWORD fpid = 0;
    if (fg) {
      GetWindowThreadProcessId(fg, &fpid);
    }
    return fpid == pid;
  }

  static bool focus_process_by_name_extended(const wchar_t *exe_name_w, int max_successes, int timeout_secs, bool exit_on_first, std::function<bool()> cancel = {}) {
    if (timeout_secs <= 0 || max_successes < 0) {
      return false;
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_secs);
    int successes = 0;
    bool any = false;
    // Throttle focus application to at most once per second
    auto last_apply = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
      if (cancel && cancel()) {
        break;
      }
      auto pids = platf::dxgi::find_process_ids_by_name(exe_name_w);
      for (auto pid : pids) {
        if (cancel && cancel()) {
          break;
        }
        if (confirm_foreground_pid(pid)) {
          std::this_thread::sleep_for(200ms);
          continue;
        }
        // Enforce 1s minimum interval between focus attempts
        auto now = std::chrono::steady_clock::now();
        if (now - last_apply < 1s) {
          continue;
        }
        HWND hwnd = find_main_window_for_pid(pid);
        if (hwnd && try_focus_hwnd(hwnd)) {
          std::this_thread::sleep_for(100ms);
          if (confirm_foreground_pid(pid)) {
            successes++;
            any = true;
            BOOST_LOG(info) << "Confirmed focus for PID=" << pid << ", successes=" << successes;
            if (exit_on_first) {
              return true;
            }
            if (max_successes > 0 && successes >= max_successes) {
              return true;
            }
          }
        }
        // Record the last time we attempted to apply focus
        last_apply = now;
      }
      // Outer pacing to roughly 1Hz focus attempt cadence
      std::this_thread::sleep_for(1s);
    }
    return any;
  }

  // Forward declarations for helpers defined later in this file
  static bool get_process_image_path(DWORD pid, std::wstring &out);
  static bool path_starts_with(const std::wstring &path, const std::wstring &dir);

  // Enumerate all running processes whose image path begins with install_dir,
  // return sorted by working set (descending)
  static std::vector<DWORD> find_pids_under_install_dir_sorted(const std::wstring &install_dir) {
    std::vector<DWORD> result;
    if (install_dir.empty()) {
      return result;
    }

    // Gather PIDs via EnumProcesses
    DWORD needed = 0;
    std::vector<DWORD> pids(1024);
    if (!EnumProcesses(pids.data(), (DWORD) (pids.size() * sizeof(DWORD)), &needed)) {
      return result;
    }
    if (needed > pids.size() * sizeof(DWORD)) {
      pids.resize((needed / sizeof(DWORD)) + 256);
      if (!EnumProcesses(pids.data(), (DWORD) (pids.size() * sizeof(DWORD)), &needed)) {
        return result;
      }
    }
    size_t count = needed / sizeof(DWORD);

    struct Item {
      DWORD pid;
      SIZE_T wset;
    };

    std::vector<Item> items;
    items.reserve(count);

    for (size_t i = 0; i < count; ++i) {
      DWORD pid = pids[i];
      if (pid == 0) {
        continue;
      }
      std::wstring img;
      if (!get_process_image_path(pid, img)) {
        continue;
      }
      if (!path_starts_with(img, install_dir)) {
        continue;
      }

      // Check if there is a focusable top-level window
      HWND hwnd = find_main_window_for_pid(pid);
      if (!hwnd) {
        continue;
      }

      HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
      SIZE_T wset = 0;
      if (h) {
        PROCESS_MEMORY_COUNTERS_EX pmc {};
        if (GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS *) &pmc, sizeof(pmc))) {
          wset = pmc.WorkingSetSize;
        }
        CloseHandle(h);
      }
      items.push_back({pid, wset});
    }

    std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
      return a.wset > b.wset;
    });
    result.reserve(items.size());
    for (auto &it : items) {
      result.push_back(it.pid);
    }
    return result;
  }

  // Try to focus any process under install_dir, preferring largest working set.
  // Waits up to total_wait_sec for a matching process to appear. For each candidate,
  // attempts SetForegroundWindow 'attempts' times with 1s delay.
  static bool focus_by_install_dir_extended(const std::wstring &install_dir, int max_successes, int total_wait_sec, bool exit_on_first, std::function<bool()> cancel = {}) {
    if (install_dir.empty() || total_wait_sec <= 0 || max_successes < 0) {
      return false;
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(1, total_wait_sec));
    int successes = 0;
    bool any = false;
    // Throttle focus application to at most once per second
    auto last_apply = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
      if (cancel && cancel()) {
        break;
      }
      auto candidates = find_pids_under_install_dir_sorted(install_dir);
      if (!candidates.empty()) {
        for (auto pid : candidates) {
          if (cancel && cancel()) {
            break;
          }
          if (confirm_foreground_pid(pid)) {
            continue;
          }
          // Enforce 1s minimum interval between focus attempts
          auto now = std::chrono::steady_clock::now();
          if (now - last_apply < 1s) {
            continue;
          }
          HWND hwnd = find_main_window_for_pid(pid);
          if (hwnd && try_focus_hwnd(hwnd)) {
            std::this_thread::sleep_for(100ms);
            if (confirm_foreground_pid(pid)) {
              successes++;
              any = true;
              BOOST_LOG(info) << "Confirmed focus (installDir) for PID=" << pid << ", successes=" << successes;
              if (exit_on_first) {
                return true;
              }
              if (max_successes > 0 && successes >= max_successes) {
                return true;
              }
            }
          }
          // Record the last time we attempted to apply focus
          last_apply = now;
        }
      } else {
        // No candidates yet; wait a bit and retry within the total window
        std::this_thread::sleep_for(1s);
      }
      // Outer pacing to roughly 1Hz focus attempt cadence
      std::this_thread::sleep_for(1s);
    }
    return any;
  }

  static std::wstring to_lower_copy(std::wstring s) {
    for (auto &ch : s) {
      ch = (wchar_t) std::towlower(ch);
    }
    return s;
  }

  static bool path_starts_with(const std::wstring &path, const std::wstring &dir) {
    if (dir.empty()) {
      return false;
    }
    auto p = to_lower_copy(path);
    auto d = to_lower_copy(dir);
    // Normalize separators
    for (auto &ch : p) {
      if (ch == L'/') {
        ch = L'\\';
      }
    }
    for (auto &ch : d) {
      if (ch == L'/') {
        ch = L'\\';
      }
    }
    if (p.size() < d.size()) {
      return false;
    }
    if (p.rfind(d, 0) != 0) {
      return false;
    }
    // Ensure boundary (dir match ends on separator or exact)
    if (p.size() == d.size()) {
      return true;
    }
    return p[d.size()] == L'\\';
  }

  static bool get_process_image_path(DWORD pid, std::wstring &out) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
      return false;
    }
    wchar_t buf[MAX_PATH];
    DWORD sz = ARRAYSIZE(buf);
    BOOL ok = QueryFullProcessImageNameW(h, 0, buf, &sz);
    CloseHandle(h);
    if (!ok) {
      return false;
    }
    out.assign(buf, sz);
    return true;
  }

  static void terminate_pid(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h) {
      return;
    }
    TerminateProcess(h, 1);
    CloseHandle(h);
  }

  // Enumerate top-level windows of a PID and send WM_CLOSE
  // Graceful-then-forceful cleanup of processes whose image path is under install_dir
  static void cleanup_graceful_then_forceful_in_dir(const std::wstring &install_dir, int exit_timeout_secs) {
    if (install_dir.empty()) {
      return;
    }
    BOOST_LOG(info) << "Cleanup: begin for install_dir='" << platf::dxgi::wide_to_utf8(install_dir) << "' timeout=" << exit_timeout_secs << "s";
    // Gather initial candidate PIDs
    auto collect = [&]() {
      return find_pids_under_install_dir_sorted(install_dir);
    };

    // Time-sliced escalation: 0-40% close; 40-70% endsession; 70-100% quit/console; then force
    auto t_total = std::max(1, exit_timeout_secs);
    auto t_start = std::chrono::steady_clock::now();
    bool sent_close = false, sent_endsession = false, sent_quit = false;
    bool logged_initial = false;
    while (true) {
      auto remaining = collect();
      if (!logged_initial) {
        BOOST_LOG(info) << "Cleanup: initial candidates count=" << remaining.size();
        for (auto pid : remaining) {
          std::wstring img;
          get_process_image_path(pid, img);
          BOOST_LOG(info) << "Cleanup: candidate PID=" << pid << " path='" << platf::dxgi::wide_to_utf8(img) << "'";
        }
        logged_initial = true;
      }
      if (remaining.empty()) {
        BOOST_LOG(info) << "Cleanup: all processes exited gracefully";
        return;
      }
      auto now = std::chrono::steady_clock::now();
      auto elapsed_ms = (int) std::chrono::duration_cast<std::chrono::milliseconds>(now - t_start).count();
      double frac = std::min(1.0, (double) elapsed_ms / (double) (t_total * 1000));

      if (!sent_close) {
        BOOST_LOG(info) << "Cleanup: stage 1 (SC_CLOSE/WM_CLOSE) for " << remaining.size() << " processes";
        for (auto pid : remaining) {
          stage_close_windows_for_pid(pid);
        }
        sent_close = true;
      } else if (frac >= 0.4 && !sent_endsession) {
        BOOST_LOG(info) << "Cleanup: stage 2 (QUERYENDSESSION/ENDSESSION)";
        for (auto pid : remaining) {
          stage_logoff_for_pid(pid);
        }
        sent_endsession = true;
      } else if (frac >= 0.7 && !sent_quit) {
        BOOST_LOG(info) << "Cleanup: stage 3 (WM_QUIT + console CTRL)";
        for (auto pid : remaining) {
          stage_quit_thread_or_console(pid);
        }
        sent_quit = true;
      }

      if (frac >= 1.0) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    // Force terminate any remaining
    auto remaining = collect();
    for (auto pid : remaining) {
      std::wstring img;
      get_process_image_path(pid, img);
      BOOST_LOG(warning) << "Cleanup: forcing termination of PID=" << pid << (img.empty() ? "" : (" path=" + platf::dxgi::wide_to_utf8(img)));
      terminate_pid(pid);
    }
  }

  struct ProcSnapshot {
    std::unordered_map<DWORD, std::vector<DWORD>> children;
    std::unordered_map<DWORD, std::wstring> exe_basename;
    std::unordered_map<DWORD, std::wstring> img_path;
  };

}  // namespace

// Shared launcher logic; invoked by both main and WinMain wrappers
static int launcher_run(int argc, char **argv) {
  // Minimal arg parsing
  std::string game_id;
  std::string public_guid;
  std::string timeout_s;
  std::string focus_attempts_s;
  std::string focus_timeout_s;
  std::string exit_timeout_s;
  bool focus_exit_on_first_flag = false;
  std::string cleanup_flag;
  std::string install_dir_arg;
  std::string wait_for_pid_s;
  std::span<char *> argspan(argv, (size_t) argc);
  parse_arg(argspan, "--game-id", game_id);
  parse_arg(argspan, "--public-guid", public_guid);
  parse_arg(argspan, "--timeout", timeout_s);
  parse_arg(argspan, "--focus-attempts", focus_attempts_s);
  parse_arg(argspan, "--focus-timeout", focus_timeout_s);
  parse_arg(argspan, "--exit-timeout", exit_timeout_s);
  focus_exit_on_first_flag = parse_flag(argspan, "--focus-exit-on-first");
  bool fullscreen = parse_flag(argspan, "--fullscreen");
  bool do_cleanup = parse_flag(argspan, "--do-cleanup");
  parse_arg(argspan, "--install-dir", install_dir_arg);
  parse_arg(argspan, "--wait-for-pid", wait_for_pid_s);

  if (!fullscreen && !do_cleanup && game_id.empty()) {
    std::fprintf(stderr, "playnite-launcher: missing --game-id <GUID> or --fullscreen\n");
    return 2;
  }

  // Startup timeout: only applies before the game actually starts
  // Default to 2 minutes; once the game starts, we wait indefinitely for stop
  int timeout_sec = 120;
  if (!timeout_s.empty()) {
    try {
      timeout_sec = std::max(1, std::stoi(timeout_s));
    } catch (...) {}
  }
  int focus_attempts = 3;
  if (!focus_attempts_s.empty()) {
    try {
      focus_attempts = std::max(0, std::stoi(focus_attempts_s));
    } catch (...) {}
  }
  int focus_timeout_secs = 15;
  if (!focus_timeout_s.empty()) {
    try {
      focus_timeout_secs = std::max(0, std::stoi(focus_timeout_s));
    } catch (...) {}
  }

  int exit_timeout_secs = 10;  // default graceful-exit window for cleanup
  if (!exit_timeout_s.empty()) {
    try {
      exit_timeout_secs = std::max(0, std::stoi(exit_timeout_s));
    } catch (...) {}
  }

  // Best effort: do not keep/attach a console if started from one
  FreeConsole();

  // Initialize logging to a dedicated launcher log file
  // Resolve log path under %AppData%\Sunshine
  std::wstring appdataW;
  appdataW.resize(MAX_PATH);
  if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appdataW.data()))) {
    appdataW = L".";  // fallback to current dir
  }
  appdataW.resize(wcslen(appdataW.c_str()));
  std::filesystem::path logdir = std::filesystem::path(appdataW) / L"Sunshine";
  std::error_code ec;
  std::filesystem::create_directories(logdir, ec);
  auto logfile = (logdir / L"sunshine_playnite_launcher.log");
  auto log_path = logfile.string();
  // Use append-mode logging to avoid cross-process truncation races with the cleanup watcher
  auto _log_guard = logging::init_append(2 /*info*/, log_path);
  BOOST_LOG(info) << "Playnite launcher starting; pid=" << GetCurrentProcessId();

  // Ensure Playnite is running if requested actions depend on it
  auto ensure_playnite_open = [&]() {
    if (!is_playnite_running()) {
      BOOST_LOG(info) << "Playnite not running; opening playnite:// URI in detached mode";
      if (!launch_uri_detached_parented(L"playnite://")) {
        BOOST_LOG(warning) << "Failed to launch playnite:// via detached CreateProcess";
      }
    }
  };

  // Cleanup-only mode: optionally wait for a specific PID to exit, then run cleanup
  if (do_cleanup) {
    BOOST_LOG(info) << "Cleanup mode: starting (installDir='" << install_dir_arg << "' fullscreen=" << (fullscreen ? 1 : 0) << ")";
    // Optional: wait for a specific PID (typically parent launcher) to exit
    if (!wait_for_pid_s.empty()) {
      try {
        DWORD wpid = (DWORD) std::stoul(wait_for_pid_s);
        if (wpid != 0 && wpid != GetCurrentProcessId()) {
          HANDLE hp = OpenProcess(SYNCHRONIZE, FALSE, wpid);
          if (hp) {
            BOOST_LOG(info) << "Cleanup mode: waiting for PID=" << wpid << " to exit";
            // Wait indefinitely for the process to exit
            DWORD wr = WaitForSingleObject(hp, INFINITE);
            CloseHandle(hp);
            BOOST_LOG(info) << "Cleanup mode: wait result=" << wr;
          } else {
            BOOST_LOG(warning) << "Cleanup mode: unable to open PID for wait: " << wpid;
          }
        }
      } catch (...) {
        BOOST_LOG(warning) << "Cleanup mode: invalid --wait-for-pid value: '" << wait_for_pid_s << "'";
      }
    }
    std::wstring install_dir_w = platf::dxgi::utf8_to_wide(install_dir_arg);
    if (!fullscreen && !install_dir_w.empty()) {
      cleanup_graceful_then_forceful_in_dir(install_dir_w, exit_timeout_secs);
    }
    if (fullscreen) {
      cleanup_fullscreen_via_desktop(std::max(3, exit_timeout_secs));
    }
    BOOST_LOG(info) << "Cleanup mode: done";
    return 0;
  }

  // Fullscreen mode: start Playnite.FullscreenApp and apply focus attempts, then schedule cleanup and exit
  if (fullscreen) {
    BOOST_LOG(info) << "Fullscreen mode requested; attempting to start Playnite.FullscreenApp.exe";
    bool started = false;
    std::string fullscreen_install_dir_utf8;
    try {
      std::wstring assocExe = query_playnite_executable_from_assoc();
      if (!assocExe.empty()) {
        std::filesystem::path base = std::filesystem::path(assocExe).parent_path();
        fullscreen_install_dir_utf8 = platf::dxgi::wide_to_utf8(base.wstring());
        std::filesystem::path fs = base / L"Playnite.FullscreenApp.exe";
        if (std::filesystem::exists(fs)) {
          BOOST_LOG(info) << "Launching FullscreenApp from: " << platf::dxgi::wide_to_utf8(fs.wstring());
          started = launch_executable_detached_parented(fs.wstring());
        }
      }
    } catch (...) {}
    if (!started) {
      BOOST_LOG(info) << "Fullscreen exe not resolved; falling back to playnite://";
      ensure_playnite_open();
    }
    WCHAR selfPath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, selfPath, ARRAYSIZE(selfPath));
    if (!spawn_cleanup_watchdog_process(selfPath, fullscreen_install_dir_utf8, exit_timeout_secs, true, GetCurrentProcessId())) {
      BOOST_LOG(warning) << "Fullscreen mode: failed to spawn cleanup watchdog";
    }
    // Wait briefly for process to appear then try to focus
    auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
      auto pids = platf::dxgi::find_process_ids_by_name(L"Playnite.FullscreenApp.exe");
      if (!pids.empty()) {
        break;
      }
      std::this_thread::sleep_for(300ms);
    }
    bool focused = focus_process_by_name_extended(L"Playnite.FullscreenApp.exe", focus_attempts, focus_timeout_secs, focus_exit_on_first_flag);
    BOOST_LOG(info) << (focused ? "Fullscreen focus applied" : "Fullscreen focus not confirmed");

    // Keep the helper alive while Playnite is running. This ensures Sunshine treats
    // the fullscreen entry as active until the user exits Playnite.
    int consecutive_missing = 0;
    auto next_focus_check = std::chrono::steady_clock::now();
    int successes_left = std::max(0, focus_attempts);
    auto focus_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(0, focus_timeout_secs));
    bool focus_budget_active = successes_left > 0 && focus_timeout_secs > 0;
    while (true) {
      bool fs_running = false;
      std::vector<DWORD> fs_pids;
      try {
        fs_pids = platf::dxgi::find_process_ids_by_name(L"Playnite.FullscreenApp.exe");
        fs_running = !fs_pids.empty();
      } catch (...) {}

      if (fs_running) {
        consecutive_missing = 0;
        auto now = std::chrono::steady_clock::now();
        if (focus_budget_active && now >= next_focus_check) {
          bool already_fg = false;
          for (auto pid : fs_pids) {
            if (confirm_foreground_pid(pid)) {
              already_fg = true;
              break;
            }
          }
          if (!already_fg) {
            int remaining_secs = (int) std::chrono::duration_cast<std::chrono::seconds>(focus_deadline - now).count();
            if (remaining_secs > 0 && successes_left > 0) {
              bool ok = focus_process_by_name_extended(L"Playnite.FullscreenApp.exe", 1, std::min(2, remaining_secs), true);
              if (ok) {
                successes_left--;
              }
            }
            if (successes_left <= 0 || std::chrono::steady_clock::now() >= focus_deadline) {
              focus_budget_active = false;
            }
          }
          next_focus_check = now + 2s;
        }
      } else {
        consecutive_missing++;
        if (consecutive_missing >= 6) {
          break;
        }
      }
      std::this_thread::sleep_for(500ms);
    }
    BOOST_LOG(info) << "Playnite appears closed; exiting launcher";
    return 0;
  }

  // If no public GUID was supplied, relaunch self with one so the GUID is visible
  if (public_guid.empty()) {
    try {
      public_guid = platf::dxgi::generate_guid();
    } catch (...) {
      public_guid.clear();
    }
    // Build command line: "exe" --game-id <id> --public-guid <guid> [--timeout N]
    WCHAR selfPath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, selfPath, ARRAYSIZE(selfPath));
    std::wstring wexe(selfPath);
    std::wstring wcmd = L"\"" + wexe + L"\" --game-id " + std::wstring(game_id.begin(), game_id.end()) +
                        L" --public-guid " + std::wstring(public_guid.begin(), public_guid.end()) +
                        (timeout_s.empty() ? L"" : (L" --timeout " + std::wstring(timeout_s.begin(), timeout_s.end())));
    STARTUPINFOW si {};
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESHOWWINDOW;  // ensure hidden window
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi {};
    std::wstring cmdline = wcmd;  // mutable buffer for CreateProcess
    BOOL ok = CreateProcessW(selfPath, cmdline.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi);
    if (ok) {
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      BOOST_LOG(info) << "Spawned child with public GUID: " << public_guid;
      return 0;  // Parent exits; child continues with server
    } else {
      BOOST_LOG(error) << "Failed to spawn child process with GUID; continuing in current process";
    }
  }
  BOOST_LOG(info) << "Using public GUID: " << public_guid;

  // Build dynamic control pipe name
  std::string control_name = std::string("sunshine_playnite_ctl_") + public_guid;

  // Discovery marker removed: do not write JSON files under %AppData%/Sunshine/playnite_launcher

  // Host IPC server and watch for status updates
  platf::playnite::IpcServer server(control_name);
  std::atomic<bool> got_started {false};
  std::atomic<bool> should_exit {false};
  std::string last_install_dir;
  std::string last_game_exe;
  std::atomic<bool> watcher_spawned {false};

  auto spawn_cleanup_watcher = [&](const std::string &install_dir_utf8) {
    bool expected = false;
    if (!watcher_spawned.compare_exchange_strong(expected, true)) {
      return;
    }
    WCHAR selfPath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, selfPath, ARRAYSIZE(selfPath));
    if (!spawn_cleanup_watchdog_process(selfPath, install_dir_utf8, exit_timeout_secs, false, GetCurrentProcessId())) {
      watcher_spawned.store(false);
    }
  };

  server.set_message_handler([&](std::span<const uint8_t> bytes) {
    // Incoming messages are JSON objects from the plugin; parse and watch for status
    auto msg = platf::playnite::parse(bytes);
    using MT = platf::playnite::MessageType;
    if (msg.type == MT::Status) {
      BOOST_LOG(info) << "Status: name=" << msg.status_name << " id=" << msg.status_game_id;
      auto norm = [](std::string s) {
        s.erase(std::remove_if(s.begin(), s.end(), [](char c) {
                  return c == '{' || c == '}';
                }),
                s.end());
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
          return (char) std::tolower(c);
        });
        return s;
      };
      if (!msg.status_game_id.empty() && norm(msg.status_game_id) == norm(game_id)) {
        if (msg.status_name == "gameStarted") {
          got_started.store(true);
        }
        if (msg.status_name == "gameStopped") {
          should_exit.store(true);
        }
        if (!msg.status_install_dir.empty()) {
          last_install_dir = msg.status_install_dir;
          // As soon as we learn the install dir, spawn a watcher to handle Moonlight-side termination
          spawn_cleanup_watcher(last_install_dir);
        }
        if (!msg.status_exe.empty()) {
          last_game_exe = msg.status_exe;
        }
      }
    }
  });

  server.start();

  // If launching a game, ensure Playnite is running first (best-effort)
  if (!game_id.empty()) {
    ensure_playnite_open();
  }

  // Wait for data pipe active then send launch command
  auto start_deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
  while (!server.is_active() && std::chrono::steady_clock::now() < start_deadline) {
    std::this_thread::sleep_for(50ms);
  }
  if (!server.is_active()) {
    BOOST_LOG(error) << "IPC did not become active; exiting";
    return 3;
  }

  // Send command: {type:"command", command:"launch", id:"<GUID>"}
  {
    nlohmann::json j;
    j["type"] = "command";
    j["command"] = "launch";
    j["id"] = game_id;
    server.send_json_line(j.dump());
    BOOST_LOG(info) << "Launch command sent for id=" << game_id;
  }

  // Best-effort: shortly after we observe game start, attempt to bring the game (or Playnite) to foreground
  // (helps controller navigation start). Prefer processes under the game's install directory by working set;
  // then try the specific game EXE by name; else fall back to Playnite windows.
  if (focus_attempts > 0 && focus_timeout_secs > 0) {
    auto start_wait = std::chrono::steady_clock::now() + 5s;
    while (!got_started.load() && std::chrono::steady_clock::now() < start_wait) {
      std::this_thread::sleep_for(200ms);
    }
    bool focused = false;
    auto overall_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(1, focus_timeout_secs));
    if (!focused && !last_install_dir.empty()) {
      try {
        std::wstring wdir = platf::dxgi::utf8_to_wide(last_install_dir);
        BOOST_LOG(info) << "Autofocus: trying installDir=" << last_install_dir;
        int remaining = (int) std::chrono::duration_cast<std::chrono::seconds>(overall_deadline - std::chrono::steady_clock::now()).count();
        if (remaining > 0) {
          focused = focus_by_install_dir_extended(wdir, focus_attempts, remaining, focus_exit_on_first_flag, [&]() {
            return should_exit.load();
          });
        }
      } catch (...) {}
    }
    if (!last_game_exe.empty()) {
      try {
        std::wstring wexe = platf::dxgi::utf8_to_wide(last_game_exe);
        // Strip path and focus by basename
        std::filesystem::path p = wexe;
        std::wstring base = p.filename().wstring();
        if (!base.empty()) {
          int remaining = (int) std::chrono::duration_cast<std::chrono::seconds>(overall_deadline - std::chrono::steady_clock::now()).count();
          if (remaining > 0) {
            focused = focus_process_by_name_extended(base.c_str(), focus_attempts, remaining, focus_exit_on_first_flag, [&]() {
              return should_exit.load();
            });
          }
        }
      } catch (...) {}
    }
    if (!focused) {
      int remaining = (int) std::chrono::duration_cast<std::chrono::seconds>(overall_deadline - std::chrono::steady_clock::now()).count();
      if (remaining > 0) {
        focused = focus_process_by_name_extended(L"Playnite.FullscreenApp.exe", focus_attempts, remaining, focus_exit_on_first_flag, [&]() {
          return should_exit.load();
        });
      }
    }
    if (!focused) {
      int remaining = (int) std::chrono::duration_cast<std::chrono::seconds>(overall_deadline - std::chrono::steady_clock::now()).count();
      if (remaining > 0) {
        focused = focus_process_by_name_extended(L"Playnite.DesktopApp.exe", focus_attempts, remaining, focus_exit_on_first_flag, [&]() {
          return should_exit.load();
        });
      }
    }
    BOOST_LOG(info) << (focused ? "Applied focus after launch" : "Focus not applied after launch");
  }

  // Wait for stop; apply timeout only if the game never starts
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
  while (!should_exit.load()) {
    // If we haven't seen the game start, enforce startup timeout
    if (!got_started.load() && std::chrono::steady_clock::now() >= deadline) {
      break;  // startup timeout only
    }
    // If Playnite has exited but we saw the game start, proceed to cleanup immediately
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
    // Best-effort cleanup: remove marker file
    // Discovery marker removal skipped (marker not created)
    // Fall through to schedule cleanup anyway
  }

  BOOST_LOG(info) << "Playnite reported gameStopped or timeout; scheduling cleanup and exiting";
  // Spawn background cleanup to terminate lingering game executables under install dir
  if (!last_install_dir.empty()) {
    WCHAR selfPath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, selfPath, ARRAYSIZE(selfPath));
    static_cast<void>(spawn_cleanup_watchdog_process(selfPath, last_install_dir, exit_timeout_secs, false, std::nullopt));
  }
  return should_exit.load() ? 0 : 4;
}

// Console entry point (used by non-WIN32 subsystem builds and tests)
int main(int argc, char **argv) {
  return launcher_run(argc, argv);
}

#ifdef _WIN32
// GUI subsystem entry point: avoid console window
int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
  int argc = 0;
  LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::string> utf8args;
  utf8args.reserve(argc);
  for (int i = 0; i < argc; ++i) {
    int need = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
    std::string s;
    s.resize(need > 0 ? (size_t) need - 1 : 0);
    if (need > 0) {
      WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), need, nullptr, nullptr);
    }
    utf8args.emplace_back(std::move(s));
  }
  std::vector<char *> argv;
  argv.reserve(utf8args.size());
  for (auto &s : utf8args) {
    argv.push_back(s.data());
  }
  int rc = launcher_run((int) argv.size(), argv.data());
  if (wargv) {
    LocalFree(wargv);
  }
  return rc;
}
#endif
