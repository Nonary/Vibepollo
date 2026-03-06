#pragma once

#include <string>
#include <winsock2.h>
#include <windows.h>

namespace playnite_launcher::cleanup {

  void cleanup_graceful_then_forceful_in_dir(const std::wstring &install_dir, int exit_timeout_secs);
  void cleanup_fullscreen_via_desktop(int exit_timeout_secs);

}  // namespace playnite_launcher::cleanup
