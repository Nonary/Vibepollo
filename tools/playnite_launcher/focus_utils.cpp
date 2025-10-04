#include "tools/playnite_launcher/focus_utils.h"

#include "src/logging.h"
#include "src/platform/windows/ipc/misc_utils.h"

#include <algorithm>
#include <chrono>
#include <Psapi.h>
#include <thread>
#include <TlHelp32.h>
#include <vector>
#include <windows.h>

using namespace std::chrono_literals;

namespace playnite_launcher::focus {
  namespace {

    std::wstring normalize_path(std::wstring value) {
      for (auto &ch : value) {
        if (ch == L'/') {
          ch = L'\\';
        }
        ch = static_cast<wchar_t>(std::towlower(ch));
      }
      return value;
    }

    bool has_prefix_with_boundary(const std::wstring &path, const std::wstring &dir) {
      if (path.size() < dir.size()) {
        return false;
      }
      if (path.compare(0, dir.size(), dir) != 0) {
        return false;
      }
      if (path.size() == dir.size()) {
        return true;
      }
      return path[dir.size()] == L'\\';
    }

    bool path_starts_with_dir(const std::wstring &path, const std::wstring &dir) {
      if (dir.empty()) {
        return false;
      }
      return has_prefix_with_boundary(normalize_path(path), normalize_path(dir));
    }

    bool is_candidate_window(HWND hwnd, DWORD pid) {
      DWORD owner = 0;
      GetWindowThreadProcessId(hwnd, &owner);
      if (owner != pid) {
        return false;
      }
      if (!IsWindowVisible(hwnd)) {
        return false;
      }
      return GetWindow(hwnd, GW_OWNER) == nullptr;
    }

    BOOL CALLBACK find_window_proc(HWND hwnd, LPARAM param) {
      auto *result = reinterpret_cast<HWND *>(param);
      DWORD pid = static_cast<DWORD>(reinterpret_cast<uintptr_t>(*result));
      if (is_candidate_window(hwnd, pid)) {
        *result = hwnd;
        return FALSE;
      }
      return TRUE;
    }

    SIZE_T query_working_set(DWORD pid) {
      HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
      if (!process) {
        return 0;
      }
      PROCESS_MEMORY_COUNTERS_EX pmc {};
      SIZE_T size = 0;
      if (GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof(pmc))) {
        size = pmc.WorkingSetSize;
      }
      CloseHandle(process);
      return size;
    }

    std::vector<DWORD> enumerate_process_ids() {
      DWORD needed = 0;
      std::vector<DWORD> ids(1024);
      if (!EnumProcesses(ids.data(), static_cast<DWORD>(ids.size() * sizeof(DWORD)), &needed)) {
        return {};
      }
      if (needed > ids.size() * sizeof(DWORD)) {
        ids.resize((needed / sizeof(DWORD)) + 256);
        if (!EnumProcesses(ids.data(), static_cast<DWORD>(ids.size() * sizeof(DWORD)), &needed)) {
          return {};
        }
      }
      ids.resize(needed / sizeof(DWORD));
      return ids;
    }

    bool process_matches_dir(DWORD pid, const std::wstring &dir, std::wstring &path) {
      if (!get_process_image_path(pid, path)) {
        return false;
      }
      return path_starts_with_dir(path, dir);
    }

    bool append_candidate_if_match(DWORD pid, const std::wstring &install_dir, std::vector<std::pair<DWORD, SIZE_T>> &matches, bool require_window) {
      if (pid == 0) {
        return false;
      }
      std::wstring path;
      if (!process_matches_dir(pid, install_dir, path)) {
        return false;
      }
      if (require_window && !find_main_window_for_pid(pid)) {
        return false;
      }
      matches.push_back({pid, query_working_set(pid)});
      return true;
    }

    std::vector<DWORD> extract_sorted_pids(std::vector<std::pair<DWORD, SIZE_T>> matches) {
      std::sort(matches.begin(), matches.end(), [](const auto &a, const auto &b) {
        return a.second > b.second;
      });
      std::vector<DWORD> result;
      result.reserve(matches.size());
      for (auto &item : matches) {
        result.push_back(item.first);
      }
      return result;
    }

  }  // namespace

  HWND find_main_window_for_pid(DWORD pid) {
    HWND result = reinterpret_cast<HWND>(static_cast<uintptr_t>(pid));
    EnumWindows(find_window_proc, reinterpret_cast<LPARAM>(&result));
    return is_candidate_window(result, pid) ? result : nullptr;
  }

  bool try_focus_hwnd(HWND hwnd) {
    if (!hwnd) {
      return false;
    }
    HWND fg = GetForegroundWindow();
    DWORD fg_tid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD cur_tid = GetCurrentThreadId();
    if (fg && fg_tid != 0 && fg_tid != cur_tid) {
      AttachThreadInput(cur_tid, fg_tid, TRUE);
    }
    ShowWindow(hwnd, SW_RESTORE);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    BOOL ok = SetForegroundWindow(hwnd);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    if (fg && fg_tid != 0 && fg_tid != cur_tid) {
      AttachThreadInput(cur_tid, fg_tid, FALSE);
    }
    return ok != FALSE;
  }

  bool confirm_foreground_pid(DWORD pid) {
    HWND fg = GetForegroundWindow();
    DWORD fpid = 0;
    if (fg) {
      GetWindowThreadProcessId(fg, &fpid);
    }
    return fpid == pid;
  }

  bool focus_process_by_name_extended(const wchar_t *exe_name_w, int max_successes, int timeout_secs, bool exit_on_first, std::function<bool()> cancel) {
    if (!exe_name_w || timeout_secs <= 0 || max_successes < 0) {
      return false;
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(1, timeout_secs));
    int successes = 0;
    bool any = false;
    auto last_apply = std::chrono::steady_clock::now() - 1s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (cancel && cancel()) {
        break;
      }
      std::vector<DWORD> pids;
      try {
        pids = platf::dxgi::find_process_ids_by_name(exe_name_w);
      } catch (...) {
        pids.clear();
      }
      if (pids.empty()) {
        std::this_thread::sleep_for(1s);
        continue;
      }
      for (auto pid : pids) {
        if (cancel && cancel()) {
          break;
        }
        if (confirm_foreground_pid(pid)) {
          std::this_thread::sleep_for(200ms);
          continue;
        }
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
            if (exit_on_first || (max_successes > 0 && successes >= max_successes)) {
              return true;
            }
          }
        }
        last_apply = now;
      }
      std::this_thread::sleep_for(1s);
    }
    return any;
  }

  std::vector<DWORD> find_pids_under_install_dir_sorted(const std::wstring &install_dir, bool require_window) {
    auto pids = enumerate_process_ids();
    std::vector<std::pair<DWORD, SIZE_T>> matches;
    matches.reserve(pids.size());
    for (auto pid : pids) {
      append_candidate_if_match(pid, install_dir, matches, require_window);
    }
    return extract_sorted_pids(std::move(matches));
  }

  std::vector<DWORD> find_pids_under_install_dir_sorted(const std::wstring &install_dir) {
    return find_pids_under_install_dir_sorted(install_dir, true);
  }

  bool focus_by_install_dir_extended(const std::wstring &install_dir, int max_successes, int total_wait_sec, bool exit_on_first, std::function<bool()> cancel) {
    if (install_dir.empty() || total_wait_sec <= 0 || max_successes < 0) {
      return false;
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(1, total_wait_sec));
    int successes = 0;
    bool any = false;
    auto last_apply = std::chrono::steady_clock::now() - 1s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (cancel && cancel()) {
        break;
      }
      auto candidates = find_pids_under_install_dir_sorted(install_dir);
      if (candidates.empty()) {
        std::this_thread::sleep_for(1s);
        continue;
      }
      for (auto pid : candidates) {
        if (cancel && cancel()) {
          break;
        }
        if (confirm_foreground_pid(pid)) {
          continue;
        }
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
            if (exit_on_first || (max_successes > 0 && successes >= max_successes)) {
              return true;
            }
          }
        }
        last_apply = now;
      }
      std::this_thread::sleep_for(1s);
    }
    return any;
  }

  bool get_process_image_path(DWORD pid, std::wstring &out) {
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

  void terminate_pid(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h) {
      return;
    }
    TerminateProcess(h, 1);
    CloseHandle(h);
  }

}  // namespace playnite_launcher::focus
