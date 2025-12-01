/**
 * @file src/platform/windows/display_helper_integration.cpp
 */
#ifdef _WIN32

  #include <winsock2.h>

  // standard
  #include <algorithm>
  #include <chrono>
  #include <filesystem>
  #include <mutex>
  #include <optional>
  #include <string>
  #include <thread>
  #include <vector>

  #include <boost/algorithm/string/predicate.hpp>

  // libdisplaydevice
  #include <display_device/json.h>
  #include <display_device/windows/win_api_layer.h>
  #include <display_device/windows/win_api_recovery.h>
  #include <display_device/windows/win_display_device.h>
  #include <nlohmann/json.hpp>

  // sunshine
  #include "display_helper_integration.h"
  #include "src/globals.h"
  #include "src/logging.h"
  #include "src/platform/windows/display_helper_coordinator.h"
  #include "src/platform/windows/display_helper_request_helpers.h"
  #include "src/platform/windows/impersonating_display_device.h"
  #include "src/platform/windows/frame_limiter_nvcp.h"
  #include "src/platform/windows/ipc/display_settings_client.h"
  #include "src/platform/windows/ipc/process_handler.h"
  #include "src/platform/windows/misc.h"
  #include "src/platform/windows/virtual_display.h"
  #include "src/process.h"
  #include <tlhelp32.h>
#include <display_device/noop_audio_context.h>
#include <display_device/noop_settings_persistence.h>
#include <display_device/windows/persistent_state.h>
#include <display_device/windows/settings_manager.h>
#include <display_device/windows/types.h>

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

  constexpr std::chrono::seconds kTopologyWaitTimeout {6};
  constexpr DWORD kHelperStopGracePeriodMs = 2000;
  constexpr std::chrono::milliseconds kHelperIpcReadyTimeout {2000};
  constexpr std::chrono::milliseconds kHelperIpcReadyPoll {150};

  bool ensure_helper_started(bool force_restart = false, bool force_enable = false);
  const char *virtual_layout_to_string(const display_helper_integration::VirtualDisplayArrangement layout);

  struct InProcessDisplayContext {
    std::shared_ptr<display_device::SettingsManagerInterface> settings_mgr;
    std::shared_ptr<display_device::WinDisplayDeviceInterface> display;
  };

  std::optional<InProcessDisplayContext> make_settings_manager() {
    try {
      auto api = std::make_shared<display_device::WinApiLayer>();
      auto dd = std::make_shared<display_device::WinDisplayDevice>(api);
      auto impersonated_dd = std::make_shared<display_device::ImpersonatingDisplayDevice>(dd);
      auto audio = std::make_shared<display_device::NoopAudioContext>();
      auto persistence = std::make_unique<display_device::PersistentState>(
        std::make_shared<display_device::NoopSettingsPersistence>()
      );
      auto settings_mgr = std::make_shared<display_device::SettingsManager>(
        impersonated_dd,
        audio,
        std::move(persistence),
        display_device::WinWorkarounds {}
      );
      return InProcessDisplayContext {
        .settings_mgr = std::move(settings_mgr),
        .display = std::move(impersonated_dd),
      };
    } catch (const std::exception &ex) {
      BOOST_LOG(error) << "Display helper (in-process): failed to initialize SettingsManager: " << ex.what();
    } catch (...) {
      BOOST_LOG(error) << "Display helper (in-process): failed to initialize SettingsManager due to unknown error.";
    }
    return std::nullopt;
  }

  bool device_id_equals_ci(const std::string &lhs, const std::string &rhs) {
    if (lhs.empty() || rhs.empty()) {
      return false;
    }
    return boost::iequals(lhs, rhs);
  }

  bool device_is_active(const std::string &device_id) {
    if (device_id.empty()) {
      return false;
    }

    auto devices = platf::display_helper::Coordinator::instance().enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
    if (!devices) {
      return false;
    }

    for (const auto &device : *devices) {
      if (device.m_device_id.empty() || !device.m_info) {
        continue;
      }
      if (device_id_equals_ci(device.m_device_id, device_id)) {
        return true;
      }
    }
    return false;
  }

  bool wait_for_device_activation(const std::string &device_id, std::chrono::steady_clock::duration timeout) {
    if (device_id.empty()) {
      return false;
    }

    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (device_is_active(device_id)) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return false;
  }

  bool wait_for_virtual_display_activation(std::chrono::steady_clock::duration timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      auto virtual_displays = VDISPLAY::enumerateSudaVDADisplays();
      bool any_active = std::any_of(
        virtual_displays.begin(),
        virtual_displays.end(),
        [](const VDISPLAY::SudaVDADisplayInfo &info) {
          return info.is_active;
        }
      );
      if (any_active) {
        return true;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return false;
  }

  bool verify_helper_topology(
    const rtsp_stream::launch_session_t &session,
    const std::string &device_id
  ) {
    if (!device_id.empty()) {
      const bool has_activation_hint = session.virtual_display &&
                                       session.virtual_display_ready_since.has_value() &&
                                       !session.virtual_display_device_id.empty() &&
                                       device_id_equals_ci(device_id, session.virtual_display_device_id);
      if (has_activation_hint && device_is_active(device_id)) {
        BOOST_LOG(debug) << "Display helper: device_id " << device_id
                         << " already active; skipping activation wait.";
        return true;
      }

      if (!wait_for_device_activation(device_id, kTopologyWaitTimeout)) {
        BOOST_LOG(error) << "Display helper: device_id " << device_id << " did not become active after APPLY.";
        return false;
      }
      return true;
    }

    if (session.virtual_display) {
      const bool hint_ready = session.virtual_display_ready_since.has_value();
      if (hint_ready) {
        BOOST_LOG(debug) << "Display helper: virtual display ready hint satisfied. Skipping activation wait.";
        return true;
      }
      if (!wait_for_virtual_display_activation(kTopologyWaitTimeout)) {
        BOOST_LOG(error) << "Display helper: virtual display topology did not become active after APPLY.";
        return false;
      }
    }

    return true;
  }

  bool apply_topology_definition(
    const display_helper_integration::DisplayTopologyDefinition &topology,
    const char *label
  ) {
    if (topology.topology.empty() && topology.monitor_positions.empty()) {
      return true;
    }

    auto ctx = make_settings_manager();
    if (!ctx) {
      BOOST_LOG(warning) << "Display helper: unable to initialize display context for topology apply (" << label << ").";
      return false;
    }

    bool topology_ok = true;
    if (!topology.topology.empty()) {
      try {
        auto current_topology = ctx->display->getCurrentTopology();
        const bool already_matches = ctx->display->isTopologyTheSame(current_topology, topology.topology);
        if (!already_matches) {
          BOOST_LOG(info) << "Display helper: applying requested topology (" << label << ").";
          topology_ok = ctx->display->setTopology(topology.topology);
          if (!topology_ok) {
            BOOST_LOG(warning) << "Display helper: requested topology apply failed (" << label << ").";
          }
        } else {
          BOOST_LOG(debug) << "Display helper: requested topology already active (" << label << ").";
        }
      } catch (const std::exception &ex) {
        BOOST_LOG(warning) << "Display helper: topology inspection failed (" << label << "): " << ex.what();
        topology_ok = false;
      } catch (...) {
        BOOST_LOG(warning) << "Display helper: topology inspection failed (" << label << ") with an unknown error.";
        topology_ok = false;
      }
    }

    for (const auto &[device_id, point] : topology.monitor_positions) {
      BOOST_LOG(debug) << "Display helper: setting origin for " << device_id
                       << " to (" << point.m_x << "," << point.m_y << ") after " << label << ".";
      (void) ctx->display->setDisplayOrigin(device_id, point);
    }

    return topology_ok;
  }

  bool apply_in_process(const display_helper_integration::DisplayApplyRequest &request) {
    if (!request.configuration) {
      BOOST_LOG(error) << "Display helper (in-process): no configuration provided for APPLY request.";
      return false;
    }

    auto ctx = make_settings_manager();
    if (!ctx) {
      return false;
    }

    const auto result = ctx->settings_mgr->applySettings(*request.configuration);
    const bool ok = (result == display_device::SettingsManagerInterface::ApplyResult::Ok);
    BOOST_LOG(info) << "Display helper (in-process): APPLY result=" << (ok ? "Ok" : "Failed");
    if (!ok) {
      return false;
    }

    // Apply optional topology/placement tweaks when provided.
    if (!request.topology.topology.empty()) {
      BOOST_LOG(debug) << "Display helper (in-process): applying topology override.";
      (void) ctx->display->setTopology(request.topology.topology);
    }
    for (const auto &[device_id, point] : request.topology.monitor_positions) {
      BOOST_LOG(debug) << "Display helper (in-process): setting origin for " << device_id
                       << " to (" << point.m_x << "," << point.m_y << ").";
      (void) ctx->display->setDisplayOrigin(device_id, point);
    }

    return ok;
  }
  constexpr DWORD kHelperStopTotalWaitMs = 2000;
  constexpr DWORD kHelperForceKillWaitMs = 2000;

  bool wait_for_helper_ipc_ready_locked() {
    const auto deadline = std::chrono::steady_clock::now() + kHelperIpcReadyTimeout;
    int attempts = 0;

    platf::display_helper_client::reset_connection();
    while (std::chrono::steady_clock::now() < deadline) {
      if (platf::display_helper_client::send_ping()) {
        if (attempts > 0) {
          BOOST_LOG(debug) << "Display helper IPC became reachable after " << attempts << " retries.";
        }
        return true;
      }
      ++attempts;
      std::this_thread::sleep_for(kHelperIpcReadyPoll);
      platf::display_helper_client::reset_connection();
    }

    BOOST_LOG(warning) << "Display helper IPC did not respond within " << kHelperIpcReadyTimeout.count()
                       << " ms of helper start.";
    return false;
  }

  const char *virtual_layout_to_string(const display_helper_integration::VirtualDisplayArrangement layout) {
    using enum display_helper_integration::VirtualDisplayArrangement;
    switch (layout) {
      case Extended:
        return "extended";
      case ExtendedPrimary:
        return "extended_primary";
      case ExtendedIsolated:
        return "extended_isolated";
      case ExtendedPrimaryIsolated:
        return "extended_primary_isolated";
      case Exclusive:
      default:
        return "exclusive";
    }
  }

  void kill_all_helper_processes() {
    helper_proc().terminate();

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
      DWORD err = GetLastError();
      BOOST_LOG(error) << "Display helper: failed to snapshot processes for cleanup (winerr=" << err << ").";
      return;
    }

    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof(entry);
    std::vector<DWORD> targets;

    if (Process32FirstW(snapshot, &entry)) {
      do {
        if (_wcsicmp(entry.szExeFile, L"sunshine_display_helper.exe") == 0 &&
            entry.th32ProcessID != GetCurrentProcessId()) {
          targets.push_back(entry.th32ProcessID);
        }
      } while (Process32NextW(snapshot, &entry));
    } else {
      DWORD err = GetLastError();
      if (err != ERROR_NO_MORE_FILES) {
        BOOST_LOG(warning) << "Display helper: process enumeration failed during cleanup (winerr=" << err << ").";
      }
    }

    CloseHandle(snapshot);

    for (DWORD pid : targets) {
      HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, pid);
      if (!h) {
        DWORD err = GetLastError();
        BOOST_LOG(warning) << "Display helper: unable to open external instance (pid=" << pid
                           << ", winerr=" << err << ") for termination.";
        continue;
      }

      DWORD wait = WaitForSingleObject(h, 0);
      if (wait == WAIT_TIMEOUT) {
        BOOST_LOG(warning) << "Display helper: terminating external instance (pid=" << pid << ").";
        if (!TerminateProcess(h, 1)) {
          DWORD err = GetLastError();
          BOOST_LOG(error) << "Display helper: TerminateProcess failed for pid=" << pid << " (winerr=" << err << ").";
        } else {
          DWORD wait_res = WaitForSingleObject(h, kHelperForceKillWaitMs);
          if (wait_res != WAIT_OBJECT_0) {
            BOOST_LOG(warning) << "Display helper: external instance pid=" << pid
                               << " did not exit within " << kHelperForceKillWaitMs << " ms.";
          }
        }
      }

      CloseHandle(h);
    }
  }

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
  // Active session display parameters snapshot for re-apply on reconnect.
  // We do NOT cache serialized JSON, only the subset of session fields that
  // affect display configuration. On reconnect, we rebuild the full
  // SingleDisplayConfiguration from current Sunshine config + these fields.

  bool dd_feature_enabled() {
    using config_option_e = config::video_t::dd_t::config_option_e;
    if (config::video.dd.configuration_option != config_option_e::disabled) {
      return true;
    }

    const bool virtual_display_selected =
      (config::video.virtual_display_mode == config::video_t::virtual_display_mode_e::per_client ||
       config::video.virtual_display_mode == config::video_t::virtual_display_mode_e::shared);
    if (virtual_display_selected) {
      return true;
    }

    std::lock_guard<std::mutex> lg(g_session_mutex);
    return g_active_session_dd && g_active_session_dd->virtual_display;
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

  bool disarm_helper_restore_if_running() {
    if (!dd_feature_enabled() || shutdown_requested()) {
      return false;
    }

    bool helper_running = false;
    {
      std::lock_guard<std::mutex> lg(helper_mutex());
      if (HANDLE h = helper_proc().get_process_handle()) {
        helper_running = (WaitForSingleObject(h, 0) == WAIT_TIMEOUT);
      }
    }

    if (!helper_running) {
      helper_running = ensure_helper_started();
    }

    if (!helper_running) {
      platf::display_helper_client::reset_connection();
      BOOST_LOG(debug) << "Display helper: DISARM skipped (helper not reachable).";
      return false;
    }

    bool ok = platf::display_helper_client::send_disarm_restore();
    if (!ok) {
      BOOST_LOG(warning) << "Display helper: DISARM send failed; retrying after connection reset.";
      platf::display_helper_client::reset_connection();
      ok = platf::display_helper_client::send_disarm_restore();
    }
    BOOST_LOG(info) << "Display helper: DISARM dispatch result=" << (ok ? "true" : "false");
    return ok;
  }

  bool ensure_helper_started(bool force_restart, bool force_enable) {
    if (!force_enable && !dd_feature_enabled()) {
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
        bool need_restart = force_restart;
        if (!need_restart) {
          // Check IPC liveness with a lightweight ping; if responsive, reuse existing helper
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
          platf::display_helper_client::reset_connection();
          BOOST_LOG(warning) << "Display helper process ping failed; keeping existing instance and deferring restart.";
          return false;
        } else {
          BOOST_LOG(info) << "Display helper restart requested (force).";
        }
        if (need_restart) {
          bool graceful_shutdown = false;
          bool stop_sent = platf::display_helper_client::send_stop();
          if (stop_sent) {
            DWORD wait_stop = WaitForSingleObject(h, kHelperStopGracePeriodMs);
            if (wait_stop == WAIT_OBJECT_0) {
              DWORD exit_code = 0;
              GetExitCodeProcess(h, &exit_code);
              BOOST_LOG(info) << "Display helper exited after STOP request (code=" << exit_code << ").";
              graceful_shutdown = true;
            } else if (wait_stop == WAIT_TIMEOUT) {
              DWORD remaining = (kHelperStopTotalWaitMs > kHelperStopGracePeriodMs) ? (kHelperStopTotalWaitMs - kHelperStopGracePeriodMs) : 0;
              if (remaining > 0) {
                DWORD wait_more = WaitForSingleObject(h, remaining);
                if (wait_more == WAIT_OBJECT_0) {
                  DWORD exit_code = 0;
                  GetExitCodeProcess(h, &exit_code);
                  BOOST_LOG(info) << "Display helper exited after extended STOP wait (code=" << exit_code << ").";
                  graceful_shutdown = true;
                } else if (wait_more == WAIT_TIMEOUT) {
                  BOOST_LOG(warning) << "Display helper STOP request timed out after " << kHelperStopTotalWaitMs
                                     << " ms; will force termination.";
                } else {
                  DWORD wait_err = GetLastError();
                  BOOST_LOG(error) << "Display helper STOP wait failed (winerr=" << wait_err
                                   << "); will force termination.";
                }
              } else {
                BOOST_LOG(warning) << "Display helper STOP request timed out after " << kHelperStopGracePeriodMs
                                   << " ms; will force termination.";
              }
            } else {
              DWORD wait_err = GetLastError();
              BOOST_LOG(error) << "Display helper STOP wait failed (winerr=" << wait_err << "); will force termination.";
            }
          } else {
            BOOST_LOG(warning) << "Display helper STOP request failed; will force termination.";
          }
          platf::display_helper_client::reset_connection();
          if (!graceful_shutdown) {
            helper_proc().terminate();
            DWORD wait_result = WaitForSingleObject(h, kHelperForceKillWaitMs);
            if (wait_result == WAIT_OBJECT_0) {
              DWORD exit_code = 0;
              GetExitCodeProcess(h, &exit_code);
              BOOST_LOG(info) << "Display helper terminated after forced shutdown (code=" << exit_code << ").";
            } else if (wait_result == WAIT_TIMEOUT) {
              BOOST_LOG(error) << "Display helper process still running after forced termination wait of "
                               << kHelperForceKillWaitMs << " ms; deferring restart.";
              return false;
            } else {
              DWORD wait_err = GetLastError();
              BOOST_LOG(error) << "Display helper forced termination wait failed (winerr=" << wait_err
                               << "); deferring restart.";
              return false;
            }
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(800));
        }
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

    kill_all_helper_processes();

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
      return false;
    }
    
    HANDLE h = helper_proc().get_process_handle();
    if (!h) {
      BOOST_LOG(error) << "Display helper started but no process handle available";
      return false;
    }
    
    DWORD pid = GetProcessId(h);
    BOOST_LOG(info) << "Display helper successfully started (pid=" << pid << ")";
    
    // Give the helper process time to initialize and create its named pipe server
    // Check if it exits early (e.g., singleton mutex conflict from incomplete cleanup)
    for (int check = 0; check < 6; ++check) {
      DWORD wait = WaitForSingleObject(h, 50);
      if (wait == WAIT_OBJECT_0) {
        DWORD exit_code = 0;
        GetExitCodeProcess(h, &exit_code);
        if (exit_code == 3) {
          BOOST_LOG(warning) << "Display helper exited immediately with code 3 (singleton conflict). "
                           << "Retrying after extended cleanup delay...";
          std::this_thread::sleep_for(std::chrono::milliseconds(1000));
          
          const bool retry_started = helper_proc().start(helper.wstring(), L"");
          if (!retry_started) {
            BOOST_LOG(error) << "Display helper retry start failed";
            return false;
          }
          h = helper_proc().get_process_handle();
          if (h) {
            pid = GetProcessId(h);
            BOOST_LOG(info) << "Display helper retry succeeded (pid=" << pid << ")";
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
          }
          break;
        } else {
          BOOST_LOG(error) << "Display helper exited unexpectedly with code " << exit_code;
          return false;
        }
      }
    }
    
    // Final initialization delay for pipe server creation
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return wait_for_helper_ipc_ready_locked();
  }

  // Watchdog state for helper liveness during active streams
  static std::atomic<bool> g_watchdog_running {false};
  static std::jthread g_watchdog_thread;
  static std::chrono::steady_clock::time_point g_last_vd_reenable {};

  constexpr auto kVirtualDisplayReenableCooldown = std::chrono::seconds(3);

  bool recently_reenabled_virtual_display() {
    if (g_last_vd_reenable.time_since_epoch().count() == 0) {
      return false;
    }
    return (std::chrono::steady_clock::now() - g_last_vd_reenable) < kVirtualDisplayReenableCooldown;
  }

  void explicit_virtual_display_reset_and_apply(
    display_helper_integration::DisplayApplyBuilder &builder,
    const rtsp_stream::launch_session_t &session,
    std::function<bool(const display_helper_integration::DisplayApplyRequest &)> apply_fn
  ) {
    // Only act if virtual display is in play.
    if (!session.virtual_display && !builder.build().session_overrides.virtual_display_override.value_or(false)) {
      return;
    }

    // Debounce to avoid hammering the driver.
    if (recently_reenabled_virtual_display()) {
      return;
    }

    // First send a "blank" request to detach virtual display.
    display_helper_integration::DisplayApplyBuilder disable_builder;
    disable_builder.set_session(session);
    auto &overrides = disable_builder.mutable_session_overrides();
    overrides.virtual_display_override = false;
    disable_builder.set_action(display_helper_integration::DisplayApplyAction::Apply);
    auto disable_req = disable_builder.build();

    BOOST_LOG(info) << "Display helper: explicit virtual display disable before re-enable.";
    (void) apply_fn(disable_req);

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // Re-enable with the original builder intent.
    BOOST_LOG(info) << "Display helper: explicit virtual display re-enable after disappearance.";
    auto enable_req = builder.build();
    if (apply_fn(enable_req)) {
      g_last_vd_reenable = std::chrono::steady_clock::now();
    }
  }

  static void set_active_session(
    const rtsp_stream::launch_session_t &session,
    std::optional<std::string> device_id_override = std::nullopt,
    std::optional<int> fps_override = std::nullopt,
    std::optional<int> width_override = std::nullopt,
    std::optional<int> height_override = std::nullopt,
    std::optional<bool> virtual_display_override = std::nullopt,
    std::optional<int> framegen_refresh_override = std::nullopt
  ) {
    std::lock_guard<std::mutex> lg(g_session_mutex);
    const int effective_fps = fps_override ? *fps_override : (session.framegen_refresh_rate && *session.framegen_refresh_rate > 0 ? *session.framegen_refresh_rate : session.fps);
    g_active_session_dd = session_dd_fields_t {
      .width = width_override ? *width_override : session.width,
      .height = height_override ? *height_override : session.height,
      .fps = effective_fps,
      .enable_hdr = session.enable_hdr,
      .enable_sops = session.enable_sops,
      .virtual_display = virtual_display_override ? *virtual_display_override : session.virtual_display,
      .virtual_display_device_id = device_id_override ? *device_id_override : session.virtual_display_device_id,
      .framegen_refresh_rate = framegen_refresh_override ? framegen_refresh_override : session.framegen_refresh_rate,
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

  std::optional<std::string> build_helper_apply_payload(const display_helper_integration::DisplayApplyRequest &request) {
    if (!request.configuration) {
      BOOST_LOG(error) << "Display helper: no configuration provided for APPLY payload.";
      return std::nullopt;
    }

    bool ok = true;
    std::string json = display_device::toJson(*request.configuration, 0u, &ok);
    if (!ok) {
      BOOST_LOG(error) << "Display helper: failed to serialize configuration for helper APPLY payload.";
      return std::nullopt;
    }

    nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
    if (j.is_discarded()) {
      BOOST_LOG(error) << "Display helper: failed to parse serialized configuration JSON for helper APPLY payload.";
      return std::nullopt;
    }

    if (request.attach_hdr_toggle_flag) {
      j["wa_hdr_toggle"] = true;
    }

    if (request.virtual_display_arrangement) {
      j["sunshine_virtual_layout"] = virtual_layout_to_string(*request.virtual_display_arrangement);
    }

    if (!request.topology.topology.empty()) {
      nlohmann::json topo = nlohmann::json::array();
      for (const auto &grp : request.topology.topology) {
        nlohmann::json group = nlohmann::json::array();
        for (const auto &id : grp) {
          group.push_back(id);
        }
        topo.push_back(std::move(group));
      }
      j["sunshine_topology"] = std::move(topo);
    }

    if (!request.topology.monitor_positions.empty()) {
      nlohmann::json positions = nlohmann::json::object();
      for (const auto &[device_id, point] : request.topology.monitor_positions) {
        positions[device_id] = {{"x", point.m_x}, {"y", point.m_y}};
      }
      j["sunshine_monitor_positions"] = std::move(positions);
    }

    return j.dump();
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
        // Do not re-apply automatically on reconnect; just confirm IPC is reachable.
        helper_ready = platf::display_helper_client::send_ping();
      }
    }
  }

}  // namespace

namespace display_helper_integration {
  bool apply(const DisplayApplyRequest &request) {
    if (request.action == DisplayApplyAction::Skip) {
      BOOST_LOG(info) << "Display helper: configuration parse failed; not dispatching.";
      return false;
    }

    if (request.action == DisplayApplyAction::Revert) {
      const bool helper_ready = ensure_helper_started(false, true);
      if (!helper_ready) {
        BOOST_LOG(warning) << "Display helper: REVERT skipped (helper not reachable).";
        clear_active_session();
        return false;
      }
      BOOST_LOG(info) << "Display helper: sending REVERT request (builder).";
      const bool ok = platf::display_helper_client::send_revert();
      BOOST_LOG(info) << "Display helper: REVERT dispatch result=" << (ok ? "true" : "false");
      clear_active_session();
      return ok;
    }

    if (request.action != DisplayApplyAction::Apply) {
      return false;
    }

    const bool system_profile_only = platf::is_running_as_system() && !platf::has_active_console_session();

    if (!system_profile_only) {
      bool helper_ready = ensure_helper_started(false, true);
      if (!helper_ready) {
        helper_ready = ensure_helper_started(false, true);
      }

      if (helper_ready) {
        auto payload = build_helper_apply_payload(request);
        if (!payload) {
          BOOST_LOG(error) << "Display helper: failed to build APPLY payload for helper dispatch.";
          return false;
        }

        BOOST_LOG(info) << "Display helper: sending APPLY request via helper.";
        const bool ok = platf::display_helper_client::send_apply_json(*payload);
        BOOST_LOG(info) << "Display helper: APPLY dispatch result=" << (ok ? "true" : "false");
        if (ok && request.session) {
          set_active_session(
            *request.session,
            request.session_overrides.device_id_override,
            request.session_overrides.fps_override,
            request.session_overrides.width_override,
            request.session_overrides.height_override,
            request.session_overrides.virtual_display_override,
            request.session_overrides.framegen_refresh_override
          );
          if (request.enable_virtual_display_watchdog) {
            platf::display_helper::Coordinator::instance().set_virtual_display_watchdog_enabled(true);
          }
        }
        return ok;
      }

      BOOST_LOG(warning) << "Display helper: helper unavailable; falling back to in-process APPLY.";
    }

    if (!request.session) {
      BOOST_LOG(error) << "Display helper: missing session context for in-process APPLY.";
      return false;
    }

    if (!apply_in_process(request)) {
      BOOST_LOG(warning) << "Display helper: in-process APPLY failed.";
      return false;
    }

    const auto device_id = request.configuration ? request.configuration->m_device_id : std::string {};
    if (!verify_helper_topology(*request.session, device_id)) {
      BOOST_LOG(warning) << "Display helper: topology verification failed after in-process APPLY.";
    }
    (void) apply_topology_definition(request.topology, "in-process");

    set_active_session(
      *request.session,
      request.session_overrides.device_id_override,
      request.session_overrides.fps_override,
      request.session_overrides.width_override,
      request.session_overrides.height_override,
      request.session_overrides.virtual_display_override,
      request.session_overrides.framegen_refresh_override
    );
    if (request.enable_virtual_display_watchdog) {
      platf::display_helper::Coordinator::instance().set_virtual_display_watchdog_enabled(true);
    }
    return true;
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

  bool disarm_pending_restore() {
    return disarm_helper_restore_if_running();
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

  bool snapshot_current_display_state() {
    if (!ensure_helper_started()) {
      BOOST_LOG(info) << "Display helper unavailable; cannot snapshot current display state.";
      return false;
    }
    BOOST_LOG(info) << "Display helper: sending SNAPSHOT_CURRENT request.";
    const bool ok = platf::display_helper_client::send_snapshot_current();
    BOOST_LOG(info) << "Display helper: SNAPSHOT_CURRENT dispatch result=" << (ok ? "true" : "false");
    return ok;
  }

  std::optional<display_device::EnumeratedDeviceList> enumerate_devices(
    display_device::DeviceEnumerationDetail detail
  ) {
    try {
      display_device::DisplayRecoveryBehaviorGuard guard(display_device::DisplayRecoveryBehavior::Skip);
      auto api = std::make_shared<display_device::WinApiLayer>();
      display_device::WinDisplayDevice dd(api);
      return dd.enumAvailableDevices(detail);
    } catch (...) {
      return std::nullopt;
    }
  }

  std::optional<std::vector<std::vector<std::string>>> capture_current_topology() {
    try {
      display_device::DisplayRecoveryBehaviorGuard guard(display_device::DisplayRecoveryBehavior::Skip);
      auto api = std::make_shared<display_device::WinApiLayer>();
      display_device::WinDisplayDevice dd(api);
      return dd.getCurrentTopology();
    } catch (...) {
      return std::nullopt;
    }
  }

  std::string enumerate_devices_json(display_device::DeviceEnumerationDetail detail) {
    auto devices = enumerate_devices(detail);
    if (!devices) {
      return "[]";
    }
    if (detail == display_device::DeviceEnumerationDetail::Minimal) {
      devices->erase(
        std::remove_if(
          devices->begin(),
          devices->end(),
          [](const display_device::EnumeratedDevice &device) {
            return !device.m_info.has_value();
          }),
        devices->end());
    }
    return display_device::toJson(*devices);
  }

  void start_watchdog() {
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
