/**
 * @file src/config.h
 * @brief Declarations for the configuration of Sunshine.
 */
#pragma once

// standard includes
#include <array>
#include <bitset>
#include <chrono>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

// local includes
#include "nvenc/nvenc_config.h"

namespace config {
  constexpr int SUNSHINE_VIRTUAL_DISPLAY_MAX_PERMANENT_COUNT = 4;

  // track modified config options
  inline std::unordered_map<std::string, std::string> modified_config_settings;
  // when a stream is active, we defer some settings until all sessions end
  inline std::unordered_map<std::string, std::string> pending_config_settings;

  inline constexpr std::array redacted_config = {
    "csrf_allowed_origins"
  };

  void log_config_settings(const std::unordered_map<std::string, std::string> &vars, bool save);

  struct video_t {
    enum class virtual_display_mode_e {
      disabled,  ///< Use physical display (output_name)
      per_client,  ///< Create unique virtual display per client
      shared  ///< Use single shared virtual display for all clients
    };

    enum class virtual_display_layout_e {
      exclusive,  ///< Deactivate all other displays (only the virtual display stays visible)
      extended,  ///< Keep other displays active and extend the desktop with the virtual display
      extended_primary,  ///< Extend the desktop and force the virtual display to be primary
      extended_isolated,  ///< Extend the desktop and move the virtual display far away from other monitors
      extended_primary_isolated  ///< Extend the desktop, force the virtual display to be primary, and isolate it
    };

    bool limit_framerate;
    // Config key: dd_wa_virtual_double_refresh (legacy alias: double_refreshrate)
    bool double_refreshrate;

    // ffmpeg params
    int qp;  // higher == more compression and less quality

    int hevc_mode;
    int av1_mode;
    bool prefer_10bit_sdr;

    int min_threads;  // Minimum number of threads/slices for CPU encoding

    struct {
      std::string sw_preset;
      std::string sw_tune;
      std::optional<int> svtav1_preset;
    } sw;

    nvenc::nvenc_config nv;
    bool nv_realtime_hags;
    bool nv_opengl_vulkan_on_dxgi;
    bool nv_sunshine_high_power_mode;

    struct {
      int preset;
      int multipass;
      int h264_coder;
      int aq;
      int vbv_percentage_increase;
    } nv_legacy;

    struct {
      std::optional<int> qsv_preset;
      std::optional<int> qsv_cavlc;
      bool qsv_slow_hevc;
    } qsv;

    struct {
      std::optional<int> amd_usage_h264;
      std::optional<int> amd_usage_hevc;
      std::optional<int> amd_usage_av1;
      std::optional<int> amd_rc_h264;
      std::optional<int> amd_rc_hevc;
      std::optional<int> amd_rc_av1;
      std::optional<int> amd_enforce_hrd;
      std::optional<int> amd_quality_h264;
      std::optional<int> amd_quality_hevc;
      std::optional<int> amd_quality_av1;
      std::optional<int> amd_preanalysis;
      std::optional<int> amd_vbaq;
      int amd_coder;
    } amd;

    struct {
      int vt_allow_sw;
      int vt_require_sw;
      int vt_realtime;
      int vt_coder;
    } vt;

    struct {
      bool strict_rc_buffer;
    } vaapi;

    struct {
      int tune;  // 0=default, 1=hq, 2=ll, 3=ull, 4=lossless
      int rc_mode;  // 0=driver, 1=cqp, 2=cbr, 4=vbr
    } vk;

    // NVIDIA TrueHDR (RTX HDR) SDR->HDR synthesis. Conversion is opt-in per app via
    // runtime overrides. Global values are retained as tuning defaults, and dials mirror
    // the NVIDIA App RTX HDR overlay. Per-app overrides live in apps.json (see proc::ctx_t).
    struct rtx_hdr_t {
      bool enabled;  ///< Legacy global flag; only runtime app overrides can activate conversion.
      bool force_sdr;  ///< Legacy compatibility setting; app-enabled RTX HDR always forces SDR source.
      int contrast;  ///< -100..100 (overlay "Contrast", default 0 = neutral)
      int saturation;  ///< -100..100 (overlay "Saturation", default 0 = neutral)
      int middle_gray;  ///< 10..100 (overlay "Middle Gray", default 50)
      int peak_brightness;  ///< 400..2000 nits (overlay "Peak Brightness", default 1000)
    } rtx_hdr;

    std::string capture;
    std::string encoder;
    std::string adapter_name;
    std::string output_name;

    virtual_display_mode_e virtual_display_mode;
    virtual_display_layout_e virtual_display_layout;

    struct dd_t {
      struct workarounds_t {
        bool dummy_plug_hdr10;  ///< Force 30 Hz and HDR for physical dummy plugs (requires VSYNC override).
        bool virtual_double_refresh;  ///< Request double refresh on virtual displays to avoid unexpected FPS drops.
      };

      enum class config_option_e {
        disabled,  ///< Disable the configuration for the device.
        verify_only,  ///< @seealso{display_device::SingleDisplayConfiguration::DevicePreparation}
        ensure_active,  ///< @seealso{display_device::SingleDisplayConfiguration::DevicePreparation}
        ensure_primary,  ///< @seealso{display_device::SingleDisplayConfiguration::DevicePreparation}
        ensure_only_display  ///< @seealso{display_device::SingleDisplayConfiguration::DevicePreparation}
      };

      enum class resolution_option_e {
        disabled,  ///< Do not change resolution.
        automatic,  ///< Change resolution and use the one received from Moonlight.
        manual  ///< Change resolution and use the manually provided one.
      };

      enum class refresh_rate_option_e {
        disabled,  ///< Do not change refresh rate.
        automatic,  ///< Change refresh rate and use the one received from Moonlight.
        manual,  ///< Change refresh rate and use the manually provided one.
        prefer_highest  ///< Prefer the highest available refresh rate for the selected resolution.
      };

      enum class hdr_option_e {
        disabled,  ///< Do not change HDR settings.
        automatic  ///< Change HDR settings and use the state requested by Moonlight.
      };

      enum class hdr_request_override_e {
        automatic,  ///< Use HDR state requested by the client.
        force_on,  ///< Force HDR enabled for the session.
        force_off  ///< Force HDR disabled for the session.
      };

      struct mode_remapping_entry_t {
        std::string requested_resolution;
        std::string requested_fps;
        std::string final_resolution;
        std::string final_refresh_rate;
      };

      struct mode_remapping_t {
        std::vector<mode_remapping_entry_t> mixed;  ///< To be used when `resolution_option` and `refresh_rate_option` is set to `automatic`.
        std::vector<mode_remapping_entry_t> resolution_only;  ///< To be use when only `resolution_option` is set to `automatic`.
        std::vector<mode_remapping_entry_t> refresh_rate_only;  ///< To be use when only `refresh_rate_option` is set to `automatic`.
      };

      config_option_e configuration_option;
      resolution_option_e resolution_option;
      std::string manual_resolution;  ///< Manual resolution in case `resolution_option == resolution_option_e::manual`.
      refresh_rate_option_e refresh_rate_option;
      std::string manual_refresh_rate;  ///< Manual refresh rate in case `refresh_rate_option == refresh_rate_option_e::manual`.
      hdr_option_e hdr_option;
      hdr_request_override_e hdr_request_override;
      std::chrono::milliseconds config_revert_delay;  ///< Time to wait until settings are reverted (after stream ends/app exists).
      bool config_revert_on_disconnect;  ///< Specify whether to revert display configuration on client disconnect.
      int paused_virtual_display_timeout_secs;  ///< Optional delay before virtual display cleanup while stream is paused (0 keeps alive).
      enum class helper_engine_e : int {
        automatic,  ///< v2 engine on pre-release builds, legacy engine on stable releases.
        v2,  ///< Always use the v2 state-machine engine.
        legacy  ///< Always use the legacy engine.
      };

      bool always_restore_from_golden;  ///< When true, prefer golden snapshot over session snapshots during restore (reduces stuck virtual screens).
      helper_engine_e display_helper_engine;  ///< Which display helper engine to run.
      int snapshot_restore_hotkey;  ///< Virtual-key code for restore hotkey (0 disables).
      std::uint32_t snapshot_restore_hotkey_modifiers;  ///< Modifier flags for the restore hotkey.
      bool use_sunshine_virtual_display_driver;  ///< Opt into the Vibepollo Display Driver instead of legacy virtual display drivers.
      bool activate_virtual_display;  ///< Auto-activate Sunshine virtual display when selected as the target output.
      int virtual_display_permanent_count;  ///< Number of always-present Sunshine virtual displays to request when explicitly configured.
      bool virtual_display_permanent_count_configured;  ///< False preserves installs that predate this setting.
      std::vector<std::string> snapshot_exclude_devices;  ///< Device IDs to skip when saving display snapshots.
      mode_remapping_t mode_remapping;
      workarounds_t wa;
    } dd;

    int max_bitrate;  // Maximum bitrate, sets ceiling in kbps for bitrate requested from client
    double minimum_fps_target;  ///< Lowest framerate that will be used when streaming. Range 0-1000, 0 = half of client's requested framerate.

    std::string fallback_mode;
    bool ignore_encoder_probe_failure;
  };

  struct audio_t {
    std::string sink;
    std::string virtual_sink;
    bool stream;
    bool install_steam_drivers;
    bool keep_default;
    bool auto_capture;
  };

  constexpr int ENCRYPTION_MODE_NEVER = 0;  // Never use video encryption, even if the client supports it
  constexpr int ENCRYPTION_MODE_OPPORTUNISTIC = 1;  // Use video encryption if available, but stream without it if not supported
  constexpr int ENCRYPTION_MODE_MANDATORY = 2;  // Always use video encryption and refuse clients that can't encrypt

  struct stream_t {
    std::chrono::milliseconds ping_timeout;

    std::string file_apps;

    int fec_percentage;
    int video_max_batch_size_kb;

    // Video encryption settings for LAN and WAN streams
    int lan_encryption_mode;
    int wan_encryption_mode;
  };

  struct nvhttp_t {
    // Could be any of the following values:
    // pc|lan|wan
    std::string origin_web_ui_allowed;

    std::string pkey;
    std::string cert;

    std::string sunshine_name;

    std::string file_state;
    std::string vibeshine_file_state;

    std::string external_ip;
  };

  struct input_t {
    std::unordered_map<int, int> keybindings;

    std::chrono::milliseconds back_button_timeout;
    std::chrono::milliseconds key_repeat_delay;
    std::chrono::duration<double> key_repeat_period;

    std::string gamepad;
    bool ds4_back_as_touchpad_click;
    bool motion_as_ds4;
    bool touchpad_as_ds4;
    // When forcing DS5 emulation via Inputtino, randomize the virtual controller MAC
    // to avoid client-side config mixing when controllers are swapped.
    bool ds5_inputtino_randomize_mac;

    bool keyboard;
    bool mouse;
    bool controller;

    bool always_send_scancodes;

    bool high_resolution_scrolling;
    bool native_pen_touch;

    bool enable_input_only_mode;
    bool forward_rumble;
  };

  struct frame_limiter_t {
    bool enable {false};

    // Provider selector. Supported values: "auto", "nvidia-control-panel", "rtss".
    std::string provider;

    // Optional FPS limit override. 0 uses the stream's requested FPS.
    int fps_limit {0};

    // When enabled, Sunshine forces the NVIDIA driver VSYNC setting to Off during streams when available.
    // When NVIDIA overrides are unavailable, the display helper falls back to the highest refresh rate instead.
    // Restores the previous VSYNC state when streaming stops.
    bool disable_vsync {false};
  };

  // Windows-only: RTSS integration settings
  struct rtss_t {
    // RTSS install path. If empty, defaults to "%PROGRAMFILES%/RivaTuner Statistics Server"
    std::string install_path;

    // SyncLimiter mode. One of: "async", "front edge sync", "back edge sync", "nvidia reflex".
    // If empty or unrecognized, SyncLimiter is not modified.
    std::string frame_limit_type;
  };

  struct lossless_scaling_t {
    std::string exe_path;
    bool legacy_auto_detect {false};
  };

  namespace flag {
    enum flag_e : std::size_t {
      PIN_STDIN = 0,  ///< Read PIN from stdin instead of http
      FRESH_STATE,  ///< Do not load or save state
      FORCE_VIDEO_HEADER_REPLACE,  ///< force replacing headers inside video data
      UPNP,  ///< Try Universal Plug 'n Play
      CONST_PIN,  ///< Use "universal" pin
      FLAG_SIZE  ///< Number of flags
    };
  }  // namespace flag

  struct prep_cmd_t {
    prep_cmd_t(std::string &&do_cmd, std::string &&undo_cmd, bool &&elevated):
        do_cmd(std::move(do_cmd)),
        undo_cmd(std::move(undo_cmd)),
        elevated(std::move(elevated)) {
    }

    explicit prep_cmd_t(std::string &&do_cmd, bool &&elevated):
        do_cmd(std::move(do_cmd)),
        elevated(std::move(elevated)) {
    }

    std::string do_cmd;
    std::string undo_cmd;
    bool elevated;
  };

  struct server_cmd_t {
    server_cmd_t(std::string &&cmd_name, std::string &&cmd_val, bool &&elevated):
        cmd_name(std::move(cmd_name)),
        cmd_val(std::move(cmd_val)),
        elevated(std::move(elevated)) {
    }

    std::string cmd_name;
    std::string cmd_val;
    bool elevated;
  };

  struct sunshine_t {
    bool hide_tray_controls;
    bool enable_pairing;
    bool enable_discovery;
    bool envvar_compatibility_mode;
    std::string locale;
    int min_log_level;
    std::bitset<flag::FLAG_SIZE> flags;
    std::string credentials_file;

    std::string username;
    std::string password;
    std::string salt;

    std::string config_file;

    struct cmd_t {
      std::string name;
      int argc;
      char **argv;
    } cmd;

    std::uint16_t port;
    std::string address_family;
    std::string bind_address;

    std::string log_file;
    bool notify_pre_releases;
    bool legacy_ordering;
    bool system_tray;
    std::vector<prep_cmd_t> prep_cmds;
    std::vector<prep_cmd_t> state_cmds;
    std::vector<server_cmd_t> server_cmds;
    std::chrono::seconds session_token_ttl;  ///< Session token time-to-live (seconds)
    std::chrono::seconds remember_me_refresh_token_ttl;  ///< Trusted device (remember-me) refresh TTL
    // Interval in seconds between automatic update checks (0 disables periodic checks)
    int update_check_interval_seconds {86400};
    bool session_history_enabled {true};  ///< Persist stream/session history to SQLite
    int session_history_ttl_days {0};  ///< Delete ended sessions older than this many days (0 disables age pruning)
    int session_history_db_size_limit_mb {0};  ///< Approximate live DB quota in MiB (0 disables size pruning)
    std::vector<std::string> csrf_allowed_origins;
    bool realtime_stats_enabled {true};  ///< Sample live host stats (CPU/GPU/RAM/VRAM) for the web UI
    int realtime_stats_poll_interval_ms {2000};  ///< Host stats sampler interval in milliseconds
  };

  extern video_t video;
  extern audio_t audio;
  extern stream_t stream;
  extern nvhttp_t nvhttp;
  extern input_t input;
  extern frame_limiter_t frame_limiter;
  extern rtss_t rtss;
  extern lossless_scaling_t lossless_scaling;
  extern sunshine_t sunshine;

  int parse(int argc, char *argv[]);
  std::unordered_map<std::string, std::string> parse_config(const std::string_view &file_content);

  // Hot-reload helpers
  void apply_config_now();
  void mark_deferred_reload();
  void maybe_apply_deferred();

  // Gate helpers so session start/resume can hold a shared lock while apply holds a unique lock.
  std::shared_lock<std::shared_mutex> acquire_apply_read_gate();

  // Runtime, non-persisted config overrides (e.g. per-application overrides).
  // Values use the same raw representation as the config file (strings for string keys,
  // JSON dumps for non-string keys).
  void set_runtime_config_overrides(std::unordered_map<std::string, std::string> overrides);
  void clear_runtime_config_overrides();
  std::unordered_map<std::string, std::string> runtime_config_overrides_snapshot();
  bool has_runtime_config_override(std::string_view key);
  bool has_runtime_config_overrides();

  void set_runtime_output_name_override(std::optional<std::string> output_name);
  std::optional<std::string> runtime_output_name_override();
  std::string get_active_output_name();
}  // namespace config
