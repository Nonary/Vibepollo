#pragma once

#include <functional>
#include <string>
#include <vector>
#include <winsock2.h>
#include <windows.h>

namespace playnite_launcher::focus {

  HWND find_main_window_for_pid(DWORD pid);
  bool try_focus_hwnd(HWND hwnd);
  bool confirm_foreground_pid(DWORD pid);

  bool focus_process_by_name_extended(const wchar_t *exe_name_w, int max_successes, int timeout_secs, bool exit_on_first, std::function<bool()> cancel = {}, DWORD *confirmed_pid_out = nullptr);

  std::vector<DWORD> find_pids_under_install_dir_sorted(const std::wstring &install_dir);
  std::vector<DWORD> find_pids_under_install_dir_sorted(const std::wstring &install_dir, bool require_window);

  // Existing process/path and visible top-level window eligibility, without
  // blocking discovery. A basename fallback is retained for legacy metadata.
  std::vector<HWND> discover_game_windows(const std::wstring &install_dir, const std::wstring &game_exe = {}, bool prefer_exe = false);
  std::string window_identity(HWND window);
  bool focus_game_window(HWND window, std::function<bool()> cancel = {});

  bool focus_by_install_dir_extended(const std::wstring &install_dir, int max_successes, int total_wait_sec, bool exit_on_first, std::function<bool()> cancel = {}, DWORD *confirmed_pid_out = nullptr);

  bool get_process_image_path(DWORD pid, std::wstring &out);
  void terminate_pid(DWORD pid);

}  // namespace playnite_launcher::focus
