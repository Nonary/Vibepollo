/**
 * @file src/process_context.h
 * @brief Application catalog entries shared by process launch and refresh.
 */
#pragma once

#include "display_target_lease.h"

#include "config.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace proc {
  typedef config::prep_cmd_t cmd_t;

  inline constexpr int kLosslessScalingDefaultLaunchDelaySeconds = 8;

  /**
   * pre_cmds -- guaranteed to be executed unless any of the commands fail.
   * detached -- commands detached from Sunshine
   * cmd -- Runs indefinitely until:
   *    No session is running and a different set of commands it to be executed
   *    Command exits
   * working_dir -- the process working directory. This is required for some games to run properly.
   * cmd_output --
   *    empty    -- The output of the commands are appended to the output of sunshine
   *    "null"   -- The output of the commands are discarded
   *    filename -- The output of the commands are appended to filename
   */
  struct lossless_scaling_profile_overrides_t {
    std::optional<bool> performance_mode;
    std::optional<int> flow_scale;
    std::optional<int> resolution_scale;
    std::optional<std::string> scaling_type;
    std::optional<int> sharpening;
    std::optional<std::string> anime4k_size;
    std::optional<bool> anime4k_vrs;
  };

  struct ctx_t {
    std::vector<cmd_t> prep_cmds;
    std::vector<cmd_t> state_cmds;

    /**
     * Some applications, such as Steam, either exit quickly, or keep running indefinitely.
     *
     * Apps that launch normal child processes and terminate will be handled by the process
     * grouping logic (wait_all). However, apps that launch child processes indirectly or
     * into another process group (such as UWP apps) can only be handled by the auto-detach
     * heuristic which catches processes that exit 0 very quickly, but we won't have proper
     * process tracking for those.
     *
     * For cases where users just want to kick off a background process and never manage the
     * lifetime of that process, they can use detached commands for that.
     */
    std::vector<std::string> detached;

    std::string idx;
    // Original configured row; generated controls have no JSON row.
    std::optional<std::size_t> configured_index;
    std::string uuid;
    std::string name;
    std::string cmd;
    std::string working_dir;
    std::string output;
    std::optional<std::string> output_name_override;
    std::string image_path;
    std::string id;
    std::string gamepad;
    std::string art_version;
    std::vector<std::string> id_aliases;
    // Provider metadata used by the Steam-owned process lifecycle boundary.
    // These remain strings because the catalog/API stores Steam IDs and paths
    // as JSON strings for compatibility with older app entries.
    std::string steam_id;
    std::string steam_install_dir;
    // Lutris also launches through a short-lived broker. Its game directory
    // lets the shared process tracker own the real Wine/native process.
    std::string lutris_id;
    std::string lutris_directory;
    // When present, this app should be launched via Playnite instead of direct cmd.
    std::string playnite_id;
    // When true, launch Playnite in fullscreen mode via the helper.
    bool playnite_fullscreen;
    bool frame_gen_limiter_fix;
    bool elevated;
    bool dualsense_haptics {false};
    bool virtual_screen {false};
    // Explicit configured Monitor profile; its UUID is independent of its title.
    bool remote_monitor_profile {false};
    std::optional<display_target::descriptor_t> monitor_target;
    // Unset inherits the device preference; false allows normal client HDR requests.
    std::optional<bool> prefer_10bit_sdr;
    std::optional<config::video_t::virtual_display_mode_e> virtual_display_mode_override;
    std::optional<config::video_t::virtual_display_layout_e> virtual_display_layout_override;
    bool auto_detach;
    bool wait_all;
    bool virtual_display;
    bool virtual_display_primary;
    bool use_app_identity;
    bool per_client_app_identity;
    bool allow_client_commands;
    bool terminate_on_pause;
    int scale_factor;
    std::chrono::seconds exit_timeout;
    bool gen1_framegen_fix;
    bool gen2_framegen_fix;
    bool frame_generation_enabled {false};
    bool lossless_scaling_enabled {false};
    bool lossless_scaling_framegen {false};
    std::string frame_generation_provider {"lossless-scaling"};
    std::optional<double> lossless_scaling_target_fps;
    std::optional<int> lossless_scaling_rtss_limit;
    std::string lossless_scaling_profile {"custom"};
    lossless_scaling_profile_overrides_t lossless_scaling_recommended;
    lossless_scaling_profile_overrides_t lossless_scaling_custom;
    int lossless_scaling_launch_delay_seconds {kLosslessScalingDefaultLaunchDelaySeconds};
    bool lossless_scaling_legacy_auto_detect {false};
    std::optional<config::video_t::dd_t::config_option_e> dd_config_option_override;

    // Per-application overrides for global config keys (raw config-file value representation).
    // These are applied at runtime and are not persisted to the global config file.
    std::unordered_map<std::string, std::string> config_overrides;
  };

}  // namespace proc
