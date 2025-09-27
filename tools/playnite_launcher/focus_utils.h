#pragma once

#include <functional>
#include <string>
#include <vector>
#include <windows.h>
#include <winsock2.h>

namespace playnite_launcher::focus {

  HWND find_main_window_for_pid(DWORD pid);
  bool try_focus_hwnd(HWND hwnd);
  bool confirm_foreground_pid(DWORD pid);

  bool focus_process_by_name_extended(const wchar_t *exe_name_w, int max_successes, int timeout_secs, bool exit_on_first, std::function<bool()> cancel = {});

  std::vector<DWORD> find_pids_under_install_dir_sorted(const std::wstring &install_dir);

  bool focus_by_install_dir_extended(const std::wstring &install_dir, int max_successes, int total_wait_sec, bool exit_on_first, std::function<bool()> cancel = {});

  bool get_process_image_path(DWORD pid, std::wstring &out);
  void terminate_pid(DWORD pid);

}  // namespace playnite_launcher::focus
