#pragma once

#include <optional>
#include <string>
#include <windows.h>
#include <winsock2.h>

namespace playnite_launcher::playnite {

  bool is_playnite_running();
  bool launch_uri_detached_parented(const std::wstring &uri);
  bool launch_executable_detached_parented_with_args(const std::wstring &exe_full_path, const std::wstring &args);
  std::wstring query_playnite_executable_from_assoc();
  bool launch_executable_detached_parented(const std::wstring &exe_full_path);
  bool spawn_cleanup_watchdog_process(const std::wstring &self_path, const std::string &install_dir_utf8, int exit_timeout_secs, bool fullscreen_flag, std::optional<DWORD> wait_for_pid);

}  // namespace playnite_launcher::playnite
