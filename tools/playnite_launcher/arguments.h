#pragma once

#include <string>

namespace playnite_launcher {

  struct LauncherConfig {
    std::string game_id;
    std::string public_guid;
    std::string install_dir;
    std::string wait_for_pid;
    int timeout_sec = 120;
    int focus_attempts = 3;
    int focus_timeout_secs = 15;
    int exit_timeout_secs = 10;
    bool focus_exit_on_first = false;
    bool fullscreen = false;
    bool cleanup = false;
  };

  struct ParseResult {
    LauncherConfig config;
    bool success = false;
    int exit_code = 0;
  };

  ParseResult parse_arguments(int argc, char **argv);

}  // namespace playnite_launcher
