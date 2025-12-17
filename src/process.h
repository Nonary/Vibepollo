/**
 * @file src/process.h
 * @brief Declarations for the startup and shutdown of the apps started by a streaming Session.
 */
#pragma once

#ifndef __kernel_entry
  #define __kernel_entry
#endif

// standard includes
#include <atomic>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <chrono>

// lib includes
#include "boost_process_shim.h"
#include <nlohmann/json.hpp>

// local includes
#include "config.h"
#include "platform/common.h"
#include "rtsp.h"
#include "utility.h"

#ifdef _WIN32
  #include "platform/windows/virtual_display.h"
#include "tools/playnite_launcher/lossless_scaling.h"

namespace VDISPLAY {
  enum class DRIVER_STATUS;
}

#endif

#define VIRTUAL_DISPLAY_UUID "8902CB19-674A-403D-A587-41B092E900BA"
#define FALLBACK_DESKTOP_UUID "EAAC6159-089A-46A9-9E24-6436885F6610"
#define REMOTE_INPUT_UUID "8CB5C136-DA67-4F99-B4A1-F9CD35005CF4"
#define TERMINATE_APP_UUID "E16CBE1B-295D-4632-9A76-EC4180C857D3"

namespace bp = boost_process_shim;
namespace proc {
  using file_t = util::safe_ptr_v2<FILE, int, fclose>;

#ifdef _WIN32
  extern VDISPLAY::DRIVER_STATUS vDisplayDriverStatus;
  void initVDisplayDriver();
#endif

  typedef config::prep_cmd_t cmd_t;

  struct active_session_guard_t {
    bool has_active_app {false};
    bool uses_playnite {false};
    std::string playnite_id;
    std::string client_uuid;
    std::chrono::steady_clock::time_point launch_started_at {};
  };

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
    std::string uuid;
    std::string name;
    std::string cmd;
    std::string working_dir;
    std::string output;
    std::string image_path;
    std::string id;
    std::string gamepad;
    // When present, this app should be launched via Playnite instead of direct cmd.
    std::string playnite_id;
    // When true, launch Playnite in fullscreen mode via the helper.
    bool playnite_fullscreen;
    bool frame_gen_limiter_fix;
    bool elevated;
    bool virtual_screen {false};
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
    bool lossless_scaling_framegen;
    std::string frame_generation_provider {"lossless-scaling"};
    std::optional<double> lossless_scaling_target_fps;
    std::optional<int> lossless_scaling_rtss_limit;
    std::string lossless_scaling_profile {"custom"};
    lossless_scaling_profile_overrides_t lossless_scaling_recommended;
    lossless_scaling_profile_overrides_t lossless_scaling_custom;
    std::optional<config::video_t::dd_t::config_option_e> dd_config_option_override;

    // Per-application overrides for global config keys (raw config-file value representation).
    // These are applied at runtime and are not persisted to the global config file.
    std::unordered_map<std::string, std::string> config_overrides;
  };

  class proc_t {
  public:
    proc_t() = default;
    proc_t(proc_t &&other) noexcept;
    proc_t &operator=(proc_t &&other) noexcept;

    std::string display_name;
    std::string initial_display;
    bool virtual_display = false;
    bool allow_client_commands = false;

    proc_t(
      bp::environment &&env,
      std::vector<ctx_t> &&apps
    ):
        _env(std::move(env)),
        _apps(std::move(apps)) {
    }

    void launch_input_only();

    int execute(const ctx_t &_app, std::shared_ptr<rtsp_stream::launch_session_t> launch_session);

    /**
     * @return `_app_id` if a process is running, otherwise returns `0`
     */
    int running();

    ~proc_t();

    // Return a snapshot copy to avoid concurrent access races
    active_session_guard_t active_session_guard() const;
    std::vector<ctx_t> get_apps() const;
    std::string get_app_image(int app_id);
    std::string get_last_run_app_name();
    std::string get_running_app_uuid();
    bp::environment get_env();
    void resume();
    void pause();
    void terminate(bool immediate = false, bool needs_refresh = true);
    bool last_run_app_frame_gen_limiter_fix() const;

    // Hot-update app list and environment without disrupting a running app
    void update_apps(std::vector<ctx_t> &&apps, bp::environment &&env);

    // Helpers for parse/refresh to extract newly parsed state without exposing internals
    std::vector<ctx_t> release_apps();
    bp::environment release_env();

  private:
    int _app_id = 0;
    std::string _app_name;

    bp::environment _env;
    std::shared_ptr<rtsp_stream::launch_session_t> _launch_session;
    std::shared_ptr<config::input_t> _saved_input_config;
    std::vector<ctx_t> _apps;
    ctx_t _app;
    std::chrono::steady_clock::time_point _app_launch_time;
    std::string _active_client_uuid;

    mutable std::mutex _apps_mutex;

    // If no command associated with _app_id, yet it's still running
    bool placebo {};

    bp::child _process;
    bp::group _process_group;

#ifdef _WIN32
    GUID _virtual_display_guid {};
    bool _virtual_display_active {false};
#endif

    file_t _pipe;
    std::vector<cmd_t>::const_iterator _app_prep_it;
    std::vector<cmd_t>::const_iterator _app_prep_begin;

#ifdef _WIN32
    void start_lossless_scaling_support(std::unordered_set<DWORD> baseline_pids, const playnite_launcher::lossless::lossless_scaling_app_metadata &metadata, std::string install_dir_hint_utf8, DWORD root_pid);
    void stop_lossless_scaling_support();

    std::thread _lossless_thread;
    std::atomic_bool _lossless_stop_requested {false};
    std::mutex _lossless_mutex;
    bool _lossless_profile_applied {false};
    playnite_launcher::lossless::lossless_scaling_profile_backup _lossless_backup {};
    std::string _lossless_last_install_dir;
    std::string _lossless_last_exe_path;
#endif
  };

  boost::filesystem::path
    find_working_directory(const std::string &cmd, const bp::environment &env);

  /**
   * @brief Calculate a stable id based on name and image data
   * @return Tuple of id calculated without index (for use if no collision) and one with.
   */
  std::tuple<std::string, std::string> calculate_app_id(const std::string &app_name, std::string app_image_path, int index);

  std::string validate_app_image_path(std::string app_image_path);
  void refresh(const std::string &file_name, bool needs_terminate = true);
  void migrate_apps(nlohmann::json *fileTree_p, nlohmann::json *inputTree_p);
  std::optional<proc::proc_t> parse(const std::string &file_name);

  /**
   * @brief Initialize proc functions
   * @return Unique pointer to `deinit_t` to manage cleanup
   */
  std::unique_ptr<platf::deinit_t> init();

  /**
   * @brief Terminates all child processes in a process group.
   * @param proc The child process itself.
   * @param group The group of all children in the process tree.
   * @param exit_timeout The timeout to wait for the process group to gracefully exit.
   */
  void terminate_process_group(bp::child &proc, bp::group &group, std::chrono::seconds exit_timeout);

  extern proc_t proc;

  extern int input_only_app_id;
  extern std::string input_only_app_id_str;
  extern int terminate_app_id;
  extern std::string terminate_app_id_str;
}  // namespace proc
