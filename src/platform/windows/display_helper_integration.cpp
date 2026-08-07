/**
 * @file src/platform/windows/display_helper_integration.cpp
 */
#ifdef _WIN32

  #include <winsock2.h>

  // standard
  #include <algorithm>
  #include <atomic>
  #include <boost/algorithm/string/predicate.hpp>
  #include <chrono>
  #include <cmath>
  #include <condition_variable>
  #include <cstdint>
  #include <exception>
  #include <filesystem>
  #include <functional>
  #include <limits>
  #include <mutex>
  #include <optional>
  #include <string>
  #include <string_view>
  #include <thread>
  #include <vector>

  // libdisplaydevice
  #include <display_device/json.h>
  #include <display_device/windows/win_api_layer.h>
  #include <display_device/windows/win_api_recovery.h>
  #include <display_device/windows/win_api_utils.h>
  #include <display_device/windows/win_display_device.h>
  #include <nlohmann/json.hpp>

  // sunshine
  #include "display_helper_integration.h"
  #include "src/globals.h"
  #include "src/logging.h"
  #include "src/platform/windows/display_helper_coordinator.h"
  #include "src/platform/windows/display_helper_request_helpers.h"
  #include "src/platform/windows/frame_limiter_nvcp.h"
  #include "src/platform/windows/impersonating_display_device.h"
  #include "src/platform/windows/ipc/display_settings_client.h"
  #include "src/platform/windows/ipc/misc_utils.h"
  #include "src/platform/windows/ipc/process_handler.h"
  #include "src/platform/windows/misc.h"
  #include "src/platform/windows/virtual_display.h"
  #include "src/process.h"
  #include "src/state_storage.h"
  #include "src/stream.h"
  #include "src/webrtc_stream.h"

  #include <display_device/noop_audio_context.h>
  #include <display_device/noop_settings_persistence.h>
  #include <display_device/windows/persistent_state.h>
  #include <display_device/windows/settings_manager.h>
  #include <display_device/windows/types.h>
  #include <tlhelp32.h>

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

  struct PendingSessionSnapshot {
    std::uint32_t id = 0;
    std::string unique_id;
    std::string client_uuid;
    int width = 0;
    int height = 0;
    int fps = 0;
    bool client_display_mode_override = false;
    std::uint32_t client_display_refresh_millihz = 0;
    std::optional<bool> client_virtual_display_override;
    bool enable_hdr = false;
    bool enable_sops = false;
    bool virtual_display = false;
    std::optional<rtsp_stream::launch_session_t::resolution_override_t> resolution_override;
    std::string virtual_display_device_id;
    std::optional<std::chrono::steady_clock::time_point> virtual_display_ready_since;
    std::optional<bool> virtual_display_hdr_enabled;
    std::optional<int> framegen_refresh_rate;
    std::optional<std::uint32_t> framegen_refresh_millihz;
    int framegen_refresh_multiplier = 1;
    bool gen1_framegen_fix = false;
    bool gen2_framegen_fix = false;
  };

  struct PendingApplyState {
    display_helper_integration::DisplayApplyRequest request;
    PendingSessionSnapshot session_snapshot;
    uint32_t session_id {0};
    bool has_session {false};
    int attempts {0};
    std::optional<std::chrono::steady_clock::time_point> ready_since;
    std::chrono::steady_clock::time_point next_attempt {};
  };

  std::mutex &pending_apply_mutex() {
    static std::mutex m;
    return m;
  }

  std::optional<PendingApplyState> &pending_apply_state() {
    static std::optional<PendingApplyState> state;
    return state;
  }

  // Serializes a claimed deferred APPLY with cancellation/revert. The pending
  // state lock only protects the queue; this lock covers the actual IPC work.
  std::mutex &pending_apply_execution_mutex() {
    static std::mutex m;
    return m;
  }

  // Requires pending_apply_execution_mutex(). A display operation owns that
  // mutex across its IPC work, so clearing the queue here cannot race an
  // in-flight deferred request or a newer normal APPLY/REVERT.
  void clear_pending_apply_queue_locked() {
    std::lock_guard<std::mutex> lock(pending_apply_mutex());
    pending_apply_state().reset();
  }

  std::atomic<bool> &cold_start_resolution_deferral_armed() {
    static std::atomic<bool> armed {true};
    return armed;
  }

  bool user_session_ready();

  bool request_includes_resolution(const display_helper_integration::DisplayApplyRequest &request) {
    if (!request.configuration) {
      return false;
    }
    return request.configuration->m_resolution.has_value();
  }

  PendingApplyState make_pending_apply_state(const display_helper_integration::DisplayApplyRequest &request) {
    PendingApplyState state;
    state.request = request;
    state.has_session = request.session != nullptr;
    state.request.session = nullptr;

    if (request.session) {
      state.session_id = request.session->id;
      state.session_snapshot.id = request.session->id;
      state.session_snapshot.unique_id = request.session->unique_id;
      state.session_snapshot.client_uuid = request.session->client_uuid;
      state.session_snapshot.width = request.session->width;
      state.session_snapshot.height = request.session->height;
      state.session_snapshot.fps = request.session->fps;
      state.session_snapshot.client_display_mode_override = request.session->client_display_mode_override;
      state.session_snapshot.client_display_refresh_millihz = request.session->client_display_refresh_millihz;
      state.session_snapshot.client_virtual_display_override = request.session->client_virtual_display_override;
      state.session_snapshot.enable_hdr = rtsp_stream::effective_hdr_requested(*request.session);
      state.session_snapshot.enable_sops = request.session->enable_sops;
      state.session_snapshot.virtual_display = request.session->virtual_display;
      state.session_snapshot.resolution_override = request.session->resolution_override;
      state.session_snapshot.virtual_display_device_id = request.session->virtual_display_device_id;
      state.session_snapshot.virtual_display_ready_since = request.session->virtual_display_ready_since;
      state.session_snapshot.virtual_display_hdr_enabled = request.session->virtual_display_hdr_enabled;
      state.session_snapshot.framegen_refresh_rate = request.session->framegen_refresh_rate;
      state.session_snapshot.framegen_refresh_millihz = request.session->framegen_refresh_millihz;
      state.session_snapshot.framegen_refresh_multiplier = request.session->framegen_refresh_multiplier;
      state.session_snapshot.gen1_framegen_fix = request.session->gen1_framegen_fix;
      state.session_snapshot.gen2_framegen_fix = request.session->gen2_framegen_fix;
    }

    return state;
  }

  void queue_deferred_resolution_apply(const display_helper_integration::DisplayApplyRequest &request) {
    PendingApplyState state = make_pending_apply_state(request);
    std::lock_guard<std::mutex> lock(pending_apply_mutex());
    pending_apply_state() = std::move(state);
    BOOST_LOG(info) << "Display helper: deferring resolution apply for session " << pending_apply_state()->session_id << ".";
  }

  void maybe_queue_deferred_resolution_apply_on_api_unavailable(
    const display_helper_integration::DisplayApplyRequest &request
  ) {
    if (!request.session) {
      return;
    }
    if (!request_includes_resolution(request)) {
      return;
    }
    const auto session_id = request.session->id;
    queue_deferred_resolution_apply(request);
    BOOST_LOG(info) << "Display helper: API unavailable; queued deferred resolution apply for session "
                    << session_id << ".";
  }

  bool should_defer_resolution_apply(const display_helper_integration::DisplayApplyRequest &request) {
    if (!request.session) {
      return false;
    }
    if (!request_includes_resolution(request)) {
      return false;
    }
    if (!platf::is_running_as_system()) {
      return false;
    }
    if (user_session_ready()) {
      return false;
    }
    return true;
  }

  void maybe_queue_deferred_resolution_apply(
    const display_helper_integration::DisplayApplyRequest &request,
    bool allow_resolution_deferral
  ) {
    if (!allow_resolution_deferral) {
      return;
    }
    if (!should_defer_resolution_apply(request)) {
      return;
    }
    bool expected = true;
    if (!cold_start_resolution_deferral_armed().compare_exchange_strong(expected, false)) {
      return;
    }
    queue_deferred_resolution_apply(request);
  }

  bool user_session_ready() {
    HANDLE user_token = platf::dxgi::retrieve_users_token(false);
    if (!user_token) {
      return false;
    }
    CloseHandle(user_token);
    return true;
  }

  constexpr std::chrono::seconds kTopologyWaitTimeout {6};
  constexpr std::chrono::milliseconds kHelperIpcReadyTimeout {5000};
  constexpr std::chrono::milliseconds kHelperIpcReadyPoll {100};

  // Stream-start requirement: stop very recent helper restore activity quickly.
  // Once the helper has had time to begin an actual restore, do not kill/overwrite
  // that in-flight restore from a later stream-start probe; the helper will either
  // finish restoring or an explicit APPLY will supersede it.
  constexpr std::chrono::milliseconds kDisarmRestoreBudget {150};
  constexpr std::chrono::milliseconds kDisarmRetryThrottle {150};
  constexpr std::chrono::milliseconds kDeferredApplyInitialDelay {2000};
  constexpr std::chrono::milliseconds kDeferredApplyRetryBase {500};
  constexpr std::chrono::milliseconds kDeferredApplyRetryMax {10000};
  constexpr std::chrono::milliseconds kHelperStartFailureCooldown {30000};
  constexpr int kHelperStartFailuresBeforeCooldown {2};
  constexpr int kMaxDeferredApplyAttempts = 6;

  // Helper liveness probe envelopes. Ordinary callers keep the client's normal
  // send budget so a reconnect under load still succeeds; only shutdown-class
  // callers (owned recovery/teardown workers) collapse to the short probe.
  // The cancellable variant is still used for both because operation_deadline
  // must be able to cut the probe short, which plain send_ping() cannot do.
  constexpr int kHelperPingTimeoutMs {5000};
  constexpr int kShutdownHelperPingTimeoutMs {250};

  bool operation_deadline_expired(
    const std::chrono::steady_clock::time_point operation_deadline) {
    return operation_deadline != std::chrono::steady_clock::time_point::max() &&
           std::chrono::steady_clock::now() >= operation_deadline;
  }

  std::chrono::milliseconds operation_wait_slice(
    const std::chrono::milliseconds requested,
    const std::chrono::steady_clock::time_point operation_deadline) {
    if (operation_deadline == std::chrono::steady_clock::time_point::max()) {
      return requested;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      operation_deadline - std::chrono::steady_clock::now());
    return std::max(
      std::chrono::milliseconds::zero(),
      std::min(requested, remaining)
    );
  }

  bool shutdown_requested();
  bool ensure_helper_started(
    bool force_restart = false,
    bool force_enable = false,
    const std::function<bool()> &cancellation_predicate = {},
    std::chrono::steady_clock::time_point operation_deadline =
      std::chrono::steady_clock::time_point::max(),
    bool shutdown_class_caller = false);
  const char *virtual_layout_to_string(const display_helper_integration::VirtualDisplayArrangement layout);

  bool helper_process_running(
    const std::function<bool()> &cancellation_predicate = {},
    const std::chrono::steady_clock::time_point operation_deadline =
      std::chrono::steady_clock::time_point::max()) {
    std::unique_lock<std::mutex> lg(helper_mutex(), std::defer_lock);
    while (!lg.try_lock()) {
      if ((cancellation_predicate && cancellation_predicate()) ||
          operation_deadline_expired(operation_deadline)) {
        return false;
      }
      const auto wait = operation_wait_slice(
        std::chrono::milliseconds(25),
        operation_deadline
      );
      if (wait <= std::chrono::milliseconds::zero()) {
        return false;
      }
      std::this_thread::sleep_for(wait);
    }
    if ((cancellation_predicate && cancellation_predicate()) ||
        operation_deadline_expired(operation_deadline)) {
      return false;
    }
    if (HANDLE h = helper_proc().get_process_handle()) {
      return WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    }
    return false;
  }

  bool restore_expected_with_live_helper(
    const std::function<bool()> &cancellation_predicate = {},
    std::chrono::steady_clock::time_point operation_deadline =
      std::chrono::steady_clock::time_point::max());

  std::chrono::milliseconds deferred_apply_retry_delay(int attempts) {
    if (attempts <= 0) {
      return kDeferredApplyRetryBase;
    }
    const int shift = std::min(attempts - 1, 5);
    auto delay = kDeferredApplyRetryBase * (1 << shift);
    if (delay > kDeferredApplyRetryMax) {
      delay = kDeferredApplyRetryMax;
    }
    return delay;
  }

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

  // User-configured exclusions plus every Sunshine-managed virtual display device id we have
  // seen. Virtual displays must never end up in restore baselines: a baseline captured while
  // one was active "restores" the physical monitors away (vibeshine#223).
  std::vector<std::string> effective_snapshot_exclude_devices() {
    std::vector<std::string> ids = config::video.dd.snapshot_exclude_devices;
    for (auto &vd : statefile::load_virtual_display_devices()) {
      const bool present = std::any_of(ids.begin(), ids.end(), [&](const std::string &existing) {
        return boost::algorithm::iequals(existing, vd);
      });
      if (!present) {
        ids.push_back(std::move(vd));
      }
    }
    return ids;
  }

  std::string build_snapshot_exclude_payload() {
    try {
      nlohmann::json j = effective_snapshot_exclude_devices();
      return j.dump();
    } catch (...) {
      return std::string {};
    }
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

  bool any_virtual_display_active() {
    auto virtual_displays = VDISPLAY::enumerateVirtualDisplays();
    return std::any_of(
      virtual_displays.begin(),
      virtual_displays.end(),
      [](const VDISPLAY::VirtualDisplayInfo &info) {
        return info.is_active;
      }
    );
  }

  bool wait_for_virtual_display_activation(std::chrono::steady_clock::duration timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (any_virtual_display_active()) {
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
      // The hint records a past observation, so confirm the display is still active
      // before skipping the wait. Mirrors the device_id branch above.
      const bool hint_ready = session.virtual_display_ready_since.has_value();
      if (hint_ready && any_virtual_display_active()) {
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

  display_device::SettingsManagerInterface::ApplyResult apply_in_process(
    const display_helper_integration::DisplayApplyRequest &request
  ) {
    if (!request.configuration) {
      BOOST_LOG(error) << "Display helper (in-process): no configuration provided for APPLY request.";
      return display_device::SettingsManagerInterface::ApplyResult::DevicePrepFailed;
    }

    auto ctx = make_settings_manager();
    if (!ctx) {
      return display_device::SettingsManagerInterface::ApplyResult::DevicePrepFailed;
    }

    const auto result = ctx->settings_mgr->applySettings(*request.configuration);
    const bool ok = (result == display_device::SettingsManagerInterface::ApplyResult::Ok);
    BOOST_LOG(info) << "Display helper (in-process): APPLY result=" << (ok ? "Ok" : "Failed");
    if (!ok) {
      return result;
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

    return display_device::SettingsManagerInterface::ApplyResult::Ok;
  }

  constexpr DWORD kHelperForceKillWaitMs = 2000;

  bool cancellation_requested(const std::function<bool()> &cancellation_predicate) {
    return cancellation_predicate && cancellation_predicate();
  }

  bool sleep_with_cancellation(
    std::chrono::milliseconds duration,
    const std::function<bool()> &cancellation_predicate,
    const std::chrono::steady_clock::time_point operation_deadline =
      std::chrono::steady_clock::time_point::max()) {
    constexpr auto slice = std::chrono::milliseconds(50);
    for (auto elapsed = std::chrono::milliseconds::zero(); elapsed < duration; elapsed += slice) {
      if (cancellation_requested(cancellation_predicate) ||
          operation_deadline_expired(operation_deadline)) {
        return false;
      }
      const auto wait = operation_wait_slice(
        std::min(slice, duration - elapsed),
        operation_deadline
      );
      if (wait <= std::chrono::milliseconds::zero()) {
        return false;
      }
      std::this_thread::sleep_for(wait);
    }
    return !cancellation_requested(cancellation_predicate) &&
           !operation_deadline_expired(operation_deadline);
  }

  // Recovery owns a stop token and main joins it before it joins stream
  // threads. Do not let that join wait behind a regular display operation:
  // ordinary callers retain blocking serialization, while cancellable callers
  // poll the gate and leave promptly after their owner requests stop.
  bool lock_pending_apply_execution(
    std::unique_lock<std::mutex> &lock,
    const std::function<bool()> &cancellation_predicate,
    const std::chrono::steady_clock::time_point operation_deadline =
      std::chrono::steady_clock::time_point::max()) {
    if (!cancellation_predicate &&
        operation_deadline == std::chrono::steady_clock::time_point::max()) {
      lock.lock();
      return true;
    }
    while (!lock.try_lock()) {
      if (!sleep_with_cancellation(
            std::chrono::milliseconds(25),
            cancellation_predicate,
            operation_deadline)) {
        return false;
      }
    }
    return !cancellation_requested(cancellation_predicate) &&
           !operation_deadline_expired(operation_deadline);
  }

  bool wait_for_process_with_cancellation(
    HANDLE process,
    DWORD timeout_ms,
    const std::function<bool()> &cancellation_predicate,
    DWORD &wait_result,
    const std::chrono::steady_clock::time_point operation_deadline =
      std::chrono::steady_clock::time_point::max()) {
    const auto deadline = std::min(
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms),
      operation_deadline
    );
    while (true) {
      if (cancellation_requested(cancellation_predicate)) {
        return false;
      }
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
      if (remaining <= std::chrono::milliseconds::zero()) {
        wait_result = WAIT_TIMEOUT;
        return true;
      }
      wait_result = WaitForSingleObject(process, static_cast<DWORD>(std::min<std::int64_t>(remaining.count(), 50)));
      if (wait_result != WAIT_TIMEOUT) {
        return true;
      }
    }
  }

  bool wait_for_helper_ipc_ready_locked(
    const std::function<bool()> &cancellation_predicate = {},
    const std::chrono::steady_clock::time_point operation_deadline =
      std::chrono::steady_clock::time_point::max(),
    const bool shutdown_class_caller = false) {
    const auto deadline = std::min(
      std::chrono::steady_clock::now() + kHelperIpcReadyTimeout,
      operation_deadline
    );
    int attempts = 0;

    if (cancellation_requested(cancellation_predicate) ||
        operation_deadline_expired(deadline) ||
        !platf::display_helper_client::reset_connection_cancellable(
          cancellation_predicate,
          deadline)) {
      return false;
    }
    while (std::chrono::steady_clock::now() < deadline) {
      if (shutdown_requested() || cancellation_requested(cancellation_predicate)) {
        return false;
      }
      const bool ping_ok = cancellation_predicate ?
                             platf::display_helper_client::send_ping_cancellable(
                               shutdown_class_caller ? kShutdownHelperPingTimeoutMs : kHelperPingTimeoutMs,
                               cancellation_predicate,
                               deadline) :
                             platf::display_helper_client::send_ping();
      if (ping_ok) {
        if (attempts > 0) {
          BOOST_LOG(debug) << "Display helper IPC became reachable after " << attempts << " retries.";
        }
        return true;
      }
      ++attempts;
      if (!sleep_with_cancellation(
            kHelperIpcReadyPoll,
            cancellation_predicate,
            deadline) ||
          !platf::display_helper_client::reset_connection_cancellable(
            cancellation_predicate,
            deadline)) {
        return false;
      }
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

  bool kill_all_helper_processes(
    const std::function<bool()> &cancellation_predicate = {},
    const std::chrono::steady_clock::time_point operation_deadline =
      std::chrono::steady_clock::time_point::max()) {
    if (cancellation_requested(cancellation_predicate) ||
        operation_deadline_expired(operation_deadline)) {
      return false;
    }
    helper_proc().terminate();

    if (cancellation_requested(cancellation_predicate)) {
      return false;
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
      DWORD err = GetLastError();
      BOOST_LOG(error) << "Display helper: failed to snapshot processes for cleanup (winerr=" << err << ").";
      return !cancellation_requested(cancellation_predicate);
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
      if (cancellation_requested(cancellation_predicate) ||
          operation_deadline_expired(operation_deadline)) {
        return false;
      }
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
          DWORD wait_res = WAIT_TIMEOUT;
          if (!wait_for_process_with_cancellation(
                h,
                kHelperForceKillWaitMs,
                cancellation_predicate,
                wait_res,
                operation_deadline)) {
            CloseHandle(h);
            return false;
          }
          if (wait_res != WAIT_OBJECT_0) {
            BOOST_LOG(warning) << "Display helper: external instance pid=" << pid
                               << " did not exit within " << kHelperForceKillWaitMs << " ms.";
          }
        }
      }

      CloseHandle(h);
    }
    return !cancellation_requested(cancellation_predicate) &&
           !operation_deadline_expired(operation_deadline);
  }

  struct session_dd_fields_t {
    std::uint64_t generation = 0;
    std::uint32_t launch_session_id = 0;
    std::string session_unique_id;
    std::string session_client_uuid;
    int width = -1;
    int height = -1;
    int fps = -1;
    std::uint32_t refresh_millihz = 0;
    bool enable_hdr = false;
    bool enable_sops = false;
    bool virtual_display = false;
    std::string virtual_display_device_id;
    std::optional<int> framegen_refresh_rate;
    std::optional<std::uint32_t> framegen_refresh_millihz;
    int framegen_refresh_multiplier = 1;
    bool gen1_framegen_fix = false;
    bool gen2_framegen_fix = false;
  };

  static std::mutex g_session_mutex;
  static std::optional<session_dd_fields_t> g_active_session_dd;
  static std::uint64_t g_next_active_session_generation = 0;

  // Tracks whether we've recently requested a helper REVERT and therefore expect a restore loop to be active.
  // Used to avoid spamming DISARM frames and to enable a kill-switch if IPC is wedged.
  static std::atomic<bool> g_restore_expected {false};

  // Resolve the effective display helper engine. In automatic mode the v2 engine
  // only rides pre-release builds; stable releases keep the legacy engine until
  // v2 has soaked, and users opt in explicitly via dd_display_helper_engine.
  static bool use_legacy_helper_engine() {
    using engine_e = config::video_t::dd_t::helper_engine_e;
    switch (config::video.dd.display_helper_engine) {
      case engine_e::legacy:
        return true;
      case engine_e::v2:
        return false;
      case engine_e::automatic:
      default:
        break;
    }
#ifdef PROJECT_VERSION_PRERELEASE
    return std::string_view(PROJECT_VERSION_PRERELEASE).empty();
#else
    return true;
#endif
  }
  static std::atomic<std::uint64_t> g_restore_generation {0};
  static std::atomic<std::uint64_t> g_disarm_generation_sent {0};
  static std::atomic<std::int64_t> g_last_revert_us {0};
  static std::atomic<std::int64_t> g_last_disarm_attempt_us {0};
  static std::atomic<std::int64_t> g_last_disarm_success_us {0};
  static std::atomic<std::int64_t> g_last_helper_start_failure_us {0};
  // The cooldown exists to stop a hot restart loop when the helper genuinely cannot
  // run. A single failure right after a helper that had been serving fine is a
  // different thing - usually one lost race - and blocking restarts for 30s there
  // leaves a live stream with no display control at all, so it costs one free retry.
  static std::atomic<int> g_consecutive_helper_start_failures {0};

  // Tracks when the most recent successful APPLY completed, so the capture thread
  // can add a stabilization delay before attempting to reinit after topology changes.
  static std::atomic<std::int64_t> g_last_apply_completed_us {0};
  static std::atomic<std::uint64_t> g_last_apply_generation {0};
  static std::atomic<std::uint64_t> g_last_verified_apply_generation {0};
  static std::atomic<std::uint64_t> g_capture_stable_eligible_apply_generation {0};
  static std::atomic<std::uint64_t> g_hdr_requested_apply_generation {0};

  // Deadline of the most recent bounded stream-start APPLY in steady-clock
  // microseconds, or zero when the most recent APPLY was unbounded. Capture
  // start reads it so its own settling waits are deducted from the same budget
  // instead of running after it has already expired.
  static std::atomic<std::int64_t> g_stream_start_deadline_us {0};

  static std::int64_t now_steady_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
  }

  static void note_successful_apply() {
    g_last_apply_completed_us.store(now_steady_us(), std::memory_order_release);
  }

  // A control command that can change the desktop must revoke any outstanding
  // capture-gate proof. Tickets carry their own generation, so this does not
  // make a later APPLY depend on mutable global request identity.
  static void invalidate_apply_verification() {
    g_last_apply_generation.fetch_add(1, std::memory_order_acq_rel);
    g_last_verified_apply_generation.store(0, std::memory_order_release);
    g_capture_stable_eligible_apply_generation.store(0, std::memory_order_release);
    g_hdr_requested_apply_generation.store(0, std::memory_order_release);
  }
  bool helper_start_failure_cooldown_active() {
    const auto last_us = g_last_helper_start_failure_us.load(std::memory_order_relaxed);
    if (last_us <= 0) {
      return false;
    }

    const auto elapsed_us = now_steady_us() - last_us;
    const auto cooldown_us = std::chrono::duration_cast<std::chrono::microseconds>(kHelperStartFailureCooldown).count();
    if (elapsed_us >= cooldown_us) {
      return false;
    }

    const auto remaining_ms = (cooldown_us - elapsed_us) / 1000;
    BOOST_LOG(warning) << "Display helper: skipping helper start during failure cooldown ("
                       << remaining_ms << "ms remaining).";
    return true;
  }

  void note_helper_start_failure(const char *reason) {
    const int failures = g_consecutive_helper_start_failures.fetch_add(1, std::memory_order_relaxed) + 1;
    if (failures < kHelperStartFailuresBeforeCooldown) {
      BOOST_LOG(warning) << "Display helper: start failed after " << reason
                         << "; retrying without a cooldown.";
      return;
    }
    g_last_helper_start_failure_us.store(now_steady_us(), std::memory_order_relaxed);
    BOOST_LOG(warning) << "Display helper: helper start failure cooldown armed after " << reason
                       << " (" << failures << " consecutive failures).";
  }

  void note_helper_start_success() {
    g_consecutive_helper_start_failures.store(0, std::memory_order_relaxed);
    g_last_helper_start_failure_us.store(0, std::memory_order_relaxed);
  }

  bool restore_expected_with_live_helper(
    const std::function<bool()> &cancellation_predicate,
    const std::chrono::steady_clock::time_point operation_deadline) {
    if (!g_restore_expected.load(std::memory_order_relaxed)) {
      return false;
    }
    if (helper_process_running(cancellation_predicate, operation_deadline)) {
      return true;
    }
    if (cancellation_requested(cancellation_predicate) ||
        operation_deadline_expired(operation_deadline)) {
      return false;
    }
    g_restore_expected.store(false, std::memory_order_relaxed);
    return false;
  }

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

  bool disarm_helper_restore_if_running(
    const std::function<bool()> &cancellation_predicate = {},
    const std::chrono::steady_clock::time_point operation_deadline =
      std::chrono::steady_clock::time_point::max()) {
    if (shutdown_requested() ||
        cancellation_requested(cancellation_predicate) ||
        operation_deadline_expired(operation_deadline)) {
      return false;
    }

    const bool helper_running = helper_process_running(
      cancellation_predicate,
      operation_deadline
    );
    if (cancellation_requested(cancellation_predicate) ||
        operation_deadline_expired(operation_deadline)) {
      return false;
    }
    if (!helper_running) {
      g_restore_expected.store(false, std::memory_order_relaxed);
      return false;
    }

    const bool restore_expected = g_restore_expected.load(std::memory_order_relaxed);
    const auto now_us = now_steady_us();
    const auto last_revert_us = g_last_revert_us.load(std::memory_order_relaxed);

    const auto last_attempt_us = g_last_disarm_attempt_us.load(std::memory_order_relaxed);

    // Don't spam DISARM frames (they share the helper's job/message queues with APPLY/REVERT).
    if ((now_us - last_attempt_us) < (kDisarmRetryThrottle.count() * 1000)) {
      const auto last_success_us = g_last_disarm_success_us.load(std::memory_order_relaxed);
      return (now_us - last_success_us) < (kDisarmRetryThrottle.count() * 1000);
    }

    // If we believe a restore loop is active, ensure we only issue one DISARM per restore generation unless it fails
    // and the throttle allows a retry.
    const auto restore_generation = g_restore_generation.load(std::memory_order_relaxed);
    if (restore_expected) {
      const auto disarmed_generation = g_disarm_generation_sent.load(std::memory_order_relaxed);
      if (disarmed_generation >= restore_generation) {
        const auto last_success_us = g_last_disarm_success_us.load(std::memory_order_relaxed);
        return (now_us - last_success_us) < (kDisarmRetryThrottle.count() * 1000);
      }
    }

    g_last_disarm_attempt_us.store(now_us, std::memory_order_relaxed);
    // The short recovery path intentionally makes one cache-only attempt. A
    // reconnect or a synchronous reset can take seconds and would defeat the
    // deadline that protects stream startup from a concurrent restore.
    const bool ok = platf::display_helper_client::send_disarm_restore_fast(
      static_cast<int>(kDisarmRestoreBudget.count()),
      operation_deadline);

    if (cancellation_requested(cancellation_predicate) ||
        operation_deadline_expired(operation_deadline)) {
      return false;
    }

    if (ok) {
      g_last_disarm_success_us.store(now_us, std::memory_order_relaxed);
      g_disarm_generation_sent.store(restore_generation, std::memory_order_relaxed);
      g_restore_expected.store(false, std::memory_order_relaxed);
      BOOST_LOG(info) << "Display helper: DISARM dispatched (fast).";
      return true;
    }

    // Fail-safe: if we recently initiated a helper restore, and DISARM couldn't be delivered quickly,
    // terminate the helper so restore activity stops immediately (prevents virtual display crash loops).
    const bool revert_recent = (now_us - last_revert_us) < (30LL * 1000LL * 1000LL);
    if (revert_recent) {
      BOOST_LOG(warning) << "Display helper: DISARM could not be delivered within "
                         << kDisarmRestoreBudget.count() << "ms; terminating helper to stop restore activity.";
      {
        std::unique_lock<std::mutex> lg(helper_mutex(), std::defer_lock);
        while (!lg.try_lock()) {
          if (!sleep_with_cancellation(
                std::chrono::milliseconds(25),
                cancellation_predicate,
                operation_deadline)) {
            return false;
          }
        }
        if (cancellation_requested(cancellation_predicate) ||
            operation_deadline_expired(operation_deadline)) {
          return false;
        }
        helper_proc().terminate();
      }
      g_restore_expected.store(false, std::memory_order_relaxed);
    }

    return false;
  }

  bool ensure_helper_started(
    bool force_restart,
    bool force_enable,
    const std::function<bool()> &cancellation_predicate,
    const std::chrono::steady_clock::time_point operation_deadline,
    const bool shutdown_class_caller) {
    if (!force_enable && !dd_feature_enabled()) {
      return false;
    }
    const bool shutting_down = shutdown_requested();
    std::unique_lock<std::mutex> lg(helper_mutex(), std::defer_lock);
    while (!lg.try_lock()) {
      if (!sleep_with_cancellation(
            std::chrono::milliseconds(25),
            cancellation_predicate,
            operation_deadline)) {
        return false;
      }
    }
    if (cancellation_requested(cancellation_predicate) ||
        operation_deadline_expired(operation_deadline)) {
      return false;
    }
    // Already started? Verify liveness to avoid stale or wedged state
    if (HANDLE h = helper_proc().get_process_handle(); h != nullptr) {
      BOOST_LOG(debug) << "Display helper: checking existing process handle...";
      DWORD wait = WaitForSingleObject(h, 0);
      if (wait == WAIT_TIMEOUT) {
        DWORD pid = GetProcessId(h);
        BOOST_LOG(debug) << "Display helper already running (pid=" << pid << ")";
        if (!force_restart) {
          // Check IPC liveness with a lightweight ping; if responsive, reuse existing helper
          bool ping_ok = false;
          for (int i = 0; i < 2 && !ping_ok; ++i) {
            if (cancellation_requested(cancellation_predicate) ||
                operation_deadline_expired(operation_deadline)) {
              return false;
            }
            ping_ok = cancellation_predicate ?
                        platf::display_helper_client::send_ping_cancellable(
                          shutdown_class_caller ? kShutdownHelperPingTimeoutMs : kHelperPingTimeoutMs,
                          cancellation_predicate,
                          operation_deadline) :
                        platf::display_helper_client::send_ping();
            if (!ping_ok &&
                !sleep_with_cancellation(
                  std::chrono::milliseconds(200),
                  cancellation_predicate,
                  operation_deadline)) {
              return false;
            }
          }
          if (ping_ok) {
            return true;
          }
          if (!platf::display_helper_client::reset_connection_cancellable(
                cancellation_predicate,
                operation_deadline)) {
            return false;
          }
          BOOST_LOG(warning) << "Display helper process ping failed; keeping existing instance and deferring restart.";
          note_helper_start_failure("failed ping");
          return false;
        }

        if (platf::display_helper_client::send_ping_fast(
              100,
              operation_deadline)) {
          BOOST_LOG(debug) << "Display helper hard restart skipped because existing helper accepted a fast ping.";
          return true;
        }
        if (!platf::display_helper_client::reset_connection_cancellable(
              cancellation_predicate,
              operation_deadline)) {
          return false;
        }
        BOOST_LOG(warning) << "Display helper hard restart requested because existing helper did not accept a fast ping.";

        BOOST_LOG(warning) << "Display helper: hard restart requested; terminating existing instance (pid=" << pid
                           << ") with no grace period.";
        if (!platf::display_helper_client::reset_connection_cancellable(
              cancellation_predicate,
              operation_deadline)) {
          return false;
        }
        helper_proc().terminate();

        DWORD wait_result = WAIT_TIMEOUT;
        if (!wait_for_process_with_cancellation(
              h,
              kHelperForceKillWaitMs,
              cancellation_predicate,
              wait_result,
              operation_deadline)) {
          return false;
        }
        if (wait_result == WAIT_OBJECT_0) {
          DWORD exit_code = 0;
          GetExitCodeProcess(h, &exit_code);
          BOOST_LOG(info) << "Display helper exited after forced termination (code=" << exit_code << ").";
        } else if (wait_result == WAIT_TIMEOUT) {
          BOOST_LOG(warning) << "Display helper: process did not exit within " << kHelperForceKillWaitMs
                             << " ms after termination request; continuing with cleanup.";
        } else {
          DWORD wait_err = GetLastError();
          BOOST_LOG(warning) << "Display helper: wait after termination failed (winerr=" << wait_err
                             << "); continuing with cleanup.";
        }

        // Small delay to reduce the chance of named pipe / mutex conflicts during rapid restart.
        if (!sleep_with_cancellation(
              std::chrono::milliseconds(100),
              cancellation_predicate,
              operation_deadline)) {
          return false;
        }
      } else {
        // Process exited; fall through to restart
        DWORD exit_code = 0;
        GetExitCodeProcess(h, &exit_code);
        BOOST_LOG(debug) << "Display helper process detected as exited (code=" << exit_code << "); preparing restart.";
      }
    }
    if (shutting_down ||
        cancellation_requested(cancellation_predicate) ||
        operation_deadline_expired(operation_deadline)) {
      return false;
    }

    if (helper_start_failure_cooldown_active()) {
      return false;
    }

    if (!kill_all_helper_processes(
          cancellation_predicate,
          operation_deadline)) {
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

    const bool allow_system_fallback = platf::is_running_as_system() && !user_session_ready();
    // Select the helper engine (legacy fallback vs v2) and propagate the log level.
    const bool legacy_engine = use_legacy_helper_engine();
    std::wstring helper_args = legacy_engine ? L"--engine=legacy" : L"--engine=v2";
    helper_args += L" --log-level=";
    helper_args += std::to_wstring(std::clamp(config::sunshine.min_log_level, 0, 6));
    statefile::save_display_helper_engine(legacy_engine ? "legacy" : "v2");
    BOOST_LOG(debug) << "Starting display helper: " << platf::to_utf8(helper.wstring())
                     << " " << platf::to_utf8(helper_args);
    if (cancellation_requested(cancellation_predicate) ||
        operation_deadline_expired(operation_deadline)) {
      return false;
    }
    bool started = helper_proc().start(helper.wstring(), helper_args, allow_system_fallback);
    if (!started && force_restart) {
      // If we were asked to hard-restart, tolerate a brief overlap window where the old
      // instance is still tearing down and retry quickly.
      for (int attempt = 0; attempt < 5 && !started; ++attempt) {
        if (!sleep_with_cancellation(
              std::chrono::milliseconds(150),
              cancellation_predicate,
              operation_deadline)) {
          return false;
        }
        started = helper_proc().start(helper.wstring(), helper_args, allow_system_fallback);
      }
    }
    if (!started) {
      BOOST_LOG(error) << "Failed to start display helper: " << platf::to_utf8(helper.wstring());
      note_helper_start_failure("process launch failure");
      return false;
    }

    HANDLE h = helper_proc().get_process_handle();
    if (!h) {
      BOOST_LOG(error) << "Display helper started but no process handle available";
      note_helper_start_failure("missing process handle");
      return false;
    }

    DWORD pid = GetProcessId(h);
    BOOST_LOG(info) << "Display helper successfully started (pid=" << pid << ")";

    // Give the helper process time to initialize and create its named pipe server
    // Check if it exits early (e.g., singleton mutex conflict from incomplete cleanup)
    for (int check = 0; check < 6; ++check) {
      if (cancellation_requested(cancellation_predicate) ||
          operation_deadline_expired(operation_deadline)) {
        return false;
      }
      const auto process_wait = operation_wait_slice(
        std::chrono::milliseconds(50),
        operation_deadline
      );
      if (process_wait <= std::chrono::milliseconds::zero()) {
        return false;
      }
      DWORD wait = WaitForSingleObject(
        h,
        static_cast<DWORD>(process_wait.count())
      );
      if (wait == WAIT_OBJECT_0) {
        DWORD exit_code = 0;
        GetExitCodeProcess(h, &exit_code);
        if (exit_code == 3) {
          BOOST_LOG(warning) << "Display helper exited immediately with code 3 (singleton conflict). "
                             << "Retrying after extended cleanup delay...";
          if (!sleep_with_cancellation(
                std::chrono::milliseconds(1000),
                cancellation_predicate,
                operation_deadline)) {
            return false;
          }

          const bool retry_started = helper_proc().start(helper.wstring(), helper_args, allow_system_fallback);
          if (!retry_started) {
            BOOST_LOG(error) << "Display helper retry start failed";
            note_helper_start_failure("singleton retry launch failure");
            return false;
          }
          h = helper_proc().get_process_handle();
          if (h) {
            pid = GetProcessId(h);
            BOOST_LOG(info) << "Display helper retry succeeded (pid=" << pid << ")";
            if (!sleep_with_cancellation(
                  std::chrono::milliseconds(300),
                  cancellation_predicate,
                  operation_deadline)) {
              return false;
            }
          }
          break;
        } else {
          BOOST_LOG(error) << "Display helper exited unexpectedly with code " << exit_code;
          note_helper_start_failure("unexpected process exit");
          return false;
        }
      }
    }

    // Final initialization delay for pipe server creation
    if (!sleep_with_cancellation(
          std::chrono::milliseconds(200),
          cancellation_predicate,
          operation_deadline)) {
      return false;
    }
    const bool ipc_ready = wait_for_helper_ipc_ready_locked(
      cancellation_predicate,
      operation_deadline,
      shutdown_class_caller
    );
    if (ipc_ready) {
      note_helper_start_success();
    } else {
      note_helper_start_failure("IPC readiness timeout");
    }
    if (ipc_ready && !legacy_engine && !cancellation_predicate) {
      // Keep the v2 helper's log verbosity in sync with Sunshine (legacy would
      // log "Unknown message type" for this frame).
      (void) platf::display_helper_client::send_log_level(std::clamp(config::sunshine.min_log_level, 0, 6));
    }
    return ipc_ready;
  }

  // Watchdog state for helper liveness during active streams. start/stop are
  // called from many threads (RTSP/WebRTC session end, app termination,
  // paused-session cleanup, hotkeys, and process shutdown). A stopping worker
  // remains owned here until a non-worker caller joins it: detaching it would
  // let it access helper globals after CRT teardown.
  enum class watchdog_state_e {
    stopped,
    running,
    stopping,
  };

  static std::mutex g_watchdog_mutex;
  static std::condition_variable g_watchdog_cv;
  static watchdog_state_e g_watchdog_state = watchdog_state_e::stopped;
  static std::jthread g_watchdog_thread;
  static std::thread::id g_watchdog_worker_id;
  static bool g_watchdog_reaping = false;
  static bool g_watchdog_teardown_completed = false;
  // The watchdog is global, but its cleanup belongs to the launch whose
  // display state it observed. This prevents an old stream's stop from
  // erasing a newer launch that has already applied its display request.
  static std::optional<std::uint64_t> g_watchdog_session_generation;
  static std::uint64_t g_watchdog_stop_epoch = 0;
  static std::chrono::steady_clock::time_point g_last_vd_reenable {};

  constexpr auto kVirtualDisplayReenableCooldown = std::chrono::seconds(3);

  static bool stream_is_active_or_pending() {
    return stream::session::running_sessions.load(std::memory_order_acquire) != 0 ||
           webrtc_stream::has_active_or_pending_sessions();
  }

  static void adopt_watchdog_session_generation(std::uint64_t generation) {
    std::lock_guard<std::mutex> lock(g_watchdog_mutex);
    if (g_watchdog_state == watchdog_state_e::running && !g_watchdog_reaping) {
      g_watchdog_session_generation = generation;
      g_watchdog_teardown_completed = false;
    }
  }

  bool recently_reenabled_virtual_display() {
    if (g_last_vd_reenable.time_since_epoch().count() == 0) {
      return false;
    }
    return (std::chrono::steady_clock::now() - g_last_vd_reenable) < kVirtualDisplayReenableCooldown;
  }

  [[maybe_unused]] void explicit_virtual_display_reset_and_apply(
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
    std::uint64_t generation = 0;
    {
      std::lock_guard<std::mutex> lg(g_session_mutex);
      const bool same_session =
        g_active_session_dd &&
        g_active_session_dd->launch_session_id == session.id &&
        g_active_session_dd->session_unique_id == session.unique_id &&
        g_active_session_dd->session_client_uuid == session.client_uuid;
      generation = same_session ?
                     g_active_session_dd->generation :
                     ++g_next_active_session_generation;
      const auto refresh_millihz = rtsp_stream::effective_display_refresh_millihz(session);
      const int effective_fps = fps_override ? *fps_override : static_cast<int>(std::min<std::uint32_t>(
        refresh_millihz,
        static_cast<std::uint32_t>(std::numeric_limits<int>::max())
      ));
      g_active_session_dd = session_dd_fields_t {
        .generation = generation,
        .launch_session_id = session.id,
        .session_unique_id = session.unique_id,
        .session_client_uuid = session.client_uuid,
        .width = width_override ? *width_override : session.width,
        .height = height_override ? *height_override : session.height,
        .fps = effective_fps,
        .refresh_millihz = refresh_millihz,
        .enable_hdr = rtsp_stream::effective_hdr_requested(session),
        .enable_sops = session.enable_sops,
        .virtual_display = virtual_display_override ? *virtual_display_override : session.virtual_display,
        .virtual_display_device_id = device_id_override ? *device_id_override : session.virtual_display_device_id,
        .framegen_refresh_rate = framegen_refresh_override ? framegen_refresh_override : session.framegen_refresh_rate,
        .framegen_refresh_millihz = session.framegen_refresh_millihz,
        .framegen_refresh_multiplier = session.framegen_refresh_multiplier,
        .gen1_framegen_fix = session.gen1_framegen_fix,
        .gen2_framegen_fix = session.gen2_framegen_fix,
      };
      if (!g_active_session_dd->virtual_display_device_id.empty()) {
        // Persist so the helper (including the boot-time restore task) can exclude this
        // device from snapshots/baselines even when its EDID is not classifiable.
        statefile::remember_virtual_display_device(g_active_session_dd->virtual_display_device_id);
      }
    }
  }

  [[maybe_unused]] static std::optional<session_dd_fields_t> get_active_session_copy() {
    std::lock_guard<std::mutex> lg(g_session_mutex);
    return g_active_session_dd;
  }

  static std::optional<std::uint64_t> active_session_generation() {
    std::lock_guard<std::mutex> lg(g_session_mutex);
    if (!g_active_session_dd) {
      return std::nullopt;
    }
    return g_active_session_dd->generation;
  }

  static std::optional<std::uint64_t> active_session_generation_for(
    const rtsp_stream::launch_session_t &session
  ) {
    std::lock_guard<std::mutex> lg(g_session_mutex);
    if (!g_active_session_dd ||
        g_active_session_dd->launch_session_id != session.id ||
        g_active_session_dd->session_unique_id != session.unique_id ||
        g_active_session_dd->session_client_uuid != session.client_uuid) {
      return std::nullopt;
    }
    return g_active_session_dd->generation;
  }

  static void clear_active_session() {
    std::lock_guard<std::mutex> lg(g_session_mutex);
    g_active_session_dd.reset();
  }

  // Called while g_watchdog_mutex is held. Keeping the session mutex through
  // the connection reset prevents a newly applied launch from being cleared by
  // a watchdog that belonged to the previous launch.
  static bool reset_connection_and_clear_active_session_if_generation(
    const std::optional<std::uint64_t> &expected_generation
  ) {
    std::lock_guard<std::mutex> lg(g_session_mutex);
    if (g_active_session_dd &&
        (!expected_generation || g_active_session_dd->generation != *expected_generation)) {
      BOOST_LOG(debug) << "Display helper: skipping stale watchdog cleanup for session generation "
                       << (expected_generation ? std::to_string(*expected_generation) : std::string {"none"})
                       << "; active generation is " << g_active_session_dd->generation << '.';
      return false;
    }
    if (config::video.dd.config_revert_on_disconnect) {
      platf::display_helper_client::reset_connection();
    }
    g_active_session_dd.reset();
    return true;
  }

  // Requires g_watchdog_mutex. A stopped watchdog must finish its IPC/session
  // teardown before another generation can begin, otherwise an old disconnect
  // can erase the newly started stream's active-session state.
  static void finish_watchdog_stop_locked(const std::optional<std::uint64_t> &expected_generation) {
    if (g_watchdog_teardown_completed) {
      return;
    }
    (void) reset_connection_and_clear_active_session_if_generation(expected_generation);
    g_watchdog_teardown_completed = true;
  }

  std::optional<std::string> build_helper_apply_payload(
    const display_helper_integration::DisplayApplyRequest &request,
    display_helper_integration::ApplyRetryPolicy retry_policy) {
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

    if (!request.topology.device_refresh_rate_overrides.empty()) {
      nlohmann::json overrides = nlohmann::json::object();
      for (const auto &[device_id, rate] : request.topology.device_refresh_rate_overrides) {
        overrides[device_id] = {{"num", rate.first}, {"den", rate.second}};
      }
      j["sunshine_device_refresh_rate_overrides"] = std::move(overrides);
    }

    // Pass golden-first restore preference to helper
    if (config::video.dd.always_restore_from_golden) {
      j["sunshine_always_restore_from_golden"] = true;
    }
    j["sunshine_restore_on_disconnect"] = config::video.dd.config_revert_on_disconnect;
    if (retry_policy == display_helper_integration::ApplyRetryPolicy::StreamStart) {
      j["sunshine_omit_final_initial_hdr_reapply"] = true;
    }

    // Always carry the exclusion list: a hard-restarted helper has no SNAPSHOT_CURRENT
    // context and would otherwise capture virtual displays into its pre-apply baseline.
    try {
      j["sunshine_snapshot_exclude_devices"] = effective_snapshot_exclude_devices();
    } catch (...) {
    }

    return j.dump();
  }

  std::string build_revert_payload(bool prefer_golden_if_current_missing) {
    nlohmann::json j = nlohmann::json::object();
    j["sunshine_prefer_golden_if_current_missing"] = prefer_golden_if_current_missing;
    j["sunshine_always_restore_from_golden"] = config::video.dd.always_restore_from_golden;
    return j.dump();
  }

  static void watchdog_proc(std::stop_token st) {
    using namespace std::chrono_literals;
    constexpr auto kActiveInterval = 5s;
    constexpr auto kSuspendedInterval = 20s;
    bool helper_ready = false;

    auto sleep_interruptible = [&st](std::chrono::milliseconds interval) {
      for (auto slept = 0ms; slept < interval && !st.stop_requested(); slept += 100ms) {
        std::this_thread::sleep_for(100ms);
      }
    };
    auto reset_connection_noexcept = []() noexcept {
      try {
        platf::display_helper_client::reset_connection();
      } catch (...) {
      }
    };

    while (!st.stop_requested()) {
      try {
        if (!dd_feature_enabled()) {
          if (helper_ready) {
            platf::display_helper_client::reset_connection();
            helper_ready = false;
          }
          sleep_interruptible(kActiveInterval);
          continue;
        }

        if (!helper_ready) {
          helper_ready = ensure_helper_started();
          if (!helper_ready) {
            sleep_interruptible(kActiveInterval);
            continue;
          }
          (void) platf::display_helper_client::send_ping();
        }

        // This worker only needs a snapshot to select its polling interval.
        // session_count() reaps STOPPING sessions, whose join path can call
        // stop_watchdog() and make this worker attempt to join itself. For the
        // same reason read the app id instead of running(): running() can drive a
        // deferred launch and call terminate(), which this worker must not do.
        const bool suspended =
          (rtsp_stream::session_count_no_cleanup() == 0) &&
          (proc::proc.current_app_id() > 0);
        const auto interval = suspended ? kSuspendedInterval : kActiveInterval;
        sleep_interruptible(interval);
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
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Display helper watchdog failed: " << e.what();
        reset_connection_noexcept();
        helper_ready = false;
        sleep_interruptible(kActiveInterval);
      } catch (...) {
        BOOST_LOG(error) << "Display helper watchdog failed with an unknown exception.";
        reset_connection_noexcept();
        helper_ready = false;
        sleep_interruptible(kActiveInterval);
      }
    }
  }

}  // namespace

namespace display_helper_integration {
  namespace {
    bool apply_internal(
      const DisplayApplyRequest &request,
      bool allow_resolution_deferral,
      ApplyVerificationTicket *verification_ticket,
      const std::function<bool()> &cancellation_predicate = {},
      ApplyRetryPolicy retry_policy = ApplyRetryPolicy::Full,
      std::chrono::steady_clock::time_point startup_deadline = {},
      bool shutdown_class_caller = false) {
      const auto verification_timeout =
        retry_policy == ApplyRetryPolicy::StreamStart ?
          kStreamStartApplyVerificationTimeout :
          kApplyVerificationTimeout;
      if (startup_deadline == std::chrono::steady_clock::time_point {}) {
        startup_deadline = std::chrono::steady_clock::now() + verification_timeout;
      }
      // Publish the budget before any helper work so the capture thread's own
      // post-APPLY settling waits share this deadline rather than starting a
      // fresh window after it has expired. An unbounded APPLY clears it.
      g_stream_start_deadline_us.store(
        retry_policy == ApplyRetryPolicy::StreamStart ?
          std::chrono::duration_cast<std::chrono::microseconds>(startup_deadline.time_since_epoch()).count() :
          0,
        std::memory_order_release
      );
      const std::function<bool()> startup_cancellation_predicate = [&]() {
        return cancellation_requested(cancellation_predicate) ||
               (retry_policy == ApplyRetryPolicy::StreamStart &&
                std::chrono::steady_clock::now() >= startup_deadline);
      };
      if (verification_ticket) {
        *verification_ticket = {};
        verification_ticket->startup_deadline = startup_deadline;
      }
      if (startup_cancellation_predicate()) {
        return false;
      }
      if (request.action == DisplayApplyAction::Skip) {
        BOOST_LOG(info) << "Display helper: configuration parse failed; not dispatching.";
        return false;
      }

      if (request.action == DisplayApplyAction::Revert) {
        invalidate_apply_verification();
        const bool helper_ready = ensure_helper_started(
          false,
          true,
          startup_cancellation_predicate,
          startup_deadline,
          shutdown_class_caller
        );
        if (!helper_ready) {
          BOOST_LOG(warning) << "Display helper: REVERT skipped (helper not reachable).";
          clear_active_session();
          return false;
        }
        BOOST_LOG(info) << "Display helper: sending REVERT request (builder).";
        const bool ok =
          retry_policy == ApplyRetryPolicy::StreamStart ?
            platf::display_helper_client::send_revert_within(
              {},
              startup_deadline,
              startup_cancellation_predicate
            ) :
            platf::display_helper_client::send_revert();
        BOOST_LOG(info) << "Display helper: REVERT dispatch result=" << (ok ? "true" : "false");
        clear_active_session();
        return ok;
      }

      if (request.action != DisplayApplyAction::Apply) {
        return false;
      }

      // Invalidate verification from any older APPLY before starting work. This
      // prevents capture from treating the previous generation as proof while a
      // newer topology change is in flight.
      const auto apply_generation = g_last_apply_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
      if (verification_ticket) {
        verification_ticket->generation = apply_generation;
      }
      const bool exclusive_virtual =
        request.virtual_display_arrangement == VirtualDisplayArrangement::Exclusive;
      const bool source_hdr_explicitly_disabled =
        request.configuration && request.configuration->m_hdr_state == display_device::HdrState::Disabled;
      g_capture_stable_eligible_apply_generation.store(
        exclusive_virtual && source_hdr_explicitly_disabled ? apply_generation : 0,
        std::memory_order_release
      );
      const bool source_hdr_requested =
        request.configuration && request.configuration->m_hdr_state == display_device::HdrState::Enabled;
      g_hdr_requested_apply_generation.store(
        source_hdr_requested ? apply_generation : 0,
        std::memory_order_release
      );

      // Prefer the helper for APPLY, even when running as SYSTEM without an interactive user session.
      // In-process display APIs frequently return ERROR_ACCESS_DENIED in that context.
      const bool system_no_user_session = platf::is_running_as_system() && !user_session_ready();
      if (system_no_user_session) {
        BOOST_LOG(debug) << "Display helper: SYSTEM context without user session; preferring helper dispatch.";
      }

      // Stream-start policy: if a helper is already running, hard-restart it immediately
      // rather than attempting graceful STOP (avoids apply timeouts and wedged restore loops).
      // Exception: if we recently asked the helper to restore and did not cancel it inside
      // the short disarm grace window, keep that helper alive and let APPLY supersede the
      // restore through IPC. Killing it here can strand the host in a partially restored
      // physical-display mode when a monitor input is still switched away.
      // In SYSTEM/no-user-session mode we still keep hard restart to recover stale pipe state,
      // but we avoid in-process display API fallback if helper IPC remains unavailable.
      const bool restore_expected =
        restore_expected_with_live_helper(
          startup_cancellation_predicate,
          startup_deadline
        );
      const bool hard_restart = (request.session != nullptr) && !restore_expected;
      if (request.session && restore_expected) {
        BOOST_LOG(info) << "Display helper: reusing existing helper because an unconfirmed restore is pending; APPLY will supersede it.";
      }

      bool helper_ready = ensure_helper_started(
        hard_restart,
        true,
        startup_cancellation_predicate,
        startup_deadline,
        shutdown_class_caller
      );
      if (!helper_ready && hard_restart) {
        if (startup_cancellation_predicate()) {
          return false;
        }
        BOOST_LOG(warning) << "Display helper: hard restart path unavailable; retrying helper start without restart.";
        helper_ready = ensure_helper_started(
          false,
          true,
          startup_cancellation_predicate,
          startup_deadline,
          shutdown_class_caller
        );
      }
      if (!helper_ready) {
        if (startup_cancellation_predicate()) {
          return false;
        }
        helper_ready = ensure_helper_started(
          hard_restart,
          true,
          startup_cancellation_predicate,
          startup_deadline,
          shutdown_class_caller
        );
      }

      if (startup_cancellation_predicate()) {
        return false;
      }

      if (helper_ready) {
        auto payload = build_helper_apply_payload(request, retry_policy);
        if (!payload) {
          BOOST_LOG(error) << "Display helper: failed to build APPLY payload for helper dispatch.";
          return false;
        }

        BOOST_LOG(info) << "Display helper: sending APPLY request via helper.";
        std::uint64_t helper_apply_request_id = 0;
        std::uint64_t client_wait_generation = 0;
        std::uint64_t connection_generation = 0;
        const auto remaining_apply_budget = std::chrono::duration_cast<std::chrono::milliseconds>(
          startup_deadline - std::chrono::steady_clock::now());
        if (remaining_apply_budget <= std::chrono::milliseconds::zero()) {
          BOOST_LOG(warning) << "Display helper: APPLY budget expired before dispatch.";
          return false;
        }
        const bool ok = platf::display_helper_client::send_apply_json(
          *payload,
          &helper_apply_request_id,
          &client_wait_generation,
          &connection_generation,
          startup_cancellation_predicate,
          static_cast<int>(remaining_apply_budget.count()),
          shutdown_class_caller);
        BOOST_LOG(info) << "Display helper: APPLY dispatch result=" << (ok ? "true" : "false");
        if (ok && startup_cancellation_predicate()) {
          BOOST_LOG(debug) << "Display helper: APPLY completion was cancelled before its session state was published.";
          return false;
        }
        // The client identifies the live helper protocol from its ApplyResult.
        // A non-zero id means this specific connection confirmed v2's
        // token/verification protocol; an untagged legacy acknowledgement
        // intentionally preserves v1's synchronous completion behavior.
        if (verification_ticket && ok && helper_apply_request_id != 0) {
          verification_ticket->uses_v2_helper = true;
          verification_ticket->helper_request_id = helper_apply_request_id;
          verification_ticket->client_wait_generation = client_wait_generation;
          verification_ticket->connection_generation = connection_generation;
        }
        if (ok && request.session) {
          if (startup_cancellation_predicate()) {
            BOOST_LOG(debug) << "Display helper: APPLY session-state publication was cancelled.";
            return false;
          }
          g_restore_expected.store(false, std::memory_order_relaxed);
          note_successful_apply();
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
        if (!ok && startup_cancellation_predicate()) {
          return false;
        }
        if (!ok && allow_resolution_deferral && request.session && platf::is_lock_screen_active()) {
          BOOST_LOG(info) << "Display helper: APPLY failed during lock screen; queuing deferred apply for retry after unlock.";
          queue_deferred_resolution_apply(request);
        }
        return ok;
      }

      if (system_no_user_session) {
        BOOST_LOG(warning) << "Display helper: helper unavailable in SYSTEM context without user session; skipping in-process APPLY fallback.";
        maybe_queue_deferred_resolution_apply(request, allow_resolution_deferral);
        return false;
      }

      if (cancellation_predicate) {
        BOOST_LOG(debug) << "Display helper: recovery APPLY will not fall back to synchronous in-process display APIs.";
        return false;
      }
      if (retry_policy == ApplyRetryPolicy::StreamStart) {
        BOOST_LOG(warning) << "Display helper: bounded stream-start APPLY will not enter the unbounded in-process fallback.";
        return false;
      }

      BOOST_LOG(warning) << "Display helper: helper unavailable; falling back to in-process APPLY.";

      if (!request.session) {
        BOOST_LOG(error) << "Display helper: missing session context for in-process APPLY.";
        return false;
      }

      const auto apply_result = apply_in_process(request);
      if (apply_result != display_device::SettingsManagerInterface::ApplyResult::Ok) {
        if (apply_result == display_device::SettingsManagerInterface::ApplyResult::ApiTemporarilyUnavailable) {
          maybe_queue_deferred_resolution_apply_on_api_unavailable(request);
        }
        BOOST_LOG(warning) << "Display helper: in-process APPLY failed.";
        return false;
      }

      const auto device_id = request.configuration ? request.configuration->m_device_id : std::string {};
      if (!verify_helper_topology(*request.session, device_id)) {
        BOOST_LOG(warning) << "Display helper: topology verification failed after in-process APPLY.";
      }
      (void) apply_topology_definition(request.topology, "in-process");

      note_successful_apply();
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
      maybe_queue_deferred_resolution_apply(request, allow_resolution_deferral);
      return true;
    }
  }  // namespace

  ApplyVerificationStatus wait_for_apply_verification(
    const ApplyVerificationTicket &ticket,
    std::chrono::milliseconds timeout) {
    // Legacy success is acknowledged only after its synchronous verification,
    // but it does not emit a separately attributable VerificationResult frame.
    if (!ticket.uses_v2_helper || ticket.generation == 0 || ticket.helper_request_id == 0 ||
        ticket.client_wait_generation == 0 || ticket.connection_generation == 0) {
      return ApplyVerificationStatus::Unknown;
    }

    auto effective_timeout = std::max(timeout, std::chrono::milliseconds::zero());
    if (ticket.startup_deadline != std::chrono::steady_clock::time_point {}) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        ticket.startup_deadline - std::chrono::steady_clock::now());
      if (remaining <= std::chrono::milliseconds::zero()) {
        BOOST_LOG(warning) << "Display helper: verification skipped because the shared stream-start budget expired.";
        return ApplyVerificationStatus::Unknown;
      }
      effective_timeout = std::min(effective_timeout, remaining);
    }
    const int timeout_ms = static_cast<int>(effective_timeout.count());
    const auto result = platf::display_helper_client::wait_for_verification_result(
      timeout_ms,
      [generation = ticket.generation] {
        return g_last_apply_generation.load(std::memory_order_acquire) != generation;
      },
      ticket.helper_request_id,
      ticket.client_wait_generation,
      ticket.connection_generation
    );
    if (!result.has_value()) {
      if (g_last_apply_generation.load(std::memory_order_acquire) != ticket.generation) {
        BOOST_LOG(debug) << "Display helper: verification gate superseded by a newer APPLY.";
      } else {
        BOOST_LOG(warning) << "Display helper: verification result unavailable; proceeding with stream.";
      }
      return ApplyVerificationStatus::Unknown;
    }

    if (*result && g_last_apply_generation.load(std::memory_order_acquire) == ticket.generation) {
      g_last_verified_apply_generation.store(ticket.generation, std::memory_order_release);
      return ApplyVerificationStatus::Verified;
    }
    return *result ? ApplyVerificationStatus::Unknown : ApplyVerificationStatus::Failed;
  }

  bool last_apply_is_capture_stable() {
    const auto before = g_last_apply_generation.load(std::memory_order_acquire);
    const auto verified = g_last_verified_apply_generation.load(std::memory_order_acquire);
    const auto eligible = g_capture_stable_eligible_apply_generation.load(std::memory_order_acquire);
    const auto after = g_last_apply_generation.load(std::memory_order_acquire);
    return before != 0 && before == after && verified == after && eligible == after;
  }

  bool last_apply_requested_hdr() {
    const auto before = g_last_apply_generation.load(std::memory_order_acquire);
    const auto hdr = g_hdr_requested_apply_generation.load(std::memory_order_acquire);
    const auto after = g_last_apply_generation.load(std::memory_order_acquire);
    return before != 0 && before == after && hdr == after;
  }

  std::optional<std::chrono::milliseconds> remaining_stream_start_budget() {
    const auto deadline_us = g_stream_start_deadline_us.load(std::memory_order_acquire);
    if (deadline_us == 0) {
      return std::nullopt;
    }
    const auto remaining_us = deadline_us - now_steady_us();
    return std::chrono::milliseconds(remaining_us > 0 ? remaining_us / 1000 : 0);
  }

  bool apply(
    const DisplayApplyRequest &request,
    ApplyVerificationTicket *verification_ticket,
    std::function<bool()> cancellation_predicate,
    ApplyRetryPolicy retry_policy,
    std::chrono::steady_clock::time_point startup_deadline,
    const bool shutdown_class_caller) {
    const auto verification_timeout =
      retry_policy == ApplyRetryPolicy::StreamStart ?
        kStreamStartApplyVerificationTimeout :
        kApplyVerificationTimeout;
    if (startup_deadline == std::chrono::steady_clock::time_point {}) {
      startup_deadline = std::chrono::steady_clock::now() + verification_timeout;
    }
    const std::function<bool()> startup_cancellation_predicate = [&]() {
      return cancellation_requested(cancellation_predicate) ||
             (retry_policy == ApplyRetryPolicy::StreamStart &&
              std::chrono::steady_clock::now() >= startup_deadline);
    };
    if (startup_cancellation_predicate()) {
      return false;
    }
    std::unique_lock<std::mutex> execution_lock(pending_apply_execution_mutex(), std::defer_lock);
    if (!lock_pending_apply_execution(
          execution_lock,
          startup_cancellation_predicate,
          startup_deadline)) {
      return false;
    }
    clear_pending_apply_queue_locked();

    // Remember the session's virtual display before the APPLY payload is built so the
    // helper can exclude it from the pre-apply baseline it may capture.
    if (request.session) {
      const auto &vd_id = request.session_overrides.device_id_override ?
                            *request.session_overrides.device_id_override :
                            request.session->virtual_display_device_id;
      if (!vd_id.empty()) {
        statefile::remember_virtual_display_device(vd_id);
      }
    }
    return apply_internal(
      request,
      true,
      verification_ticket,
      cancellation_predicate,
      retry_policy,
      startup_deadline,
      shutdown_class_caller
    );
  }

  bool revert(bool prefer_golden_if_current_missing) {
    std::unique_lock<std::mutex> execution_lock(pending_apply_execution_mutex());
    invalidate_apply_verification();
    clear_pending_apply_queue_locked();
    if (!ensure_helper_started()) {
      BOOST_LOG(info) << "Display helper unavailable; cannot send revert.";
      return false;
    }
    BOOST_LOG(info) << "Display helper: sending REVERT request"
                    << (prefer_golden_if_current_missing ? " (prefer golden if current missing)." : ".");
    const bool ok = platf::display_helper_client::send_revert(build_revert_payload(prefer_golden_if_current_missing));
    BOOST_LOG(info) << "Display helper: REVERT dispatch result=" << (ok ? "true" : "false");
    if (ok) {
      g_restore_expected.store(true, std::memory_order_relaxed);
      g_last_revert_us.store(now_steady_us(), std::memory_order_relaxed);
      g_restore_generation.fetch_add(1, std::memory_order_relaxed);
    }
    clear_active_session();
    return ok;
  }

  bool disarm_pending_restore(
    std::function<bool()> cancellation_predicate,
    const std::chrono::steady_clock::time_point operation_deadline) {
    if ((cancellation_predicate && cancellation_predicate()) ||
        operation_deadline_expired(operation_deadline)) {
      return false;
    }
    invalidate_apply_verification();
    return disarm_helper_restore_if_running(
      cancellation_predicate,
      operation_deadline
    );
  }

  bool restore_in_progress(
    std::function<bool()> cancellation_predicate,
    const std::chrono::steady_clock::time_point operation_deadline) {
    if ((cancellation_predicate && cancellation_predicate()) ||
        operation_deadline_expired(operation_deadline)) {
      return false;
    }
    return restore_expected_with_live_helper(
      cancellation_predicate,
      operation_deadline
    );
  }

  bool export_golden_restore() {
    if (!ensure_helper_started()) {
      BOOST_LOG(info) << "Display helper unavailable; cannot export golden snapshot.";
      return false;
    }
    BOOST_LOG(info) << "Display helper: sending EXPORT_GOLDEN request.";
    const bool ok = platf::display_helper_client::send_export_golden(build_snapshot_exclude_payload());
    BOOST_LOG(info) << "Display helper: EXPORT_GOLDEN dispatch result=" << (ok ? "true" : "false");
    return ok;
  }

  bool reset_persistence() {
    invalidate_apply_verification();
    if (!ensure_helper_started()) {
      BOOST_LOG(info) << "Display helper unavailable; cannot reset persistence.";
      return false;
    }
    BOOST_LOG(info) << "Display helper: sending RESET request.";
    const bool ok = platf::display_helper_client::send_reset();
    BOOST_LOG(info) << "Display helper: RESET dispatch result=" << (ok ? "true" : "false");
    return ok;
  }

  bool snapshot_current_display_state(
    std::function<bool()> cancellation_predicate,
    const std::chrono::steady_clock::time_point operation_deadline) {
    if (cancellation_requested(cancellation_predicate) ||
        operation_deadline_expired(operation_deadline)) {
      return false;
    }
    if (restore_expected_with_live_helper(
          cancellation_predicate,
          operation_deadline)) {
      BOOST_LOG(info) << "Display helper: skipping SNAPSHOT_CURRENT while an unconfirmed restore is pending.";
      return false;
    }

    if (!ensure_helper_started(
          false,
          false,
          cancellation_predicate,
          operation_deadline)) {
      BOOST_LOG(info) << "Display helper unavailable; cannot snapshot current display state.";
      return false;
    }
    BOOST_LOG(info) << "Display helper: sending SNAPSHOT_CURRENT request.";
    const auto payload = build_snapshot_exclude_payload();
    // Wire behavior is selected from the helper that actually answered APPLY,
    // not from a configuration value that may have changed while a helper was
    // being reused. An unknown connection dispatches in pipe order; APPLY has
    // its own pre-apply baseline fallback if that snapshot is not yet saved.
    const bool bounded =
      operation_deadline != std::chrono::steady_clock::time_point::max();
    const bool v2_helper =
      !bounded &&
      platf::display_helper_client::uses_v2_response_protocol();
    const bool ok =
      bounded ?
        platf::display_helper_client::send_snapshot_current_within(
          payload,
          operation_deadline,
          cancellation_predicate
        ) :
        (v2_helper ?
           platf::display_helper_client::send_snapshot_current_and_wait(payload) :
           platf::display_helper_client::send_snapshot_current(payload));
    BOOST_LOG(info) << "Display helper: SNAPSHOT_CURRENT "
                    << (bounded ? "bounded operation" : (v2_helper ? "completion" : "dispatch"))
                    << " result=" << (ok ? "true" : "false");
    return ok;
  }

  bool apply_pending_if_ready(std::function<bool()> cancellation_predicate) {
    const auto cancelled = [&] {
      return cancellation_predicate && cancellation_predicate();
    };
    if (cancelled()) {
      return false;
    }

    // A deferred APPLY must not outlive a normal APPLY, REVERT, or session
    // teardown. Keep ownership of the display operation until its IPC and
    // active-session update are complete.
    std::unique_lock<std::mutex> execution_lock(pending_apply_execution_mutex(), std::defer_lock);
    if (!lock_pending_apply_execution(execution_lock, cancellation_predicate)) {
      return false;
    }
    bool check_idle_stop_after_apply = false;
    if (cancelled()) {
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(pending_apply_mutex());
      if (!pending_apply_state()) {
        return false;
      }
    }

    if (cancelled() || (platf::is_running_as_system() && !user_session_ready())) {
      return false;
    }

    const auto now = std::chrono::steady_clock::now();
    PendingApplyState pending;
    bool stream_was_live_when_claimed = false;
    {
      std::lock_guard<std::mutex> lock(pending_apply_mutex());
      if (cancelled()) {
        return false;
      }
      if (!pending_apply_state()) {
        return false;
      }
      auto &state = *pending_apply_state();
      if (!state.ready_since) {
        state.ready_since = now;
        state.next_attempt = now + kDeferredApplyInitialDelay;
        BOOST_LOG(info) << "Display helper: user session detected; delaying deferred APPLY for "
                        << kDeferredApplyInitialDelay.count() << "ms.";
        return false;
      }
      if (now < state.next_attempt) {
        return false;
      }
      if (state.attempts >= kMaxDeferredApplyAttempts) {
        BOOST_LOG(warning) << "Display helper: deferred APPLY exceeded retry limit; giving up on session "
                           << state.session_id << ".";
        pending_apply_state().reset();
        return false;
      }
      // The RTSP join path decrements its live-session count before it clears
      // deferred work. Do not claim an orphaned request in that window: leave
      // it for teardown to remove rather than publishing stale display state.
      stream_was_live_when_claimed = stream_is_active_or_pending();
      if (!stream_was_live_when_claimed) {
        return false;
      }
      pending = state;
      pending_apply_state().reset();
    }

    if (cancelled()) {
      std::lock_guard<std::mutex> lock(pending_apply_mutex());
      if (!pending_apply_state()) {
        pending.request.session = nullptr;
        pending_apply_state() = std::move(pending);
      }
      return false;
    }

    std::optional<rtsp_stream::launch_session_t> session;
    if (pending.has_session) {
      rtsp_stream::launch_session_t snapshot {};
      snapshot.id = pending.session_snapshot.id;
      snapshot.unique_id = pending.session_snapshot.unique_id;
      snapshot.client_uuid = pending.session_snapshot.client_uuid;
      snapshot.width = pending.session_snapshot.width;
      snapshot.height = pending.session_snapshot.height;
      snapshot.fps = pending.session_snapshot.fps;
      snapshot.client_display_mode_override = pending.session_snapshot.client_display_mode_override;
      snapshot.client_display_refresh_millihz = pending.session_snapshot.client_display_refresh_millihz;
      snapshot.client_virtual_display_override = pending.session_snapshot.client_virtual_display_override;
      snapshot.enable_hdr = pending.session_snapshot.enable_hdr;
      snapshot.enable_sops = pending.session_snapshot.enable_sops;
      snapshot.virtual_display = pending.session_snapshot.virtual_display;
      snapshot.resolution_override = pending.session_snapshot.resolution_override;
      snapshot.virtual_display_device_id = pending.session_snapshot.virtual_display_device_id;
      snapshot.virtual_display_ready_since = pending.session_snapshot.virtual_display_ready_since;
      snapshot.virtual_display_hdr_enabled = pending.session_snapshot.virtual_display_hdr_enabled;
      snapshot.framegen_refresh_rate = pending.session_snapshot.framegen_refresh_rate;
      snapshot.framegen_refresh_millihz = pending.session_snapshot.framegen_refresh_millihz;
      snapshot.framegen_refresh_multiplier = pending.session_snapshot.framegen_refresh_multiplier;
      snapshot.gen1_framegen_fix = pending.session_snapshot.gen1_framegen_fix;
      snapshot.gen2_framegen_fix = pending.session_snapshot.gen2_framegen_fix;
      session = std::move(snapshot);
      pending.request.session = &*session;
    } else {
      pending.request.session = nullptr;
    }

    BOOST_LOG(info) << "Display helper: applying deferred configuration for session " << pending.session_id << ".";
    // This entry point's predicate belongs to an owned shutdown worker by
    // contract, so a caller that supplies one is shutdown-class.
    const bool ok = apply_internal(
      pending.request,
      false,
      nullptr,
      cancellation_predicate,
      ApplyRetryPolicy::Full,
      {},
      static_cast<bool>(cancellation_predicate)
    );
    if (ok && stream_was_live_when_claimed && session) {
      if (const auto generation = active_session_generation_for(*session)) {
        // This retry may complete after start_watchdog() observed no active
        // descriptor (for example while the lock screen deferred APPLY). Bind
        // the already-live stream to its exact generation before teardown can
        // make the liveness count drop to zero.
        adopt_watchdog_session_generation(*generation);
        check_idle_stop_after_apply = !stream_is_active_or_pending();
      }
    }
    if (!ok) {
      if (cancelled()) {
        pending.request.session = nullptr;
        std::lock_guard<std::mutex> lock(pending_apply_mutex());
        if (!pending_apply_state()) {
          pending_apply_state() = std::move(pending);
        }
        return false;
      }
      pending.attempts += 1;
      pending.request.session = nullptr;
      const auto delay = deferred_apply_retry_delay(pending.attempts);
      pending.next_attempt = std::chrono::steady_clock::now() + delay;
      std::lock_guard<std::mutex> lock(pending_apply_mutex());
      if (!pending_apply_state()) {
        pending_apply_state() = pending;
        BOOST_LOG(warning) << "Display helper: deferred APPLY failed; retrying in "
                           << delay.count() << "ms (attempt " << pending.attempts
                           << "/" << kMaxDeferredApplyAttempts << ").";
      } else {
        BOOST_LOG(info) << "Display helper: deferred APPLY failed but a newer pending configuration is queued; dropping retry.";
      }
    }
    if (check_idle_stop_after_apply) {
      // Do not join the watchdog while owning the display-operation gate: its
      // worker may observe session teardown. A newer APPLY that starts first
      // will publish a different generation, which ordinary stop preserves.
      execution_lock.unlock();
      // current_app_id() rather than running(): running() can drive a deferred
      // launch and call terminate() from this APPLY path.
      if (!stream_is_active_or_pending() && proc::proc.current_app_id() <= 0) {
        stop_watchdog();
      }
    }
    return ok;
  }

  bool has_pending_apply() {
    std::lock_guard<std::mutex> lock(pending_apply_mutex());
    return pending_apply_state().has_value();
  }

  void clear_pending_apply() {
    std::unique_lock<std::mutex> execution_lock(pending_apply_execution_mutex());
    clear_pending_apply_queue_locked();
  }

  int64_t ms_since_last_apply() {
    const auto last_us = g_last_apply_completed_us.load(std::memory_order_relaxed);
    if (last_us == 0) {
      return std::numeric_limits<int64_t>::max();
    }
    const auto elapsed_us = now_steady_us() - last_us;
    return elapsed_us / 1000;
  }

  namespace {
    constexpr double kEdidRefreshToleranceHz = 0.5;

    struct ParsedEdidRefreshInfo {
      bool present {false};
      std::optional<int> max_vertical_hz;
      double max_timing_hz {0.0};
    };

    void consider_timing(double hz, ParsedEdidRefreshInfo &out) {
      if (!std::isfinite(hz) || hz <= 0.0) {
        return;
      }
      if (hz > out.max_timing_hz) {
        out.max_timing_hz = hz;
      }
    }

    void parse_detailed_descriptor(const uint8_t *descriptor, ParsedEdidRefreshInfo &out) {
      if (!descriptor) {
        return;
      }

      const uint16_t pixel_clock = static_cast<uint16_t>(descriptor[0] | (static_cast<uint16_t>(descriptor[1]) << 8));
      if (pixel_clock == 0) {
        if (descriptor[3] == 0xFD) {
          const int max_vertical = static_cast<int>(descriptor[6]);
          if (max_vertical > 0 && max_vertical < 2000) {
            if (!out.max_vertical_hz || max_vertical > *out.max_vertical_hz) {
              out.max_vertical_hz = max_vertical;
            }
          }
        }
        return;
      }

      const uint16_t h_active = static_cast<uint16_t>(descriptor[2] | (static_cast<uint16_t>(descriptor[4] & 0xF0) << 4));
      const uint16_t h_blanking = static_cast<uint16_t>(descriptor[3] | (static_cast<uint16_t>(descriptor[4] & 0x0F) << 8));
      const uint16_t v_active = static_cast<uint16_t>(descriptor[5] | (static_cast<uint16_t>(descriptor[7] & 0xF0) << 4));
      const uint16_t v_blanking = static_cast<uint16_t>(descriptor[6] | (static_cast<uint16_t>(descriptor[7] & 0x0F) << 8));
      const uint32_t h_total = static_cast<uint32_t>(h_active) + static_cast<uint32_t>(h_blanking);
      const uint32_t v_total = static_cast<uint32_t>(v_active) + static_cast<uint32_t>(v_blanking);
      if (h_total == 0 || v_total == 0) {
        return;
      }

      const double pixel_clock_hz = static_cast<double>(pixel_clock) * 10000.0;
      double refresh_hz = pixel_clock_hz / (static_cast<double>(h_total) * static_cast<double>(v_total));
      if ((descriptor[17] & 0x80) != 0) {
        refresh_hz *= 2.0;
      }

      consider_timing(refresh_hz, out);
    }

    ParsedEdidRefreshInfo parse_edid_refresh(const std::vector<std::byte> &edid) {
      ParsedEdidRefreshInfo info;
      if (edid.empty()) {
        return info;
      }
      info.present = true;
      if (edid.size() < 128) {
        return info;
      }

      const auto *bytes = reinterpret_cast<const uint8_t *>(edid.data());
      const auto parse_block_descriptors = [&](const uint8_t *block, std::size_t start, std::size_t end) {
        if (!block || start >= end) {
          return;
        }
        for (std::size_t offset = start; offset + 17 < end; offset += 18) {
          parse_detailed_descriptor(block + offset, info);
        }
      };

      parse_block_descriptors(bytes, 54, 126);

      const std::size_t block_count = edid.size() / 128;
      const uint8_t extension_count = bytes[126];
      const std::size_t max_extensions = std::min<std::size_t>(extension_count, block_count > 0 ? block_count - 1 : 0);
      for (std::size_t idx = 0; idx < max_extensions; ++idx) {
        const std::size_t block_start = (idx + 1) * 128;
        if (block_start + 128 > edid.size()) {
          break;
        }
        const auto *ext = bytes + block_start;
        if (ext[0] == 0x02) {
          const uint8_t dtd_offset = ext[2];
          if (dtd_offset >= 4 && dtd_offset < 127) {
            const std::size_t start = block_start + dtd_offset;
            const std::size_t end = block_start + 127;
            for (std::size_t offset = start; offset + 17 < end; offset += 18) {
              parse_detailed_descriptor(bytes + offset, info);
            }
          }
        }
      }

      return info;
    }

    std::vector<std::byte> read_edid_for_device_id(const std::string &device_id) {
      if (device_id.empty()) {
        return {};
      }
      try {
        display_device::DisplayRecoveryBehaviorGuard guard(display_device::DisplayRecoveryBehavior::Skip);
        auto api = std::make_shared<display_device::WinApiLayer>();
        auto display_data = api->queryDisplayConfig(display_device::QueryType::All);
        if (!display_data) {
          return {};
        }

        auto source_data = display_device::win_utils::collectSourceDataForMatchingPaths(*api, display_data->m_paths);
        auto it = source_data.find(device_id);
        if (it == source_data.end()) {
          for (const auto &entry : source_data) {
            if (boost::iequals(entry.first, device_id)) {
              it = source_data.find(entry.first);
              break;
            }
          }
        }

        if (it == source_data.end() || it->second.m_source_id_to_path_index.empty()) {
          return {};
        }

        const UINT32 source_id = it->second.m_active_source.value_or(it->second.m_source_id_to_path_index.begin()->first);
        const auto path_it = it->second.m_source_id_to_path_index.find(source_id);
        if (path_it == it->second.m_source_id_to_path_index.end()) {
          return {};
        }

        const std::size_t path_index = path_it->second;
        if (path_index >= display_data->m_paths.size()) {
          return {};
        }

        const auto &path = display_data->m_paths[path_index];
        return api->getEdid(path);
      } catch (const std::exception &ex) {
        BOOST_LOG(warning) << "Display helper: failed to read EDID for device " << device_id << ": " << ex.what();
      } catch (...) {
        BOOST_LOG(warning) << "Display helper: failed to read EDID for device " << device_id << " due to unknown error.";
      }

      return {};
    }

    std::optional<display_device::EnumeratedDevice> find_device_for_hint(const std::string &hint) {
      if (hint.empty()) {
        return std::nullopt;
      }

      auto devices = enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
      if (!devices) {
        return std::nullopt;
      }

      for (const auto &device : *devices) {
        if (device_id_equals_ci(device.m_device_id, hint) || device_id_equals_ci(device.m_display_name, hint) ||
            device_id_equals_ci(device.m_friendly_name, hint)) {
          return device;
        }
      }

      return std::nullopt;
    }
  }  // namespace

  std::optional<FramegenEdidSupportResult> framegen_edid_refresh_support(
    const std::string &device_hint,
    const std::vector<int> &targets_hz
  ) {
    const auto resolved_device = find_device_for_hint(device_hint);
    if (!resolved_device) {
      return std::nullopt;
    }

    FramegenEdidSupportResult result;
    result.device_id = resolved_device->m_device_id;
    if (!resolved_device->m_friendly_name.empty()) {
      result.device_label = resolved_device->m_friendly_name;
    } else if (!resolved_device->m_display_name.empty()) {
      result.device_label = resolved_device->m_display_name;
    } else {
      result.device_label = resolved_device->m_device_id;
    }

    const auto edid_bytes = read_edid_for_device_id(result.device_id);
    const auto parsed = parse_edid_refresh(edid_bytes);
    result.edid_present = parsed.present;
    if (parsed.max_vertical_hz) {
      result.max_vertical_hz = parsed.max_vertical_hz;
    }
    if (parsed.max_timing_hz > 0.0) {
      result.max_timing_hz = parsed.max_timing_hz;
    }

    for (int hz : targets_hz) {
      FramegenEdidTargetSupport target {};
      target.hz = hz;
      if (!parsed.present || edid_bytes.empty()) {
        target.supported = std::nullopt;
        target.method = "unknown";
      } else if (parsed.max_vertical_hz && static_cast<double>(*parsed.max_vertical_hz) + kEdidRefreshToleranceHz >= static_cast<double>(hz)) {
        target.supported = true;
        target.method = "range";
      } else if (parsed.max_timing_hz > 0.0 && parsed.max_timing_hz + kEdidRefreshToleranceHz >= static_cast<double>(hz)) {
        target.supported = true;
        target.method = "timing";
      } else if (parsed.max_vertical_hz) {
        target.supported = false;
        target.method = "range";
      } else if (parsed.max_timing_hz > 0.0) {
        target.supported = false;
        target.method = "timing";
      } else {
        target.supported = std::nullopt;
        target.method = "unknown";
      }
      result.targets.push_back(std::move(target));
    }

    return result;
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

  std::optional<std::vector<std::vector<std::string>>> capture_physical_topology() {
    auto topology = capture_current_topology();
    if (!topology) {
      return std::nullopt;
    }

    size_t removed_virtual_devices = 0;
    for (auto &group : *topology) {
      const auto new_end = std::remove_if(group.begin(), group.end(), [&](const std::string &device_id) {
        if (!VDISPLAY::is_virtual_display_output(device_id)) {
          return false;
        }
        ++removed_virtual_devices;
        return true;
      });
      group.erase(new_end, group.end());
    }
    topology->erase(
      std::remove_if(topology->begin(), topology->end(), [](const auto &group) {
        return group.empty();
      }),
      topology->end()
    );

    if (removed_virtual_devices != 0) {
      BOOST_LOG(info) << "Display helper: removed " << removed_virtual_devices
                      << " existing virtual display identity from the stream topology baseline.";
    }
    return topology;
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
          }
        ),
        devices->end()
      );
    }
    return display_device::toJson(*devices);
  }

  void start_watchdog() {
    std::uint64_t stop_epoch_at_start = 0;
    {
      std::lock_guard lock(g_watchdog_mutex);
      stop_epoch_at_start = g_watchdog_stop_epoch;
    }

    for (;;) {
      std::jthread worker_to_join;
      std::optional<std::uint64_t> reaped_session_generation;
      {
        std::unique_lock lock(g_watchdog_mutex);
        if (g_watchdog_state == watchdog_state_e::running) {
          const auto active_generation = active_session_generation();
          if (active_generation && g_watchdog_session_generation != active_generation) {
            // The existing worker remains useful for the newly active launch.
            // Its eventual cleanup must belong to that launch, not the stream
            // that originally created the worker.
            g_watchdog_session_generation = active_generation;
            g_watchdog_teardown_completed = false;
          }
          return;
        }

        // The worker can re-enter stop_watchdog() through proc::running(). It
        // must never wait for an external thread that is currently joining it.
        if (g_watchdog_reaping) {
          if (g_watchdog_worker_id == std::this_thread::get_id()) {
            BOOST_LOG(warning) << "Display helper: watchdog restart deferred while its worker is stopping.";
            return;
          }
          g_watchdog_cv.wait(lock, [] {
            return !g_watchdog_reaping;
          });
          continue;
        }

        if (g_watchdog_thread.joinable()) {
          if (g_watchdog_thread.get_id() == std::this_thread::get_id()) {
            BOOST_LOG(warning) << "Display helper: watchdog restart deferred while its worker is stopping.";
            return;
          }
          g_watchdog_state = watchdog_state_e::stopping;
          g_watchdog_thread.request_stop();
          reaped_session_generation = g_watchdog_session_generation;
          worker_to_join = std::move(g_watchdog_thread);
          g_watchdog_reaping = true;
        } else {
          if (g_watchdog_stop_epoch != stop_epoch_at_start) {
            // A stop that raced this start wins. A later stream start will
            // take a fresh epoch and may launch a new watchdog.
            return;
          }
          g_watchdog_state = watchdog_state_e::stopped;
          try {
            g_watchdog_session_generation = active_session_generation();
            g_watchdog_thread = std::jthread(watchdog_proc);
            g_watchdog_worker_id = g_watchdog_thread.get_id();
            g_watchdog_teardown_completed = false;
            g_watchdog_state = watchdog_state_e::running;
          } catch (const std::system_error &e) {
            BOOST_LOG(error) << "Display helper: failed to start watchdog: " << e.what();
            g_watchdog_session_generation.reset();
          }
          return;
        }
      }

      bool joined = false;
      try {
        // Do not hold g_watchdog_mutex across join(): the worker may enter
        // proc::running(), which can synchronously call stop_watchdog().
        worker_to_join.join();
        joined = true;
      } catch (const std::system_error &e) {
        BOOST_LOG(error) << "Display helper: unable to reap stopping watchdog: " << e.what();
      }

      {
        std::lock_guard lock(g_watchdog_mutex);
        const bool stop_requested_while_reaping = g_watchdog_stop_epoch != stop_epoch_at_start;
        if (!joined) {
          // Preserve ownership on the only static jthread object. A later
          // external caller can retry; detaching would outlive CRT teardown.
          g_watchdog_thread = std::move(worker_to_join);
          g_watchdog_state = watchdog_state_e::stopping;
          g_watchdog_reaping = false;
          g_watchdog_cv.notify_all();
          return;
        }
        g_watchdog_worker_id = {};
        finish_watchdog_stop_locked(reaped_session_generation);
        g_watchdog_session_generation.reset();
        if (stop_requested_while_reaping) {
          // A later stop belongs to this stale generation too. Do not launch
          // a replacement after that caller has observed stop completion.
          g_watchdog_state = watchdog_state_e::stopped;
          g_watchdog_reaping = false;
          g_watchdog_cv.notify_all();
          return;
        }

        // Keep reaping asserted through replacement construction. A stop that
        // arrives after the old worker is joined then waits here and will see
        // (and stop) the new generation instead of being lost in a stopped
        // no-thread gap.
        try {
          g_watchdog_session_generation = active_session_generation();
          g_watchdog_thread = std::jthread(watchdog_proc);
          g_watchdog_worker_id = g_watchdog_thread.get_id();
          g_watchdog_teardown_completed = false;
          g_watchdog_state = watchdog_state_e::running;
        } catch (const std::system_error &e) {
          g_watchdog_state = watchdog_state_e::stopped;
          g_watchdog_session_generation.reset();
          BOOST_LOG(error) << "Display helper: failed to start watchdog: " << e.what();
        }
        g_watchdog_reaping = false;
        g_watchdog_cv.notify_all();
        return;
      }
    }
  }

  void stop_watchdog(bool force) {
    for (;;) {
      std::jthread worker_to_join;
      std::optional<std::uint64_t> stopped_session_generation;
      {
        std::unique_lock lock(g_watchdog_mutex);
        if (g_watchdog_state == watchdog_state_e::stopped && !g_watchdog_thread.joinable() && !g_watchdog_reaping) {
          return;
        }

        // Callers report a stream transition after releasing their own session
        // locks. A newer stream can therefore start between an old caller's
        // last-session check and this stop request. Do not let that stale stop
        // take down the watchdog the newer stream has already adopted.
        if (!force && stream_is_active_or_pending()) {
          BOOST_LOG(debug) << "Display helper: deferring watchdog stop while a stream is active or pending.";
          return;
        }

        const auto active_generation = active_session_generation();
        if (!force && active_generation &&
            (!g_watchdog_session_generation || *active_generation != *g_watchdog_session_generation)) {
          BOOST_LOG(debug) << "Display helper: ignoring stale watchdog stop for session generation "
                           << (g_watchdog_session_generation ? std::to_string(*g_watchdog_session_generation) : std::string {"none"})
                           << "; active generation is " << *active_generation << '.';
          return;
        }

        ++g_watchdog_stop_epoch;
        g_watchdog_state = watchdog_state_e::stopping;
        stopped_session_generation = force ? std::nullopt : g_watchdog_session_generation;
        const bool caller_is_worker = g_watchdog_worker_id == std::this_thread::get_id();
        if (g_watchdog_thread.joinable()) {
          g_watchdog_thread.request_stop();
        }

        if (caller_is_worker) {
          // Keep the jthread owned globally; an external start/stop or main's
          // shutdown will join it. Waiting here would deadlock the reaper.
          finish_watchdog_stop_locked(stopped_session_generation);
          BOOST_LOG(warning) << "Display helper: watchdog requested its own stop; deferring join to an external caller.";
          return;
        }

        if (g_watchdog_reaping) {
          g_watchdog_cv.wait(lock, [] {
            return !g_watchdog_reaping;
          });
          if (force) {
            // A stale reaper may have installed a replacement while we were
            // waiting. Re-evaluate the owned watchdog before reporting a
            // forced shutdown as complete.
            continue;
          }
          return;
        }

        if (!g_watchdog_thread.joinable()) {
          g_watchdog_worker_id = {};
          g_watchdog_state = watchdog_state_e::stopped;
          finish_watchdog_stop_locked(stopped_session_generation);
          g_watchdog_session_generation.reset();
          return;
        }

        worker_to_join = std::move(g_watchdog_thread);
        g_watchdog_reaping = true;
      }
      bool joined = false;
      try {
        // See start_watchdog(): proc::running() can re-enter this function from
        // the worker, so the watchdog mutex must remain available while joining.
        worker_to_join.join();
        joined = true;
      } catch (const std::system_error &e) {
        BOOST_LOG(error) << "Display helper: failed to join watchdog thread: " << e.what();
      }

      {
        std::lock_guard lock(g_watchdog_mutex);
        g_watchdog_reaping = false;
        if (!joined) {
          g_watchdog_thread = std::move(worker_to_join);
          g_watchdog_state = watchdog_state_e::stopping;
          g_watchdog_cv.notify_all();
          return;
        }
        g_watchdog_worker_id = {};
        g_watchdog_state = watchdog_state_e::stopped;
        finish_watchdog_stop_locked(stopped_session_generation);
        g_watchdog_session_generation.reset();
        g_watchdog_cv.notify_all();
      }
      return;
    }
  }
}  // namespace display_helper_integration

#endif
