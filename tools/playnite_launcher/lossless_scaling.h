#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <windows.h>
#include <winsock2.h>

namespace playnite_launcher::lossless {

  struct lossless_scaling_options {
    bool enabled = false;
    std::optional<int> target_fps;
    std::optional<int> rtss_limit;
    std::optional<std::filesystem::path> configured_path;
  };

  struct lossless_scaling_profile_backup {
    bool valid = false;
    bool had_auto_scale = false;
    std::string auto_scale;
    bool had_auto_scale_delay = false;
    int auto_scale_delay = 0;
    bool had_lsfg_target = false;
    int lsfg_target = 0;
  };

  struct lossless_scaling_runtime_state {
    std::vector<DWORD> running_pids;
    std::optional<std::wstring> exe_path;
    bool previously_running = false;
    bool stopped = false;
  };

  lossless_scaling_options read_lossless_scaling_options();
  lossless_scaling_runtime_state capture_lossless_scaling_state();
  void lossless_scaling_stop_processes(lossless_scaling_runtime_state &state);
  bool lossless_scaling_apply_global_profile(const lossless_scaling_options &options, const std::string &install_dir_utf8, const std::string &exe_path_utf8, lossless_scaling_profile_backup &backup);
  bool lossless_scaling_restore_global_profile(const lossless_scaling_profile_backup &backup);
  void lossless_scaling_restart_foreground(const lossless_scaling_runtime_state &state, bool force_launch);

}  // namespace playnite_launcher::lossless
