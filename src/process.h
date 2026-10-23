/**
 * @file src/process.h
 * @brief Declarations for the startup and shutdown of the apps started by a streaming Session.
 */
#pragma once
#include "managed_app_focus.h"
#include "process_catalog.h"

#ifndef __kernel_entry
  #define __kernel_entry
#endif

// standard includes
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// lib includes
#include "boost_process_shim.h"
#include <nlohmann/json.hpp>

// local includes
#include "config.h"
#include "platform/common.h"
#include "rtsp.h"
#include "steam_process_tracker.h"
#include "utility.h"

#ifdef _WIN32
  #include "platform/windows/virtual_display.h"
  #include "tools/playnite_launcher/lossless_scaling.h"
  #include "platform/windows/dualsense_usbip_gamepad.h"

namespace VDISPLAY {
  enum class DRIVER_STATUS;
  enum class DRIVER_SELECTION;
}

#endif

#include "legacy_generated_app_identities.h"

namespace bp = boost_process_shim;
#define DEFAULT_APP_IMAGE_PATH SUNSHINE_ASSETS_DIR "/box.png"

namespace proc {
  using file_t = util::safe_ptr_v2<FILE, int, fclose>;

#ifdef _WIN32
  extern std::atomic<VDISPLAY::DRIVER_STATUS> vDisplayDriverStatus;
  extern std::atomic<VDISPLAY::DRIVER_SELECTION> vDisplayDriverSelection;
  struct vdisplay_driver_status_snapshot_t {
    VDISPLAY::DRIVER_STATUS status;
    VDISPLAY::DRIVER_SELECTION selection;
  };
  vdisplay_driver_status_snapshot_t vDisplayDriverStatusSnapshot();
  void setVDisplayDriverStatus(VDISPLAY::DRIVER_STATUS status);
  void setVDisplayDriverStatus(VDISPLAY::DRIVER_STATUS status, VDISPLAY::DRIVER_SELECTION selection);
  void initVDisplayDriver();
#endif

  struct active_session_guard_t {
    bool has_active_app {false};
    bool uses_playnite {false};
    std::string playnite_id;
    std::string client_uuid;
    std::uint64_t normal_vdd_identity_token {0};
    std::chrono::steady_clock::time_point launch_started_at {};
  };

#ifdef _WIN32
  struct running_app_state_t {
    bool has_active_app {false};
    bool trackable {false};
    bool uses_playnite {false};
    std::string playnite_id;
    std::string name;
    std::string command;
    std::string working_dir;
    uint32_t root_pid {0};
  };
#endif

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

    // Callers holding the configuration read gate must defer cleanup writes.
    // The caller owns stream lifecycle serialization. Startup returns 503 before
    // mutation when terminal admission is closed; an admitted execute stays counted.
    int execute(const ctx_t &_app, std::shared_ptr<rtsp_stream::launch_session_t> launch_session, bool config_read_gate_held = false);

    /**
     * @return `_app_id` if a process is running, otherwise returns `0`
     */
    int running();

    /**
     * @return A side-effect-free snapshot of the current application ID.
     */
    int current_app_id() const;

    ~proc_t();

    // Return a snapshot copy to avoid concurrent access races
    active_session_guard_t active_session_guard() const;
    std::vector<ctx_t> get_apps() const;
    std::string get_app_image(int app_id);
    std::optional<ctx_t> resolve_app(const std::string &appid, const std::string &appuuid = "") const;
    std::optional<ctx_t> resolve_app(int app_id) const;
    std::optional<ctx_t> active_app_snapshot() const;
    std::string get_last_run_app_name();
    std::string get_running_app_uuid();
    bp::environment get_env();
    void resume();
    void pause(bool stream_lifecycle_lock_held = false);
    void terminate(
      bool immediate = false,
      bool needs_refresh = true,
      bool skip_display_revert = false,
      bool stream_lifecycle_lock_held = false,
      bool config_read_gate_held = false
    );
    bool last_run_app_frame_gen_limiter_fix() const;
    bool is_launch_deferred() const;
    bool has_trackable_running_app() const;
    bool foreground_window_matches_running_app();
#ifdef _WIN32
    running_app_state_t running_app_state() const;
    bool running_app_contains_pid(uint32_t pid);
#endif

    // Hot-update app list and environment without disrupting a running app
    void update_apps(std::vector<ctx_t> &&apps, bp::environment &&env);
#if defined(_WIN32) || defined(__linux__)
    bool update_active_app_live_rtx_hdr_overrides(const std::string &app_uuid);
    bool update_active_app_live_rtx_hdr_overrides(
      const std::string &app_uuid,
      const std::unordered_map<std::string, std::string> &rtx_hdr_overrides
    );
#endif

    // Helpers for parse/refresh to extract newly parsed state without exposing internals
    std::vector<ctx_t> release_apps();
    bp::environment release_env();

  private:
    friend void refresh(const std::string &file_name, bool needs_terminate);
    catalog::state_view_t catalog_state();
    int launch_app_commands(bool stream_lifecycle_lock_held, bool config_read_gate_held = false);

    std::atomic<int> _app_id {0};
    std::string _app_name;

    bp::environment _env;
    // Preserve the active undo environment until teardown finishes.
    std::optional<bp::environment> _pending_catalog_environment;
    std::shared_ptr<rtsp_stream::launch_session_t> _launch_session;
    std::shared_ptr<config::input_t> _saved_input_config;
    // Entries inserted by a prior stream must not leak into a later launch,
    // while values loaded from apps.json remain explicit user configuration.
    std::unordered_set<std::string> _stream_owned_environment_keys;
    std::vector<ctx_t> _apps;
    ctx_t _app;
    std::chrono::steady_clock::time_point _app_launch_time;
    std::string _active_client_uuid;
    std::uint64_t _active_client_vdd_identity_token {0};

    mutable std::mutex _apps_mutex;

    // If no command associated with _app_id, yet it's still running
    bool placebo {};

    std::unique_ptr<managed_app_focus::session> _app_focus;
    platf::steam::lifecycle::tracker _steam_tracker;
    std::shared_ptr<platf::steam::lifecycle::process_controller> _steam_process_controller;
    bool _steam_tracking_active {false};
    bool _steam_tracking_associated {false};
    std::chrono::steady_clock::time_point _steam_tracking_deadline {};
    std::chrono::steady_clock::time_point _steam_last_tracking_poll {};

#ifdef _WIN32
    std::atomic_bool _deferred_launch {false};
    std::chrono::steady_clock::time_point _dualsense_ready_deadline {};
    bool _lossless_should_start_support {false};
    playnite_launcher::lossless::lossless_scaling_app_metadata _lossless_metadata {};
#endif

    bp::child _process;
    bp::group _process_group;

#ifdef _WIN32
    std::shared_ptr<platf::dualsense_usbip_gamepad::session_scope> _dualsense_usbip_session;
    GUID _virtual_display_guid {};
    bool _virtual_display_active {false};
    std::optional<config::runtime_output_override_lease_t> _runtime_output_override_lease;
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
   * @brief Calculate a stable id based on UUID when present, otherwise legacy name and image data.
   * @return Tuple of id calculated without index (for use if no collision) and one with.
   */
  std::tuple<std::string, std::string> calculate_app_id(const std::string &app_name, const std::string &app_uuid, std::string app_image_path, int index);

  bool check_valid_png(const std::filesystem::path &path);
  std::string validate_app_image_path(std::string app_image_path);
  std::optional<std::string> read_validated_app_image(const std::string &validated_path);
  std::optional<std::string> read_configured_app_image(const std::string &image_path);
  std::string configured_app_image_version(const ctx_t &app);
  std::string calculate_app_cover_fingerprint(std::string app_image_path);
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

#ifdef _WIN32
  /** Mark an app-triggered display revert to run after the final stream ends. */
  void defer_display_revert();

  /** Consume a previously deferred app-triggered display revert exactly once. */
  bool consume_deferred_display_revert();

  /** Discard a deferred revert when a replacement app takes ownership of the display. */
  void clear_deferred_display_revert();
#endif

  extern proc_t proc;

  extern int input_only_app_id;
  extern std::string input_only_app_id_str;
  extern int terminate_app_id;
  extern std::string terminate_app_id_str;
}  // namespace proc
