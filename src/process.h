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
#include <unordered_map>
#include <unordered_set>
#include <thread>

// lib includes
#include <boost/process/v1.hpp>

// local includes
#include "config.h"
#include "platform/common.h"
#include "rtsp.h"
#include "utility.h"

#ifdef _WIN32
  #include "tools/playnite_launcher/lossless_scaling.h"

  namespace VDISPLAY {
    enum class DRIVER_STATUS;
  }

#endif

namespace proc {
  using file_t = util::safe_ptr_v2<FILE, int, fclose>;

#ifdef _WIN32
  extern VDISPLAY::DRIVER_STATUS vDisplayDriverStatus;
  void initVDisplayDriver();
#endif

  typedef config::prep_cmd_t cmd_t;

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

    std::string name;
    std::string cmd;
    std::string working_dir;
    std::string output;
    std::string image_path;
    std::string id;
    // When present, this app should be launched via Playnite instead of direct cmd.
    std::string playnite_id;
    // When true, launch Playnite in fullscreen mode via the helper.
    bool playnite_fullscreen;
    bool frame_gen_limiter_fix;
    bool elevated;
    bool virtual_screen {false};
    bool auto_detach;
    bool wait_all;
    std::chrono::seconds exit_timeout;
    bool gen1_framegen_fix;
    bool gen2_framegen_fix;
    bool lossless_scaling_framegen;
    std::string frame_generation_provider {"lossless-scaling"};
    std::optional<int> lossless_scaling_target_fps;
    std::optional<int> lossless_scaling_rtss_limit;
    std::string lossless_scaling_profile {"custom"};
    lossless_scaling_profile_overrides_t lossless_scaling_recommended;
    lossless_scaling_profile_overrides_t lossless_scaling_custom;
  };

  class proc_t {
  public:
    proc_t() = default;
    proc_t(proc_t &&other) noexcept;
    proc_t &operator=(proc_t &&other) noexcept;

    proc_t(
      boost::process::v1::environment &&env,
      std::vector<ctx_t> &&apps
    ):
        _app_id(0),
        _env(std::move(env)),
        _apps(std::move(apps)) {
    }

    int execute(int app_id, std::shared_ptr<rtsp_stream::launch_session_t> launch_session);

    /**
     * @return `_app_id` if a process is running, otherwise returns `0`
     */
    int running();

    ~proc_t();

    // Return a snapshot copy to avoid concurrent access races
    std::vector<ctx_t> get_apps() const;
    std::string get_app_image(int app_id);
    std::string get_last_run_app_name();
    bool last_run_app_frame_gen_limiter_fix() const;
    void terminate();

    // Hot-update app list and environment without disrupting a running app
    void update_apps(std::vector<ctx_t> &&apps, boost::process::v1::environment &&env);

    // Helpers for parse/refresh to extract newly parsed state without exposing internals
    std::vector<ctx_t> release_apps();
    boost::process::v1::environment release_env();

  private:
    int _app_id;

    boost::process::v1::environment _env;
    std::vector<ctx_t> _apps;
    ctx_t _app;
    std::chrono::steady_clock::time_point _app_launch_time;

    mutable std::mutex _apps_mutex;

    // If no command associated with _app_id, yet it's still running
    bool placebo {};

    boost::process::v1::child _process;
    boost::process::v1::group _process_group;

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

  /**
   * @brief Calculate a stable id based on name and image data
   * @return Tuple of id calculated without index (for use if no collision) and one with.
   */
  std::tuple<std::string, std::string> calculate_app_id(const std::string &app_name, std::string app_image_path, int index);

  std::string validate_app_image_path(std::string app_image_path);
  void refresh(const std::string &file_name);
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
  void terminate_process_group(boost::process::v1::child &proc, boost::process::v1::group &group, std::chrono::seconds exit_timeout);

  extern proc_t proc;
}  // namespace proc
