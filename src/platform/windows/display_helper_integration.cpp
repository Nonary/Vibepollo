/**
 * @file src/platform/windows/display_helper_integration.cpp
 */
#ifdef _WIN32

  #include <winsock2.h>

  // standard
  #include <algorithm>
  #include <filesystem>
  #include <mutex>
  #include <optional>
  #include <string>
  #include <thread>

  // libdisplaydevice
  #include <display_device/json.h>
  #include <display_device/windows/win_api_layer.h>
  #include <display_device/windows/win_display_device.h>
  #include <nlohmann/json.hpp>

  // sunshine
  #include "display_helper_integration.h"
  #include "src/display_device.h"  // For configuration parsing only
  #include "src/globals.h"
  #include "src/logging.h"
  #include "src/platform/windows/frame_limiter_nvcp.h"
  #include "src/platform/windows/ipc/display_settings_client.h"
  #include "src/platform/windows/ipc/process_handler.h"
  #include "src/platform/windows/misc.h"
  #include "src/platform/windows/virtual_display.h"
  #include "src/process.h"

namespace {
  // Serialize helper start/inspect to avoid races that could spawn duplicate helpers
  std::mutex &helper_mutex() {
    static std::mutex m;
    return m;
  }

  // Persistent process handler to keep helper alive while Sunshine runs
  ProcessHandler &helper_proc() {
    static ProcessHandler h(/*use_job=*/false);
    return h;
  }

  bool session_targets_desktop(const rtsp_stream::launch_session_t &session) {
    const auto apps = proc::proc.get_apps();
    if (apps.empty()) {
      return false;
    }

    const auto app_id = std::to_string(session.appid);
    const auto it = std::find_if(apps.begin(), apps.end(), [&](const proc::ctx_t &app) {
      return app.id == app_id;
    });

    if (it == apps.end()) {
      return session.appid <= 0;
    }

    return it->cmd.empty() && it->playnite_id.empty();
  }

  bool dd_feature_enabled() {
    using config_option_e = config::video_t::dd_t::config_option_e;
    if (config::video.dd.configuration_option != config_option_e::disabled) {
      return true;
    }

    const bool virtual_display_selected = config::video.output_name == VDISPLAY::SUDOVDA_VIRTUAL_DISPLAY_SELECTION;
    return virtual_display_selected && config::video.dd.activate_virtual_display;
  }

  bool shutdown_requested() {
    if (!mail::man) {
      return false;
    }
    try {
      auto shutdown_event = mail::man->event<bool>(mail::shutdown);
      return shutdown_event && shutdown_event->peek();
    } catch (...) {
      return false;
    }
  }

  bool ensure_helper_started() {
    if (!dd_feature_enabled()) {
      return false;
    }
    const bool shutting_down = shutdown_requested();
    std::lock_guard<std::mutex> lg(helper_mutex());
    // Already started? Verify liveness to avoid stale or wedged state
    if (HANDLE h = helper_proc().get_process_handle(); h != nullptr) {
      BOOST_LOG(debug) << "Display helper: checking existing process handle...";
      DWORD wait = WaitForSingleObject(h, 0);
      if (wait == WAIT_TIMEOUT) {
        DWORD pid = GetProcessId(h);
        BOOST_LOG(debug) << "Display helper already running (pid=" << pid << ")";
        // Check IPC liveness with a lightweight ping; if unresponsive, restart the helper
        bool ping_ok = false;
        for (int i = 0; i < 2 && !ping_ok; ++i) {
          ping_ok = platf::display_helper_client::send_ping();
          if (!ping_ok) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
          }
        }
        if (ping_ok) {
          return true;
        }
        BOOST_LOG(warning) << "Display helper process is running but IPC ping failed; restarting helper.";
        helper_proc().terminate();
        // Allow a brief moment for termination to complete
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      } else {
        // Process exited; fall through to restart
        DWORD exit_code = 0;
        GetExitCodeProcess(h, &exit_code);
        BOOST_LOG(debug) << "Display helper process detected as exited (code=" << exit_code << "); preparing restart.";
      }
    }
    if (shutting_down) {
      return false;
    }
    // Compute path to sunshine_display_helper.exe inside the tools subdirectory next to Sunshine.exe
    wchar_t module_path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, module_path, _countof(module_path))) {
      BOOST_LOG(error) << "Failed to resolve Sunshine module path; cannot launch display helper.";
      return false;
    }
    std::filesystem::path exe_path(module_path);
    std::filesystem::path dir = exe_path.parent_path();
    std::filesystem::path helper = dir / L"tools" / L"sunshine_display_helper.exe";

    if (!std::filesystem::exists(helper)) {
      BOOST_LOG(warning) << "Display helper not found at: " << platf::to_utf8(helper.wstring())
                         << ". Ensure the tools subdirectory is present and contains sunshine_display_helper.exe.";
      return false;
    }

    BOOST_LOG(debug) << "Starting display helper: " << platf::to_utf8(helper.wstring());
    const bool started = helper_proc().start(helper.wstring(), L"");
    if (!started) {
      BOOST_LOG(error) << "Failed to start display helper: " << platf::to_utf8(helper.wstring());
    } else if (HANDLE h = helper_proc().get_process_handle(); h != nullptr) {
      DWORD pid = GetProcessId(h);
      BOOST_LOG(info) << "Display helper successfully started (pid=" << pid << ")";
    }
    return started;
  }

  // Watchdog state for helper liveness during active streams
  static std::atomic<bool> g_watchdog_running {false};
  static std::jthread g_watchdog_thread;

  // Active session display parameters snapshot for re-apply on reconnect.
  // We do NOT cache serialized JSON, only the subset of session fields that
  // affect display configuration. On reconnect, we rebuild the full
  // SingleDisplayConfiguration from current Sunshine config + these fields.
  struct session_dd_fields_t {
    int width = -1;
    int height = -1;
    int fps = -1;
    bool enable_hdr = false;
    bool enable_sops = false;
    bool virtual_display = false;
    std::string virtual_display_device_id;
    std::optional<int> framegen_refresh_rate;
    bool gen1_framegen_fix = false;
    bool gen2_framegen_fix = false;
  };

  static std::mutex g_session_mutex;
  static std::optional<session_dd_fields_t> g_active_session_dd;

  static void set_active_session(const rtsp_stream::launch_session_t &session, std::optional<std::string> device_id_override = std::nullopt, std::optional<int> fps_override = std::nullopt) {
    std::lock_guard<std::mutex> lg(g_session_mutex);
    const int effective_fps = fps_override ? *fps_override : (session.framegen_refresh_rate && *session.framegen_refresh_rate > 0 ? *session.framegen_refresh_rate : session.fps);
    g_active_session_dd = session_dd_fields_t {
      .width = session.width,
      .height = session.height,
      .fps = effective_fps,
      .enable_hdr = session.enable_hdr,
      .enable_sops = session.enable_sops,
      .virtual_display = session.virtual_display,
      .virtual_display_device_id = device_id_override ? *device_id_override : session.virtual_display_device_id,
      .framegen_refresh_rate = session.framegen_refresh_rate,
      .gen1_framegen_fix = session.gen1_framegen_fix,
      .gen2_framegen_fix = session.gen2_framegen_fix,
    };
  }

  static std::optional<session_dd_fields_t> get_active_session_copy() {
    std::lock_guard<std::mutex> lg(g_session_mutex);
    return g_active_session_dd;
  }

  static void clear_active_session() {
    std::lock_guard<std::mutex> lg(g_session_mutex);
    g_active_session_dd.reset();
  }

  static void watchdog_proc(std::stop_token st) {
    using namespace std::chrono_literals;
    const auto interval = 5s;
    bool helper_ready = false;

    while (!st.stop_requested()) {
      if (!dd_feature_enabled()) {
        if (helper_ready) {
          platf::display_helper_client::reset_connection();
          helper_ready = false;
        }
        for (auto slept = 0ms; slept < interval && !st.stop_requested(); slept += 100ms) {
          std::this_thread::sleep_for(100ms);
        }
        continue;
      }

      if (!helper_ready) {
        helper_ready = ensure_helper_started();
        if (!helper_ready) {
          for (auto slept = 0ms; slept < interval && !st.stop_requested(); slept += 100ms) {
            std::this_thread::sleep_for(100ms);
          }
          continue;
        }
        (void) platf::display_helper_client::send_ping();
      }

      for (auto slept = 0ms; slept < interval && !st.stop_requested(); slept += 100ms) {
        std::this_thread::sleep_for(100ms);
      }
      if (st.stop_requested()) {
        break;
      }

      if (!platf::display_helper_client::send_ping()) {
        // Avoid logging ping failures to reduce log spam; proceed to reconnect
        platf::display_helper_client::reset_connection();
        helper_ready = ensure_helper_started();
        if (!helper_ready) {
          continue;
        }
        // Attempt to re-derive and apply the desired configuration from
        // current Sunshine state if a session is active.
        auto session_opt = get_active_session_copy();
        if (session_opt) {
          auto _hot_apply_gate = config::acquire_apply_read_gate();
          // Rebuild a minimal launch_session_t carrying display-related fields
          rtsp_stream::launch_session_t tmp_session {};
          tmp_session.width = session_opt->width;
          tmp_session.height = session_opt->height;
          tmp_session.fps = session_opt->fps;
          tmp_session.enable_hdr = session_opt->enable_hdr;
          tmp_session.enable_sops = session_opt->enable_sops;
          tmp_session.virtual_display = session_opt->virtual_display;
          tmp_session.virtual_display_device_id = session_opt->virtual_display_device_id;
          tmp_session.framegen_refresh_rate = session_opt->framegen_refresh_rate;
          tmp_session.gen1_framegen_fix = session_opt->gen1_framegen_fix;
          tmp_session.gen2_framegen_fix = session_opt->gen2_framegen_fix;
          bool reapplied = display_helper_integration::apply_from_session(config::video, tmp_session);
          BOOST_LOG(info) << "Display helper watchdog: re-assert APPLY after reconnect result=" << (reapplied ? "true" : "false");
          if (!reapplied) {
            helper_ready = platf::display_helper_client::send_ping();
          }
        } else {
          helper_ready = platf::display_helper_client::send_ping();
        }
      }
    }
  }

}  // namespace

namespace display_helper_integration {
  bool apply_from_session(const config::video_t &video_config, const rtsp_stream::launch_session_t &session) {
    if (!ensure_helper_started()) {
      BOOST_LOG(info) << "Display helper unavailable; cannot send apply.";
      return false;
    }

    // Best-effort liveness probe: if ping fails, reset and try once more
    if (!platf::display_helper_client::send_ping()) {
      BOOST_LOG(warning) << "Display helper: initial ping failed; resetting connection and retrying.";
      platf::display_helper_client::reset_connection();
      (void) platf::display_helper_client::send_ping();
    }

    // Check if virtual display auto-activation is enabled
    const bool is_virtual_display = (video_config.output_name == VDISPLAY::SUDOVDA_VIRTUAL_DISPLAY_SELECTION);
    const int display_fps = session.framegen_refresh_rate && *session.framegen_refresh_rate > 0 ? *session.framegen_refresh_rate : session.fps;
    if (is_virtual_display && video_config.dd.activate_virtual_display) {
      BOOST_LOG(info) << "Display helper: Virtual display detected with auto-activation enabled. Activating virtual display via EnsureOnly mode.";

      // Use parse_configuration to get the properly overridden values
      const auto parsed = display_device::parse_configuration(video_config, session);
      if (const auto *cfg = std::get_if<display_device::SingleDisplayConfiguration>(&parsed)) {
        // Start with the parsed configuration that includes all overrides
        display_device::SingleDisplayConfiguration vd_cfg = *cfg;

        // Override device ID and device prep for virtual display
        std::string target_device_id = session.virtual_display_device_id;
        if (target_device_id.empty()) {
          if (auto resolved = VDISPLAY::resolveAnyVirtualDisplayDeviceId()) {
            target_device_id = *resolved;
          }
        }
        if (target_device_id.empty()) {
          target_device_id = video_config.output_name;
        }
        vd_cfg.m_device_id = target_device_id;
        vd_cfg.m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::EnsureOnlyDisplay;

        std::string json = display_device::toJson(vd_cfg);
        BOOST_LOG(info) << "Display helper: sending APPLY for virtual display activation with configuration:\n"
                        << json;
        const bool ok = platf::display_helper_client::send_apply_json(json);
        BOOST_LOG(info) << "Display helper: Virtual display APPLY dispatch result=" << (ok ? "true" : "false");

        if (ok) {
          // Blacklist the virtual display device_id so it won't be saved in topology snapshots
          BOOST_LOG(info) << "Display helper: blacklisting virtual display device_id from topology exports: " << target_device_id;
          platf::display_helper_client::send_blacklist(target_device_id);

          set_active_session(session, target_device_id, display_fps);
        }
        return ok;
      } else {
        BOOST_LOG(error) << "Display helper: Failed to parse configuration for virtual display.";
        return false;
      }
    }

    const bool dummy_plug_mode = video_config.dd.wa.dummy_plug_hdr10;
    const bool desktop_session = session_targets_desktop(session);
    const bool gen1_framegen_fix = session.gen1_framegen_fix;
    const bool gen2_framegen_fix = session.gen2_framegen_fix;
    bool should_force_refresh = config::rtss.disable_vsync_ullm &&
                                (!platf::has_nvidia_gpu() || !platf::frame_limiter_nvcp::is_available());
    if (gen1_framegen_fix || gen2_framegen_fix) {
      should_force_refresh = true;
    }
    if (dummy_plug_mode && !gen1_framegen_fix && !gen2_framegen_fix) {
      should_force_refresh = false;
    }

    const auto parsed = display_device::parse_configuration(video_config, session);
    if (const auto *cfg = std::get_if<display_device::SingleDisplayConfiguration>(&parsed)) {
      // Copy parsed config so we can optionally override refresh when VSYNC/ULLM suppression is enabled
      auto cfg_effective = *cfg;

      if (dummy_plug_mode && !gen1_framegen_fix && !gen2_framegen_fix && !desktop_session) {
        BOOST_LOG(info) << "Display helper: dummy plug HDR10 mode forcing 30 Hz for non-desktop session.";
        cfg_effective.m_refresh_rate = display_device::Rational {30u, 1u};
        cfg_effective.m_hdr_state = display_device::HdrState::Enabled;
      }
      if (dummy_plug_mode && (gen1_framegen_fix || gen2_framegen_fix) && !desktop_session) {
        BOOST_LOG(info) << "Display helper: Frame generated capture fix overriding dummy plug refresh lock.";
        cfg_effective.m_hdr_state = display_device::HdrState::Enabled;
      }
      if (should_force_refresh) {
        if (gen1_framegen_fix || gen2_framegen_fix) {
          BOOST_LOG(info) << "Display helper: Frame generated capture fix forcing the highest available refresh rate for this session.";
        } else {
          BOOST_LOG(info) << "Display helper: VSYNC/ULLM suppression enabled; forcing the highest available refresh rate for this session. Disable the Sunshine RTSS 'Disable VSYNC/ULLM' option if the refresh change was not intended.";
        }
        cfg_effective.m_refresh_rate = display_device::Rational {10000u, 1u};
        if (!cfg_effective.m_resolution && session.width >= 0 && session.height >= 0) {
          cfg_effective.m_resolution = display_device::Resolution {
            static_cast<unsigned int>(session.width),
            static_cast<unsigned int>(session.height)
          };
        }
      }

      std::string json = display_device::toJson(cfg_effective);
      // Embed helper-only flag for HDR workaround (async; fixed 1s delay)
      try {
        if (video_config.dd.wa.hdr_toggle) {
          // Parse, attach flag, and dump back. Unknown fields are ignored by the helper's typed parser.
          nlohmann::json j = nlohmann::json::parse(json);
          j["wa_hdr_toggle"] = true;
          json = j.dump();
        }
      } catch (...) {
        // Non-fatal: fall back to raw JSON without the extra flag
      }
      BOOST_LOG(info) << "Display helper: sending APPLY with configuration:\n"
                      << json;
      const bool ok = platf::display_helper_client::send_apply_json(json);
      BOOST_LOG(info) << "Display helper: APPLY dispatch result=" << (ok ? "true" : "false");
      if (ok) {
        set_active_session(session, std::nullopt, display_fps);
      }
      return ok;
    }
    if (std::holds_alternative<display_device::configuration_disabled_tag_t>(parsed)) {
      if (dummy_plug_mode && !gen1_framegen_fix && !gen2_framegen_fix && !desktop_session) {
        display_device::SingleDisplayConfiguration cfg_override;
        cfg_override.m_device_id = video_config.output_name;  // optional
        cfg_override.m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::VerifyOnly;
        if (session.width >= 0 && session.height >= 0) {
          cfg_override.m_resolution = display_device::Resolution {
            static_cast<unsigned int>(session.width),
            static_cast<unsigned int>(session.height)
          };
        }
        cfg_override.m_refresh_rate = display_device::Rational {30u, 1u};
        cfg_override.m_hdr_state = display_device::HdrState::Enabled;

        std::string json = display_device::toJson(cfg_override);
        try {
          if (video_config.dd.wa.hdr_toggle) {
            nlohmann::json j = nlohmann::json::parse(json);
            j["wa_hdr_toggle"] = true;
            json = j.dump();
          }
        } catch (...) {
        }
        BOOST_LOG(info) << "Display helper: sending APPLY (dummy plug HDR10) with configuration:\n"
                        << json;
        const bool ok = platf::display_helper_client::send_apply_json(json);
        BOOST_LOG(info) << "Display helper: APPLY dispatch result=" << (ok ? "true" : "false");
        if (ok) {
          set_active_session(session, std::nullopt, display_fps);
        }
        return ok;
      }

      if (dummy_plug_mode && (gen1_framegen_fix || gen2_framegen_fix) && !desktop_session) {
        BOOST_LOG(info) << "Display helper: Frame generated capture fix active; skipping dummy plug HDR 30 Hz override.";
      }

      if (should_force_refresh) {
        if (gen1_framegen_fix || gen2_framegen_fix) {
          BOOST_LOG(info) << "Display helper: Frame generated capture fix forcing the highest available refresh rate for this session.";
        } else {
          BOOST_LOG(info) << "Display helper: VSYNC/ULLM suppression enabled; forcing the highest available refresh rate for this session. Disable the Sunshine RTSS 'Disable VSYNC/ULLM' option if the refresh change was not intended.";
        }
        display_device::SingleDisplayConfiguration cfg_override;
        cfg_override.m_device_id = video_config.output_name;  // optional
        cfg_override.m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::VerifyOnly;
        if (session.width >= 0 && session.height >= 0) {
          cfg_override.m_resolution = display_device::Resolution {
            static_cast<unsigned int>(session.width),
            static_cast<unsigned int>(session.height)
          };
        }
        cfg_override.m_refresh_rate = display_device::Rational {10000u, 1u};

        std::string json = display_device::toJson(cfg_override);
        try {
          if (video_config.dd.wa.hdr_toggle) {
            nlohmann::json j = nlohmann::json::parse(json);
            j["wa_hdr_toggle"] = true;
            json = j.dump();
          }
        } catch (...) {
        }
        BOOST_LOG(info) << "Display helper: sending APPLY (VSYNC/ULLM suppression) with configuration:\n"
                        << json;
        const bool ok = platf::display_helper_client::send_apply_json(json);
        BOOST_LOG(info) << "Display helper: APPLY dispatch result=" << (ok ? "true" : "false");
        if (ok) {
          set_active_session(session, std::nullopt, display_fps);
        }
        return ok;
      }

      // Otherwise, if disabled and no override, request revert so helper can restore
      BOOST_LOG(info) << "Display configuration disabled; requesting REVERT via helper.";
      const bool ok = platf::display_helper_client::send_revert();
      BOOST_LOG(info) << "Display helper: REVERT dispatch result=" << (ok ? "true" : "false");
      return ok;
    }
    // failed_to_parse -> let caller fallback
    BOOST_LOG(info) << "Display helper: configuration parse failed; not dispatching.";
    return false;
  }

  bool revert() {
    if (!ensure_helper_started()) {
      BOOST_LOG(info) << "Display helper unavailable; cannot send revert.";
      return false;
    }
    BOOST_LOG(info) << "Display helper: sending REVERT request.";
    const bool ok = platf::display_helper_client::send_revert();
    BOOST_LOG(info) << "Display helper: REVERT dispatch result=" << (ok ? "true" : "false");
    clear_active_session();
    return ok;
  }

  bool export_golden_restore() {
    if (!ensure_helper_started()) {
      BOOST_LOG(info) << "Display helper unavailable; cannot export golden snapshot.";
      return false;
    }
    BOOST_LOG(info) << "Display helper: sending EXPORT_GOLDEN request.";
    const bool ok = platf::display_helper_client::send_export_golden();
    BOOST_LOG(info) << "Display helper: EXPORT_GOLDEN dispatch result=" << (ok ? "true" : "false");
    return ok;
  }

  bool reset_persistence() {
    if (!ensure_helper_started()) {
      BOOST_LOG(info) << "Display helper unavailable; cannot reset persistence.";
      return false;
    }
    BOOST_LOG(info) << "Display helper: sending RESET request.";
    const bool ok = platf::display_helper_client::send_reset();
    BOOST_LOG(info) << "Display helper: RESET dispatch result=" << (ok ? "true" : "false");
    return ok;
  }

  std::optional<display_device::EnumeratedDeviceList> enumerate_devices() {
    try {
      auto api = std::make_shared<display_device::WinApiLayer>();
      display_device::WinDisplayDevice dd(api);
      return dd.enumAvailableDevices();
    } catch (...) {
      return std::nullopt;
    }
  }

  std::string enumerate_devices_json() {
    auto devices = enumerate_devices();
    if (!devices) {
      return "[]";
    }
    return display_device::toJson(*devices);
  }

  void start_watchdog() {
    if (!dd_feature_enabled()) {
      return;
    }
    if (g_watchdog_running.exchange(true, std::memory_order_acq_rel)) {
      return;  // already running
    }
    g_watchdog_thread = std::jthread(watchdog_proc);
  }

  void stop_watchdog() {
    if (!g_watchdog_running.exchange(false, std::memory_order_acq_rel)) {
      return;  // not running
    }
    if (g_watchdog_thread.joinable()) {
      g_watchdog_thread.request_stop();
      g_watchdog_thread.join();
    }
    if (config::video.dd.config_revert_on_disconnect) {
      platf::display_helper_client::reset_connection();
    }
    clear_active_session();
  }
}  // namespace display_helper_integration

#endif
