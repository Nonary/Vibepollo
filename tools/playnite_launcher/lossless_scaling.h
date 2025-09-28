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
    std::optional<std::string> active_profile;
    std::optional<std::string> capture_api;
    std::optional<int> queue_target;
    std::optional<bool> hdr_enabled;
    std::optional<int> flow_scale;
    std::optional<bool> performance_mode;
    std::optional<int> resolution_scale;
    std::optional<std::string> frame_generation_mode;
    std::optional<std::string> lsfg3_mode;
    std::optional<std::string> scaling_type;
    std::optional<int> sharpness;
    std::optional<int> ls1_sharpness;
    std::optional<std::string> anime4k_type;
    std::optional<bool> anime4k_vrs;
  };

  struct lossless_scaling_profile_backup {
    bool valid = false;
    bool had_auto_scale = false;
    std::string auto_scale;
    bool had_auto_scale_delay = false;
    int auto_scale_delay = 0;
    bool had_lsfg_target = false;
    int lsfg_target = 0;
    bool had_capture_api = false;
    std::string capture_api;
    bool had_queue_target = false;
    int queue_target = 0;
    bool had_hdr_support = false;
    bool hdr_support = false;
    bool had_flow_scale = false;
    int flow_scale = 0;
    bool had_lsfg_size = false;
    std::string lsfg_size;
    bool had_lsfg3_mode = false;
    std::string lsfg3_mode;
    bool had_frame_generation = false;
    std::string frame_generation;
    bool had_scaling_type = false;
    std::string scaling_type;
    bool had_scale_factor = false;
    double scale_factor = 1.0;
    bool had_sharpness = false;
    int sharpness = 0;
    bool had_ls1_sharpness = false;
    int ls1_sharpness = 0;
    bool had_anime4k_type = false;
    std::string anime4k_type;
    bool had_vrs = false;
    bool vrs = false;
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
