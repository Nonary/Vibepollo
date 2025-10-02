#include "tools/playnite_launcher/playnite_process.h"

#include "src/logging.h"
#include "src/platform/windows/ipc/misc_utils.h"

#include <array>
#include <filesystem>
#include <optional>
#include <Psapi.h>
#include <shellapi.h>
#include <Shlwapi.h>
#include <string>
#include <windows.h>
#include <winsock2.h>

namespace playnite_launcher::playnite {

  bool is_playnite_running() {
    try {
      auto d = platf::dxgi::find_process_ids_by_name(L"Playnite.DesktopApp.exe");
      if (!d.empty()) {
        return true;
      }
      auto f = platf::dxgi::find_process_ids_by_name(L"Playnite.FullscreenApp.exe");
      if (!f.empty()) {
        return true;
      }
    } catch (...) {
    }
    return false;
  }

  std::wstring get_explorer_path() {
    WCHAR winDir[MAX_PATH] = {};
    if (GetWindowsDirectoryW(winDir, ARRAYSIZE(winDir)) > 0) {
      std::filesystem::path p(winDir);
      p /= L"explorer.exe";
      if (std::filesystem::exists(p)) {
        return p.wstring();
      }
    }
    WCHAR out[MAX_PATH] = {};
    DWORD rc = SearchPathW(nullptr, L"explorer.exe", nullptr, ARRAYSIZE(out), out, nullptr);
    if (rc > 0 && rc < ARRAYSIZE(out)) {
      return std::wstring(out);
    }
    return L"explorer.exe";
  }

  DWORD explorer_pid_from_windows() {
    DWORD pid = 0;
    HWND shell = GetShellWindow();
    if (shell) {
      GetWindowThreadProcessId(shell, &pid);
      if (pid) {
        return pid;
      }
    }
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray) {
      GetWindowThreadProcessId(tray, &pid);
    }
    return pid;
  }

  DWORD explorer_pid_from_process_list() {
    DWORD current_session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &current_session);
    try {
      auto pids = platf::dxgi::find_process_ids_by_name(L"explorer.exe");
      for (DWORD candidate : pids) {
        DWORD session = 0;
        ProcessIdToSessionId(candidate, &session);
        if (session == current_session) {
          return candidate;
        }
      }
      if (!pids.empty()) {
        return pids.front();
      }
    } catch (...) {
    }
    return 0;
  }

  HANDLE open_explorer_parent_handle() {
    DWORD pid = explorer_pid_from_windows();
    if (!pid) {
      pid = explorer_pid_from_process_list();
    }
    if (!pid) {
      return nullptr;
    }
    return OpenProcess(PROCESS_CREATE_PROCESS | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_DUP_HANDLE, FALSE, pid);
  }

  bool assign_parent_attributes(HANDLE parent, STARTUPINFOEXW &si, LPPROC_THREAD_ATTRIBUTE_LIST &attrList) {
    if (!parent) {
      return true;
    }
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    attrList = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, size));
    if (!attrList) {
      BOOST_LOG(warning) << "assign_parent_attributes: HeapAlloc failed";
      SetLastError(ERROR_OUTOFMEMORY);
      return false;
    }
    if (!InitializeProcThreadAttributeList(attrList, 1, 0, &size)) {
      DWORD err = GetLastError();
      BOOST_LOG(warning) << "assign_parent_attributes: InitializeProcThreadAttributeList failed: " << err;
      HeapFree(GetProcessHeap(), 0, attrList);
      attrList = nullptr;
      SetLastError(err);
      return false;
    }
    if (!UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, &parent, sizeof(parent), nullptr, nullptr)) {
      DWORD err = GetLastError();
      BOOST_LOG(warning) << "assign_parent_attributes: UpdateProcThreadAttribute failed: " << err;
      DeleteProcThreadAttributeList(attrList);
      HeapFree(GetProcessHeap(), 0, attrList);
      attrList = nullptr;
      SetLastError(err);
      return false;
    }
    si.lpAttributeList = attrList;
    return true;
  }

  void free_parent_attributes(LPPROC_THREAD_ATTRIBUTE_LIST attrList) {
    if (attrList) {
      DeleteProcThreadAttributeList(attrList);
      HeapFree(GetProcessHeap(), 0, attrList);
    }
  }

  bool launch_detached_command(const wchar_t *application, const std::wstring &cmd, STARTUPINFOEXW &si, PROCESS_INFORMATION &pi, DWORD flags) {
    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(0);
    return CreateProcessW(application, cmdline.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si.StartupInfo, &pi);
  }

  void close_process_info(PROCESS_INFORMATION &pi) {
    if (pi.hThread) {
      CloseHandle(pi.hThread);
      pi.hThread = nullptr;
    }
    if (pi.hProcess) {
      CloseHandle(pi.hProcess);
      pi.hProcess = nullptr;
    }
  }

  bool launch_uri_detached_parented(const std::wstring &uri) {
    HANDLE parent = open_explorer_parent_handle();
    if (!parent) {
      BOOST_LOG(warning) << "Unable to open explorer.exe as parent; proceeding without parent override";
    }
    STARTUPINFOEXW si {};
    si.StartupInfo.cb = sizeof(si);
    LPPROC_THREAD_ATTRIBUTE_LIST attrList = nullptr;
    if (parent && !assign_parent_attributes(parent, si, attrList)) {
      CloseHandle(parent);
      parent = nullptr;
    }
    std::wstring exe = get_explorer_path();
    std::wstring cmd = L"\"" + exe + L"\"";
    if (!uri.empty()) {
      cmd += L" ";
      cmd += uri;
    }
    DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
                  CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW | DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB;
    PROCESS_INFORMATION pi {};
    BOOL ok = launch_detached_command(exe.c_str(), cmd, si, pi, flags);
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    free_parent_attributes(attrList);
    if (parent) {
      CloseHandle(parent);
    }
    if (!ok) {
      BOOST_LOG(warning) << "CreateProcessW(explorer uri) failed: " << err;
      return false;
    }
    close_process_info(pi);
    return true;
  }

  std::wstring query_assoc_string(ASSOCSTR str, LPCWSTR extra) {
    std::array<wchar_t, 4096> buf {};
    DWORD sz = static_cast<DWORD>(buf.size());
    if (AssocQueryStringW(ASSOCF_NOTRUNCATE, str, L"playnite", extra, buf.data(), &sz) == S_OK && buf[0] != 0) {
      return std::wstring(buf.data());
    }
    return std::wstring();
  }

  std::wstring parse_command_executable(const std::wstring &command) {
    if (command.empty()) {
      return std::wstring();
    }
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(command.c_str(), &argc);
    std::wstring exe;
    if (argv && argc >= 1) {
      exe.assign(argv[0]);
    } else if (!command.empty() && command.front() == L'"') {
      auto pos = command.find(L'"', 1);
      if (pos != std::wstring::npos) {
        exe = command.substr(1, pos - 1);
      }
    }
    if (argv) {
      LocalFree(argv);
    }
    if (exe.empty()) {
      auto space = command.find(L' ');
      exe = (space == std::wstring::npos) ? command : command.substr(0, space);
    }
    return exe;
  }

  std::wstring query_playnite_executable_from_assoc() {
    auto exe = query_assoc_string(ASSOCSTR_EXECUTABLE, nullptr);
    if (!exe.empty()) {
      return exe;
    }
    auto command = query_assoc_string(ASSOCSTR_COMMAND, L"open");
    return parse_command_executable(command);
  }

  bool launch_executable_detached_parented_with_args(const std::wstring &exe_full_path, const std::wstring &args) {
    // Build command line: "<exe>" <args>
    std::wstring cmd = L"\"" + exe_full_path + L"\"";
    if (!args.empty()) {
      cmd += L" ";
      cmd += args;
    }
    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(0);

    std::wstring working_dir;
    try {
      std::filesystem::path p(exe_full_path);
      working_dir = p.parent_path().wstring();
    } catch (...) {
    }
    const wchar_t *cwd_ptr = working_dir.empty() ? nullptr : working_dir.c_str();

    auto attempt_launch = [&](bool use_parent) -> std::pair<bool, DWORD> {
      HANDLE parent = nullptr;
      LPPROC_THREAD_ATTRIBUTE_LIST attrList = nullptr;
      STARTUPINFOEXW si_ex {};
      STARTUPINFOW si_basic {};
      STARTUPINFOW *si_ptr = nullptr;
      DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
                    CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW | DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB;

      if (use_parent) {
        parent = open_explorer_parent_handle();
        if (!parent) {
          use_parent = false;
        } else if (!assign_parent_attributes(parent, si_ex, attrList)) {
          CloseHandle(parent);
          parent = nullptr;
          use_parent = false;
        } else {
          si_ex.StartupInfo.cb = sizeof(si_ex);
          si_ptr = &si_ex.StartupInfo;
        }
      }

      if (!use_parent) {
        flags &= ~EXTENDED_STARTUPINFO_PRESENT;
      }
      if (!si_ptr) {
        si_basic.cb = sizeof(si_basic);
        si_ptr = &si_basic;
      }

      PROCESS_INFORMATION pi {};
      BOOL ok = CreateProcessW(exe_full_path.c_str(), cmdline.data(), nullptr, nullptr, FALSE, flags, nullptr, cwd_ptr, si_ptr, &pi);
      DWORD err = ok ? ERROR_SUCCESS : GetLastError();
      if (pi.hThread) {
        CloseHandle(pi.hThread);
      }
      if (pi.hProcess) {
        CloseHandle(pi.hProcess);
      }
      if (attrList) {
        free_parent_attributes(attrList);
      }
      if (parent) {
        CloseHandle(parent);
      }
      return {ok != FALSE, err};
    };

    auto result = attempt_launch(true);
    if (!result.first && result.second == ERROR_INVALID_HANDLE) {
      BOOST_LOG(warning) << "CreateProcessW(executable with args) failed with ERROR_INVALID_HANDLE when using explorer parent; retrying without explicit parent";
      result = attempt_launch(false);
    }
    if (result.first) {
      return true;
    }
    BOOST_LOG(warning) << "CreateProcessW(executable with args) failed: " << result.second;
    return false;
  }

  bool launch_executable_detached_parented(const std::wstring &exe_full_path) {
    return launch_executable_detached_parented_with_args(exe_full_path, std::wstring());
  }

  bool spawn_cleanup_watchdog_process(const std::wstring &self_path, const std::string &install_dir_utf8, int exit_timeout_secs, bool fullscreen_flag, std::optional<DWORD> wait_for_pid) {
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

      BOOST_LOG(info) << "Spawning cleanup watcher (fullscreen=" << fullscreen_flag << ", installDir='" << install_dir_utf8
                      << "' waitPid=" << (wait_for_pid ? std::to_string(*wait_for_pid) : std::string("none")) << ")";

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

}  // namespace playnite_launcher::playnite
