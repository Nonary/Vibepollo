#include "virtual_display.h"
#include "virtual_display_recovery_registry.h"

#include <virtual_display/driver/control_client.h>
#include <virtual_display/driver/windows_control_client.h>

#include <openssl/rand.h>

#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/windows/display_helper_coordinator.h"
#include "src/platform/windows/misc.h"
#include "src/process.h"
#include "src/state_storage.h"
#include "src/uuid.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <charconv>
#include <cctype>
#include <cfgmgr32.h>
#include <chrono>
#include <cmath>
#include <combaseapi.h>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <devguid.h>
#include <dxgi.h>
#include <dxgi1_6.h>
#include <exception>
#include <filesystem>
#include <fstream>
#include <highlevelmonitorconfigurationapi.h>
#include <icm.h>
#include <iomanip>
#include <initguid.h>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <physicalmonitorenumerationapi.h>
#include <setupapi.h>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <unordered_map>
#include <vector>
#include <winsock2.h>
#include <windows.h>
#include <wincrypt.h>
#include <winreg.h>
#include <wrl/client.h>

#ifndef CPST_EXTENDED_DISPLAY_COLOR_MODE
  // MinGW headers may not expose the extended display color mode constant.
  #define CPST_EXTENDED_DISPLAY_COLOR_MODE 8
#endif

namespace fs = std::filesystem;
namespace sunshine_driver = virtual_display::driver;

namespace {
  constexpr std::uint32_t TEMPORARY_DISPLAY_NAME_CHARS = sunshine_driver::kDisplayNameChars;
}  // namespace

namespace VDISPLAY_SUNSHINE {
  using VDISPLAY::DRIVER_STATUS;
  using VDISPLAY::VirtualDisplayCreationResult;
  using VDISPLAY::VirtualDisplayInfo;
  using VDISPLAY::VirtualDisplayRecoveryParams;
  using VDISPLAY::ensure_display_readiness_e;
  using VDISPLAY::ensure_display_result;
  inline constexpr const char *VIRTUAL_DISPLAY_SELECTION = VDISPLAY::VIRTUAL_DISPLAY_SELECTION;

  void closeVDisplayDevice();
  DRIVER_STATUS openVDisplayDevice();
  bool ensure_driver_is_ready();
  bool startPingThread(std::function<void()> failCb);
  void setWatchdogFeedingEnabled(bool enable);
  bool setRenderAdapterByLuid(const LUID &adapter_luid, const std::wstring &adapter_name, std::uint64_t dedicated_video_memory, std::uint64_t shared_system_memory);
  bool setRenderAdapterByName(const std::wstring &adapterName);
  bool setRenderAdapterWithMostDedicatedMemory();
  bool renderAdapterRequestProvenanceMatches(
    const GUID &guid,
    const LUID &requested_luid,
    std::string_view context
  );
  void ensureVirtualDisplayRegistryDefaults();
  std::optional<VirtualDisplayCreationResult> createVirtualDisplay(
    const char *s_client_uid,
    const char *s_client_name,
    const char *s_hdr_profile,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    const GUID &guid,
    uint32_t base_fps_millihz = 0,
    bool framegen_refresh_active = false,
    int framegen_refresh_multiplier = 1,
    bool hdr_requested = false,
    bool allow_pending_enumeration = false,
    bool replace_existing = true
  );
  bool removeVirtualDisplay(const GUID &guid);
  static bool remove_virtual_display_impl(
    const GUID &guid,
    bool cancel_recovery_monitor,
    std::stop_token stop_token = {}
  );
  uint64_t client_uuid_to_virtual_display_id(const GUID &client_guid);
  bool removeAllVirtualDisplays();
  std::optional<std::string> resolveVirtualDisplayDeviceId(const std::wstring &display_name);
  std::optional<std::string> resolveVirtualDisplayDeviceIdForClient(const std::string &client_name);
  std::optional<std::string> resolveActiveVirtualDisplayDeviceIdForStableId(
    const std::string &stable_id,
    const std::string &preferred_output_identifier,
    const std::string &client_name,
    bool allow_any_fallback = true
  );
  std::optional<std::string> resolveAnyVirtualDisplayDeviceId();
  bool is_virtual_display_selection(const std::string &output_identifier);
  bool isVirtualDisplayDriverInstalled();
  std::vector<VirtualDisplayInfo> enumerateVirtualDisplays();
  uuid_util::uuid_t persistentVirtualDisplayUuid();
  bool has_active_physical_display();
  bool should_auto_enable_virtual_display();
  bool has_retained_ensure_display();
  void cleanup_retained_ensure_display();
  ensure_display_result ensure_display(const std::optional<LUID> &required_adapter_luid);
  void cleanup_ensure_display(const ensure_display_result &result);

  enum class RestartCooldownBehavior {
    skip,
    wait,
  };

  enum class OpenRecoveryBehavior {
    transport_only,
    recover_driver,
  };

  static bool ensure_driver_is_ready_impl(RestartCooldownBehavior cooldown_behavior, std::stop_token stop_token = {});
  static DRIVER_STATUS open_vdisplay_device_impl(std::stop_token stop_token, OpenRecoveryBehavior recovery_behavior);
  static bool start_ping_thread_impl(
    std::function<void()> fail_cb,
    std::stop_token stop_token,
    OpenRecoveryBehavior recovery_behavior
  );
  static std::optional<VirtualDisplayCreationResult> create_virtual_display_with_stop(
    const char *s_client_uid,
    const char *s_client_name,
    const char *s_hdr_profile,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    const GUID &guid,
    uint32_t base_fps_millihz,
    bool framegen_refresh_active,
    int framegen_refresh_multiplier,
    bool hdr_requested,
    bool allow_pending_enumeration,
    bool replace_existing,
    std::stop_token stop_token
  );

  namespace {
    constexpr auto WATCHDOG_INIT_GRACE = std::chrono::seconds(30);
    constexpr auto DRIVER_RESTART_TIMEOUT = std::chrono::seconds(5);
    constexpr auto DRIVER_RESTART_POLL_INTERVAL = std::chrono::milliseconds(500);
    constexpr auto DRIVER_RESTART_FAILURE_COOLDOWN = std::chrono::seconds(3);
    constexpr int DRIVER_RESTART_MAX_ATTEMPTS = 3;
    constexpr auto DEVICE_RESTART_SETTLE_DELAY = std::chrono::milliseconds(200);
    constexpr auto VIRTUAL_DISPLAY_TEARDOWN_COOLDOWN = std::chrono::milliseconds(250);
    constexpr std::wstring_view SUNSHINE_DRIVER_HARDWARE_ID = L"root\\sunshinevirtualdisplay";
    constexpr std::wstring_view SUNSHINE_DRIVER_FRIENDLY_NAME_W = L"Sunshine Virtual Display Driver";
    constexpr std::uint32_t DRIVER_LEASE_TIMEOUT_MS = 30000;
    constexpr std::uint16_t REQUIRED_DRIVER_PROTOCOL_MAJOR = sunshine_driver::kProtocolVersionMajor;
    constexpr std::uint16_t REQUIRED_DRIVER_PROTOCOL_MINOR = sunshine_driver::kMinimumCompatibleProtocolVersionMinor;
    constexpr std::uint16_t SECURE_RECLAIM_DRIVER_PROTOCOL_MINOR = 7;

    bool wait_for_monitor_stop(std::stop_token stop_token, std::chrono::steady_clock::duration duration);

    std::atomic<bool> g_watchdog_feed_requested {false};
    std::atomic<bool> g_watchdog_stop_requested {false};
    std::atomic<std::int64_t> g_watchdog_grace_deadline_ns {0};
    std::recursive_mutex g_watchdog_lifecycle_mutex;
    std::mutex g_watchdog_thread_mutex;
    std::mutex g_watchdog_wake_mutex;
    std::condition_variable g_watchdog_wake_cv;
    std::uint64_t g_watchdog_wake_generation = 0;
    std::thread g_watchdog_thread;
    std::shared_ptr<sunshine_driver::WindowsControlTransport> g_watchdog_transport;
    std::function<void()> g_watchdog_fail_cb;
    std::atomic_bool g_watchdog_failure_callback_pending {false};
    bool g_watchdog_start_in_progress = false;
    std::atomic<std::int64_t> g_last_teardown_ns {0};
    std::atomic<std::int64_t> g_last_restart_failure_ns {0};
    std::recursive_mutex g_virtual_display_operation_mutex;
    std::mutex g_ensure_display_acquire_mutex;
    std::mutex g_ensure_display_state_mutex;
    std::condition_variable g_ensure_display_state_cv;
    VDISPLAY::policy::probe_display_lifetime_t g_ensure_display_lifetime;
    GUID g_ensure_display_guid {};

    vdisplay_recovery::monitor_registry_t &watchdog_failure_callbacks() {
      static vdisplay_recovery::monitor_registry_t callbacks;
      return callbacks;
    }

    // Recovery can recreate a display before Windows publishes its monitor
    // path. Keep the resulting deferred HDR work owned so main can cancel and
    // join it before tearing down configuration, display-helper, and logging
    // state. Ordinary best-effort profile work keeps its existing detached
    // behavior.
    vdisplay_recovery::monitor_registry_t &deferred_hdr_profile_workers() {
      static vdisplay_recovery::monitor_registry_t workers;
      return workers;
    }

    // Windows scale can only be applied once the target is active and Windows has
    // exposed its monitor device path, which does not happen until the display
    // helper's APPLY activates it — after creation has already returned. Keyed by
    // display GUID and stopped with the recovery monitors so a session teardown
    // cancels a finalizer still waiting on activation.
    vdisplay_recovery::monitor_registry_t &deferred_scale_workers() {
      static vdisplay_recovery::monitor_registry_t workers;
      return workers;
    }

    bool guid_equal(const GUID &lhs, const GUID &rhs) {
      return std::memcmp(&lhs, &rhs, sizeof(GUID)) == 0;
    }

    bool is_ensure_display_client(const char *client_uid) {
      return client_uid && std::strcmp(client_uid, "sunshine-ensure") == 0;
    }

    std::int64_t steady_ticks_from_time(std::chrono::steady_clock::time_point tp) {
      return std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
    }

    std::chrono::steady_clock::time_point time_from_steady_ticks(std::int64_t ticks) {
      return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(ticks));
    }

    void note_virtual_display_teardown() {
      g_last_teardown_ns.store(steady_ticks_from_time(std::chrono::steady_clock::now()), std::memory_order_release);
    }

    bool enforce_teardown_cooldown_if_needed(std::stop_token stop_token = {}) {
      if (stop_token.stop_requested()) {
        return false;
      }
      const auto last_teardown = g_last_teardown_ns.load(std::memory_order_acquire);
      if (last_teardown <= 0) {
        return true;
      }

      const auto last_time = time_from_steady_ticks(last_teardown);
      const auto deadline = last_time + VIRTUAL_DISPLAY_TEARDOWN_COOLDOWN;
      const auto now = std::chrono::steady_clock::now();
      if (deadline > now) {
        const auto sleep_for = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        BOOST_LOG(debug) << "Delaying virtual display creation for " << sleep_for.count()
                         << " ms to let teardown settle.";
        if (wait_for_monitor_stop(stop_token, sleep_for)) {
          return false;
        }
      }
      return !stop_token.stop_requested();
    }

    bool within_grace_period(std::chrono::steady_clock::time_point now) {
      auto deadline_ticks = g_watchdog_grace_deadline_ns.load(std::memory_order_acquire);
      if (deadline_ticks <= 0) {
        return false;
      }
      return now < time_from_steady_ticks(deadline_ticks);
    }

    void set_watchdog_stop_requested(bool stop_requested) {
      {
        std::lock_guard<std::mutex> lock(g_watchdog_wake_mutex);
        g_watchdog_stop_requested.store(stop_requested, std::memory_order_release);
        ++g_watchdog_wake_generation;
      }
      g_watchdog_wake_cv.notify_all();
    }

    void set_watchdog_feed_requested(bool feed_requested, bool reset_grace) {
      {
        std::lock_guard<std::mutex> lock(g_watchdog_wake_mutex);
        if (reset_grace) {
          const auto deadline = std::chrono::steady_clock::now() + WATCHDOG_INIT_GRACE;
          g_watchdog_grace_deadline_ns.store(steady_ticks_from_time(deadline), std::memory_order_release);
        }
        g_watchdog_feed_requested.store(feed_requested, std::memory_order_release);
        ++g_watchdog_wake_generation;
      }
      g_watchdog_wake_cv.notify_all();
    }

    bool wait_for_watchdog_state_change(
      const std::chrono::milliseconds duration,
      std::uint64_t &observed_generation
    ) {
      std::unique_lock<std::mutex> lock(g_watchdog_wake_mutex);
      (void) g_watchdog_wake_cv.wait_for(lock, duration, [&]() {
        return g_watchdog_stop_requested.load(std::memory_order_acquire) ||
               g_watchdog_wake_generation != observed_generation;
      });
      observed_generation = g_watchdog_wake_generation;
      return g_watchdog_stop_requested.load(std::memory_order_acquire);
    }

    void stop_watchdog_thread() {
      std::lock_guard<std::recursive_mutex> lifecycle_lock(g_watchdog_lifecycle_mutex);
      set_watchdog_stop_requested(true);

      std::thread watchdog_thread;
      std::shared_ptr<sunshine_driver::WindowsControlTransport> watchdog_transport;
      {
        std::lock_guard<std::mutex> lock(g_watchdog_thread_mutex);
        if (!g_watchdog_thread.joinable()) {
          watchdog_transport = std::move(g_watchdog_transport);
          if (watchdog_transport) {
            watchdog_transport->cancel_pending_io();
          }
          return;
        }
        if (g_watchdog_thread.get_id() == std::this_thread::get_id()) {
          // A current worker cannot join itself. Keep its std::thread owned
          // in static storage so the next external lifecycle call can reap it.
          return;
        }
        watchdog_transport = std::move(g_watchdog_transport);
        watchdog_thread = std::move(g_watchdog_thread);
      }

      if (watchdog_transport) {
        watchdog_transport->cancel_pending_io();
      }

      if (!watchdog_thread.joinable()) {
        return;
      }

      watchdog_thread.join();
    }

    std::function<void()> copy_watchdog_fail_cb() {
      std::lock_guard<std::mutex> lock(g_watchdog_thread_mutex);
      return g_watchdog_fail_cb;
    }

    void store_watchdog_fail_cb(const std::function<void()> &fail_cb) {
      std::lock_guard<std::mutex> lock(g_watchdog_thread_mutex);
      g_watchdog_fail_cb = fail_cb;
    }

    std::function<void()> default_watchdog_fail_cb() {
      return []() {
        BOOST_LOG(error) << "Sunshine virtual display lease-feed failed without a registered process callback; closing driver transport.";
        closeVDisplayDevice();
      };
    }

    bool watchdog_thread_running() {
      std::lock_guard<std::mutex> lock(g_watchdog_thread_mutex);
      return g_watchdog_thread.joinable();
    }

    bool ensure_watchdog_thread_active_for_lease(
      std::stop_token stop_token = {},
      OpenRecoveryBehavior recovery_behavior = OpenRecoveryBehavior::recover_driver
    ) {
      if (stop_token.stop_requested()) {
        return false;
      }
      if (watchdog_thread_running()) {
        return true;
      }

      auto fail_cb = copy_watchdog_fail_cb();
      if (!fail_cb) {
        BOOST_LOG(warning) << "Sunshine virtual display lease-feed thread is not running and no failure callback is registered; using driver transport reset fallback.";
        fail_cb = default_watchdog_fail_cb();
      }

      if (!start_ping_thread_impl(std::move(fail_cb), stop_token, recovery_behavior)) {
        BOOST_LOG(warning) << "Sunshine virtual display lease-feed thread could not be started for an active temporary display.";
        return false;
      }

      return true;
    }

    void dispatch_watchdog_fail_cb(std::shared_ptr<const std::function<void()>> fail_cb) {
      if (!fail_cb || !*fail_cb) {
        return;
      }
      bool expected = false;
      if (!g_watchdog_failure_callback_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        // The pending callback is already closing the same failed driver.
        // Never synchronously replace/join it from a watchdog worker.
        return;
      }

      if (!watchdog_failure_callbacks().start(
            "watchdog-failure",
            [fail_cb = std::move(fail_cb)](std::stop_token stop_token) {
              const auto clear_pending = [] {
                g_watchdog_failure_callback_pending.store(false, std::memory_order_release);
              };
              if (stop_token.stop_requested()) {
                clear_pending();
                return;
              }
              try {
                if (*fail_cb) {
                  (*fail_cb)();
                }
              } catch (const std::exception &err) {
                BOOST_LOG(error) << "Sunshine virtual display lease-feed failure callback threw: " << err.what();
              } catch (...) {
                BOOST_LOG(error) << "Sunshine virtual display lease-feed failure callback threw an unknown exception.";
              }
              clear_pending();
            }
          )) {
        g_watchdog_failure_callback_pending.store(false, std::memory_order_release);
        BOOST_LOG(error) << "Sunshine virtual display lease-feed: failed to queue failure callback.";
      }
    }

    bool should_skip_restart_attempt(std::chrono::steady_clock::time_point now, std::chrono::milliseconds &cooldown_remaining) {
      const auto last_failure = g_last_restart_failure_ns.load(std::memory_order_acquire);
      if (last_failure <= 0) {
        return false;
      }
      const auto last_time = time_from_steady_ticks(last_failure);
      const auto deadline = last_time + DRIVER_RESTART_FAILURE_COOLDOWN;
      if (now >= deadline) {
        return false;
      }
      cooldown_remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      return true;
    }

    void note_restart_failure(std::chrono::steady_clock::time_point now) {
      g_last_restart_failure_ns.store(steady_ticks_from_time(now), std::memory_order_release);
    }

    void log_control_failure(std::string_view operation, sunshine_driver::ControlStatus status, std::uint32_t native_error) {
      BOOST_LOG(debug) << operation << " failed: status=" << sunshine_driver::to_string(status)
                       << " native_error=" << native_error << '.';
    }

    bool check_driver_protocol_compatible(sunshine_driver::ControlClient &client) {
      const auto version = client.query_protocol_version();
      if (!version.ok()) {
        if (version.status == sunshine_driver::ControlStatus::ProtocolIncompatible &&
            !sunshine_driver::is_valid_api_namespace(version.value.api_namespace)) {
          BOOST_LOG(warning) << "Sunshine virtual display control protocol namespace mismatch.";
        } else if (version.status == sunshine_driver::ControlStatus::ProtocolIncompatible) {
          BOOST_LOG(warning) << "Sunshine virtual display control protocol version "
                             << version.value.major << '.' << version.value.minor << '.' << version.value.patch
                             << " is incompatible; require "
                             << REQUIRED_DRIVER_PROTOCOL_MAJOR << '.' << REQUIRED_DRIVER_PROTOCOL_MINOR << "+.";
        } else {
          log_control_failure("Sunshine virtual display protocol query", version.status, version.native_error);
        }
        return false;
      }

      return true;
    }

    bool query_driver(sunshine_driver::ControlClient &client) {
      const auto result = client.query_permanent_display_count();
      if (!result.ok()) {
        BOOST_LOG(debug) << "Sunshine virtual display permanent count query failed during readiness probe"
                         << " (status=" << sunshine_driver::to_string(result.status)
                         << ", native_error=" << result.native_error << ").";
        return false;
      }

      return true;
    }

    bool set_permanent_display_count(
      sunshine_driver::ControlClient &client,
      std::uint32_t display_count,
      const bool accept_matching_count_after_failure = true
    ) {
      sunshine_driver::PermanentDisplayCountRequest request {};
      request.display_count = display_count;

      const auto result = client.set_permanent_display_count(request);
      if (!result.ok()) {
        const auto after_failure = client.query_permanent_display_count();
        if (accept_matching_count_after_failure &&
            after_failure.ok() &&
            after_failure.value.current_display_count == display_count) {
          BOOST_LOG(warning) << "Sunshine virtual display permanent count changed to " << display_count
                             << " at runtime, but the driver reported failure while persisting it"
                             << " (status=" << sunshine_driver::to_string(result.status)
                             << ", native_error=" << result.native_error << ").";
          return true;
        }

        BOOST_LOG(warning) << "Failed to set Sunshine virtual display permanent count to " << display_count
                           << " (status=" << sunshine_driver::to_string(result.status)
                           << ", native_error=" << result.native_error << ").";
        return false;
      }

      if (result.value.current_display_count != display_count) {
        BOOST_LOG(warning) << "Sunshine virtual display permanent count update returned "
                           << result.value.current_display_count
                           << " after requesting " << display_count << '.';
        return false;
      }

      BOOST_LOG(debug) << "Sunshine virtual display permanent count set to " << result.value.current_display_count
                       << " (max=" << result.value.max_display_count
                       << ", temporary=" << result.value.temporary_display_count << ").";
      return true;
    }

    bool driver_transport_responsive(sunshine_driver::WindowsControlTransport *transport) {
      if (!transport || !transport->valid()) {
        return false;
      }

      sunshine_driver::ControlClient client {*transport};
      if (!check_driver_protocol_compatible(client)) {
        return false;
      }

      if (!query_driver(client)) {
        return false;
      }

      return true;
    }

    bool probe_driver_responsive_once() {
      auto opened = sunshine_driver::open_first_control_device();
      if (!opened.ok()) {
        return false;
      }

      return driver_transport_responsive(opened.transport.get());
    }

    bool equals_ci(std::wstring_view lhs, std::wstring_view rhs) {
      if (lhs.size() != rhs.size()) {
        return false;
      }

      for (size_t i = 0; i < lhs.size(); ++i) {
        if (std::towlower(lhs[i]) != std::towlower(rhs[i])) {
          return false;
        }
      }

      return true;
    }

    bool multi_sz_contains_ci(const std::vector<wchar_t> &values, std::wstring_view target) {
      if (values.empty()) {
        return false;
      }

      const wchar_t *current = values.data();
      while (*current != L'\0') {
        const size_t length = std::wcslen(current);
        if (equals_ci(std::wstring_view(current, length), target)) {
          return true;
        }
        current += length + 1;
      }

      return false;
    }

    std::string trim_copy(std::string_view value) {
      const auto start = value.find_first_not_of(" \t\r\n");
      if (start == std::string_view::npos) {
        return {};
      }
      const auto end = value.find_last_not_of(" \t\r\n");
      return std::string(value.substr(start, end - start + 1));
    }

    std::optional<uint32_t> parse_refresh_hz(std::string_view value) {
      const auto trimmed = trim_copy(value);
      if (trimmed.empty()) {
        return std::nullopt;
      }
      try {
        const double hz = std::stod(trimmed);
        if (!std::isfinite(hz) || hz <= 0.0) {
          return std::nullopt;
        }
        const double clamped = std::min(hz, static_cast<double>(std::numeric_limits<uint32_t>::max()));
        const auto rounded = static_cast<uint32_t>(std::lround(clamped));
        if (rounded == 0) {
          return std::nullopt;
        }
        return rounded;
      } catch (...) {
        return std::nullopt;
      }
    }

    uint32_t highest_requested_refresh_hz() {
      using dd_t = config::video_t::dd_t;
      uint32_t max_hz = 0;

      if (config::video.dd.refresh_rate_option == dd_t::refresh_rate_option_e::manual) {
        if (auto manual = parse_refresh_hz(config::video.dd.manual_refresh_rate)) {
          max_hz = std::max(max_hz, *manual);
        }
      }

      const auto process_entries = [&](const auto &entries) {
        for (const auto &entry : entries) {
          if (auto parsed = parse_refresh_hz(entry.final_refresh_rate)) {
            max_hz = std::max(max_hz, *parsed);
          }
        }
      };

      process_entries(config::video.dd.mode_remapping.mixed);
      process_entries(config::video.dd.mode_remapping.refresh_rate_only);
      process_entries(config::video.dd.mode_remapping.resolution_only);

      return max_hz;
    }

    uint32_t apply_refresh_overrides(uint32_t fps_millihz, uint32_t base_fps_millihz = 0u, int framegen_refresh_multiplier = 1) {
      constexpr uint64_t scale = 1000ull;
      using dd_t = config::video_t::dd_t;
      // Manual refresh rate override takes priority over everything, including the multiplied virtual refresh.
      if (config::video.dd.refresh_rate_option == dd_t::refresh_rate_option_e::manual) {
        if (auto manual = parse_refresh_hz(config::video.dd.manual_refresh_rate)) {
          const uint64_t forced = static_cast<uint64_t>(*manual) * scale;
          return static_cast<uint32_t>(
            std::min<uint64_t>(forced, std::numeric_limits<uint32_t>::max())
          );
        }
      }
      const int refresh_multiplier = std::max(1, framegen_refresh_multiplier);
      if (refresh_multiplier > 1 && base_fps_millihz > 0) {
        const uint64_t minimum_millihz = static_cast<uint64_t>(base_fps_millihz) * static_cast<uint64_t>(refresh_multiplier);
        const uint32_t safe_minimum = static_cast<uint32_t>(std::min<uint64_t>(minimum_millihz, std::numeric_limits<uint32_t>::max()));
        // Ensure we're at least at the minimum, but never lower if already higher
        if (fps_millihz < safe_minimum) {
          fps_millihz = safe_minimum;
        }
      }
      const uint32_t max_hz = highest_requested_refresh_hz();
      if (max_hz == 0) {
        return fps_millihz;
      }
      uint64_t required = static_cast<uint64_t>(max_hz) * scale;
      if (required <= fps_millihz) {
        return fps_millihz;
      }
      required = std::min<uint64_t>(required, std::numeric_limits<uint32_t>::max());
      return static_cast<uint32_t>(required);
    }

    class DevInfoHandle {
    public:
      explicit DevInfoHandle(HDEVINFO value):
          handle(value) {}

      DevInfoHandle(const DevInfoHandle &) = delete;
      DevInfoHandle &operator=(const DevInfoHandle &) = delete;

      DevInfoHandle(DevInfoHandle &&other) noexcept:
          handle(other.handle) {
        other.handle = INVALID_HANDLE_VALUE;
      }

      DevInfoHandle &operator=(DevInfoHandle &&other) noexcept {
        if (this != &other) {
          if (handle != INVALID_HANDLE_VALUE) {
            SetupDiDestroyDeviceInfoList(handle);
          }

          handle = other.handle;
          other.handle = INVALID_HANDLE_VALUE;
        }

        return *this;
      }

      ~DevInfoHandle() {
        if (handle != INVALID_HANDLE_VALUE) {
          SetupDiDestroyDeviceInfoList(handle);
        }
      }

      HDEVINFO get() const {
        return handle;
      }

      bool valid() const {
        return handle != INVALID_HANDLE_VALUE;
      }

    private:
      HDEVINFO handle;
    };

    bool load_device_property_multi_sz(HDEVINFO info, SP_DEVINFO_DATA &data, DWORD property, std::vector<wchar_t> &buffer) {
      buffer.clear();

      DWORD reg_type = 0;
      DWORD required = 0;
      if (!SetupDiGetDeviceRegistryPropertyW(info, &data, property, &reg_type, nullptr, 0, &required)) {
        const DWORD err = GetLastError();
        if (err != ERROR_INSUFFICIENT_BUFFER) {
          return false;
        }
      }

      if (required == 0) {
        return false;
      }

      buffer.resize((required / sizeof(wchar_t)) + 1);
      if (!SetupDiGetDeviceRegistryPropertyW(
            info,
            &data,
            property,
            &reg_type,
            reinterpret_cast<PBYTE>(buffer.data()),
            static_cast<DWORD>(buffer.size() * sizeof(wchar_t)),
            &required
          )) {
        return false;
      }

      if (reg_type != REG_MULTI_SZ) {
        return false;
      }

      if (buffer.empty() || buffer.back() != L'\0') {
        buffer.push_back(L'\0');
      }
      if (buffer.size() < 2 || buffer[buffer.size() - 2] != L'\0') {
        buffer.push_back(L'\0');
      }

      return true;
    }

    std::optional<std::wstring> load_device_property_string(HDEVINFO info, SP_DEVINFO_DATA &data, DWORD property) {
      DWORD reg_type = 0;
      DWORD required = 0;
      if (!SetupDiGetDeviceRegistryPropertyW(info, &data, property, &reg_type, nullptr, 0, &required)) {
        const DWORD err = GetLastError();
        if (err != ERROR_INSUFFICIENT_BUFFER) {
          return std::nullopt;
        }
      }

      if (required == 0) {
        return std::nullopt;
      }

      std::vector<wchar_t> buffer((required / sizeof(wchar_t)) + 1);
      if (!SetupDiGetDeviceRegistryPropertyW(
            info,
            &data,
            property,
            &reg_type,
            reinterpret_cast<PBYTE>(buffer.data()),
            static_cast<DWORD>(buffer.size() * sizeof(wchar_t)),
            &required
          )) {
        return std::nullopt;
      }

      if (reg_type != REG_SZ && reg_type != REG_EXPAND_SZ) {
        return std::nullopt;
      }

      return std::wstring(buffer.data());
    }

    std::optional<std::wstring> extract_device_instance_id(HDEVINFO info, SP_DEVINFO_DATA &data) {
      DWORD required = 0;
      if (!SetupDiGetDeviceInstanceIdW(info, &data, nullptr, 0, &required)) {
        const DWORD err = GetLastError();
        if (err != ERROR_INSUFFICIENT_BUFFER) {
          return std::nullopt;
        }
      }

      if (required == 0) {
        return std::nullopt;
      }

      std::wstring buffer(required, L'\0');
      if (!SetupDiGetDeviceInstanceIdW(info, &data, buffer.data(), required, nullptr)) {
        return std::nullopt;
      }

      buffer.resize(std::wcslen(buffer.c_str()));
      if (buffer.empty()) {
        return std::nullopt;
      }

      return buffer;
    }

    std::optional<std::wstring> find_virtual_display_device_instance_id() {
      DevInfoHandle info(SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DIGCF_PRESENT));
      if (!info.valid()) {
        const DWORD err = GetLastError();
        BOOST_LOG(warning) << "Failed to acquire display device info set for Sunshine virtual display lookup (error=" << err << ")";
        return std::nullopt;
      }

      std::vector<wchar_t> hardware_ids;

      for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA device_info {};
        device_info.cbSize = sizeof(device_info);
        if (!SetupDiEnumDeviceInfo(info.get(), index, &device_info)) {
          const DWORD err = GetLastError();
          if (err != ERROR_NO_MORE_ITEMS) {
            BOOST_LOG(warning) << "SetupDiEnumDeviceInfo failed while scanning for Sunshine virtual display device (error=" << err << ")";
          }
          break;
        }

        bool matches = false;
        if (load_device_property_multi_sz(info.get(), device_info, SPDRP_HARDWAREID, hardware_ids)) {
          matches = multi_sz_contains_ci(hardware_ids, SUNSHINE_DRIVER_HARDWARE_ID);
        }

        if (!matches) {
          if (auto friendly = load_device_property_string(info.get(), device_info, SPDRP_FRIENDLYNAME)) {
            matches = equals_ci(*friendly, SUNSHINE_DRIVER_FRIENDLY_NAME_W);
          }
        }

        if (!matches) {
          continue;
        }

        if (auto instance_id = extract_device_instance_id(info.get(), device_info)) {
          return instance_id;
        }
      }

      return std::nullopt;
    }

    bool apply_device_state_change(HDEVINFO info_set, SP_DEVINFO_DATA &data, DWORD state_change) {
      SP_PROPCHANGE_PARAMS params {};
      params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
      params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
      params.StateChange = state_change;
      params.Scope = DICS_FLAG_GLOBAL;
      params.HwProfile = 0;

      if (!SetupDiSetClassInstallParamsW(info_set, &data, &params.ClassInstallHeader, sizeof(params))) {
        const DWORD err = GetLastError();
        BOOST_LOG(warning) << "Failed to stage property change for Sunshine virtual display device (state=" << state_change << ", error=" << err << ")";
        return false;
      }

      const BOOL invoked = SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, info_set, &data);
      const DWORD err = invoked ? ERROR_SUCCESS : GetLastError();
      (void) SetupDiSetClassInstallParamsW(info_set, &data, nullptr, 0);

      if (!invoked) {
        if (state_change == DICS_DISABLE && err == ERROR_NOT_DISABLEABLE) {
          BOOST_LOG(info) << "Sunshine virtual display device is not disableable (error=" << err << "); continuing with enable.";
          return true;
        }

        BOOST_LOG(warning) << "Property change request rejected for Sunshine virtual display device (state=" << state_change << ", error=" << err << ")";
        return false;
      }

      return true;
    }

    /**
     * @brief Check if a device is stuck in the disabled state (CM_PROB_DISABLED, code 22).
     *
     * This can happen when a previous DICS_DISABLE -> DICS_ENABLE cycle fails at the
     * DICS_ENABLE step, leaving the device disabled with no automatic recovery.
     */
    bool is_device_disabled(const std::wstring &instance_id) {
      DEVINST dev_inst = 0;
      CONFIGRET cr = CM_Locate_DevNodeW(&dev_inst, const_cast<DEVINSTID_W>(instance_id.c_str()), CM_LOCATE_DEVNODE_NORMAL);
      if (cr != CR_SUCCESS) {
        // Device not found via normal lookup; try phantom because the device may be disabled and not enumerated.
        cr = CM_Locate_DevNodeW(&dev_inst, const_cast<DEVINSTID_W>(instance_id.c_str()), CM_LOCATE_DEVNODE_PHANTOM);
        if (cr != CR_SUCCESS) {
          return false;
        }
      }

      ULONG status = 0, problem = 0;
      cr = CM_Get_DevNode_Status(&status, &problem, dev_inst, 0);
      if (cr != CR_SUCCESS) {
        return false;
      }

      // DN_HAS_PROBLEM with CM_PROB_DISABLED means the device is disabled
      if ((status & DN_HAS_PROBLEM) && problem == CM_PROB_DISABLED) {
        return true;
      }

      return false;
    }

    /**
     * @brief Attempt to re-enable a Sunshine virtual display device that is stuck in the disabled state.
     *
     * Unlike restart_virtual_display_device(), this only performs DICS_ENABLE (no disable first)
     * since the device is already disabled.
     */
    bool try_reenable_disabled_device(const std::wstring &instance_id, std::stop_token stop_token = {}) {
      if (stop_token.stop_requested()) {
        return false;
      }
      BOOST_LOG(warning) << "Sunshine virtual display device is stuck disabled (CM_PROB_DISABLED); attempting re-enable.";

      DevInfoHandle dev_set(SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES));
      if (!dev_set.valid()) {
        return false;
      }

      SP_DEVINFO_DATA device_info {};
      device_info.cbSize = sizeof(device_info);
      if (!SetupDiOpenDeviceInfoW(dev_set.get(), instance_id.c_str(), nullptr, 0, &device_info)) {
        return false;
      }

      if (!apply_device_state_change(dev_set.get(), device_info, DICS_ENABLE)) {
        BOOST_LOG(error) << "Failed to re-enable disabled Sunshine virtual display device. A reboot may be required.";
        return false;
      }

      // Give the device time to initialize after re-enable
      if (wait_for_monitor_stop(stop_token, DEVICE_RESTART_SETTLE_DELAY * 2)) {
        return false;
      }

      // Verify it's no longer disabled
      if (is_device_disabled(instance_id)) {
        BOOST_LOG(error) << "Sunshine virtual display device still disabled after re-enable attempt. A reboot may be required.";
        return false;
      }

      BOOST_LOG(info) << "Sunshine virtual display device successfully re-enabled from disabled state.";
      return true;
    }

    constexpr int ENABLE_RETRY_MAX = 2;

    bool restart_virtual_display_device(const std::wstring &instance_id) {
      DevInfoHandle info(SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES));
      if (!info.valid()) {
        const DWORD err = GetLastError();
        BOOST_LOG(warning) << "Failed to acquire global device info set for Sunshine virtual display restart (error=" << err << ")";
        return false;
      }

      SP_DEVINFO_DATA device_info {};
      device_info.cbSize = sizeof(device_info);
      if (!SetupDiOpenDeviceInfoW(info.get(), instance_id.c_str(), nullptr, 0, &device_info)) {
        const DWORD err = GetLastError();
        BOOST_LOG(warning) << "Failed to open Sunshine virtual display instance " << platf::to_utf8(instance_id) << " (error=" << err << ")";
        return false;
      }

      if (!apply_device_state_change(info.get(), device_info, DICS_DISABLE)) {
        return false;
      }

      std::this_thread::sleep_for(DEVICE_RESTART_SETTLE_DELAY);

      // Retry DICS_ENABLE to avoid leaving the device stuck disabled
      for (int retry = 0; retry <= ENABLE_RETRY_MAX; ++retry) {
        if (apply_device_state_change(info.get(), device_info, DICS_ENABLE)) {
          return true;
        }
        if (retry < ENABLE_RETRY_MAX) {
          BOOST_LOG(warning) << "DICS_ENABLE failed (attempt " << (retry + 1) << "/" << (ENABLE_RETRY_MAX + 1)
                             << "); retrying after settle delay.";
          std::this_thread::sleep_for(DEVICE_RESTART_SETTLE_DELAY);
        }
      }

      BOOST_LOG(error) << "All DICS_ENABLE attempts failed after disable; device may be stuck disabled.";
      return false;
    }

    void erase_render_adapter_request_provenance(const uuid_util::uuid_t &guid);
    bool render_adapter_request_provenance_matches(
      const uuid_util::uuid_t &guid,
      std::string_view context
    );

    struct ActiveVirtualDisplayTracker {
      void add(const uuid_util::uuid_t &guid) {
        std::lock_guard<std::mutex> lg(mutex);
        if (std::find(guids.begin(), guids.end(), guid) == guids.end()) {
          guids.push_back(guid);
        }
      }

      void remove(const uuid_util::uuid_t &guid) {
        std::lock_guard<std::mutex> lg(mutex);
        auto it = std::remove(guids.begin(), guids.end(), guid);
        if (it != guids.end()) {
          guids.erase(it, guids.end());
        }
      }

      std::vector<uuid_util::uuid_t> other_than(const uuid_util::uuid_t &guid) {
        std::lock_guard<std::mutex> lg(mutex);
        std::vector<uuid_util::uuid_t> result;
        result.reserve(guids.size());
        for (const auto &entry : guids) {
          if (!(entry == guid)) {
            result.push_back(entry);
          }
        }
        return result;
      }

      std::vector<uuid_util::uuid_t> all() {
        std::lock_guard<std::mutex> lg(mutex);
        return guids;
      }

      bool contains(const uuid_util::uuid_t &guid) {
        std::lock_guard<std::mutex> lg(mutex);
        return std::any_of(guids.begin(), guids.end(), [&](const auto &entry) {
          return entry == guid;
        });
      }

    private:
      std::mutex mutex;
      std::vector<uuid_util::uuid_t> guids;
    };

    ActiveVirtualDisplayTracker &active_virtual_display_tracker() {
      static ActiveVirtualDisplayTracker tracker;
      return tracker;
    }

    uuid_util::uuid_t guid_to_uuid(const GUID &guid) {
      uuid_util::uuid_t uuid {};
      std::memcpy(uuid.b8, &guid, sizeof(uuid.b8));
      return uuid;
    }

    GUID uuid_to_guid(const uuid_util::uuid_t &uuid) {
      GUID guid {};
      std::memcpy(&guid, uuid.b8, sizeof(guid));
      return guid;
    }

    struct DriverLeaseInfo {
      std::uint64_t display_id;
      std::uint64_t lease_id;
      std::optional<std::wstring> display_name;
      std::optional<std::string> device_id;
      std::optional<std::wstring> monitor_device_path;
    };

    struct DriverLeaseTracker {
      void put(const uuid_util::uuid_t &guid, DriverLeaseInfo info) {
        std::lock_guard<std::mutex> lg(mutex);
        leases[guid.string()] = std::move(info);
      }

      std::optional<DriverLeaseInfo> get(const uuid_util::uuid_t &guid) {
        std::lock_guard<std::mutex> lg(mutex);
        auto it = leases.find(guid.string());
        if (it == leases.end()) {
          return std::nullopt;
        }
        return it->second;
      }

      void update_identity(
        const uuid_util::uuid_t &guid,
        const std::optional<std::wstring> &display_name,
        const std::optional<std::string> &device_id,
        const std::optional<std::wstring> &monitor_device_path
      ) {
        std::lock_guard<std::mutex> lg(mutex);
        auto it = leases.find(guid.string());
        if (it == leases.end()) {
          return;
        }
        it->second.display_name = display_name;
        it->second.device_id = device_id;
        it->second.monitor_device_path = monitor_device_path;
      }

      void remove(const uuid_util::uuid_t &guid) {
        std::lock_guard<std::mutex> lg(mutex);
        leases.erase(guid.string());
      }

      std::vector<DriverLeaseInfo> all() {
        std::lock_guard<std::mutex> lg(mutex);
        std::vector<DriverLeaseInfo> result;
        result.reserve(leases.size());
        for (const auto &[_, info] : leases) {
          result.push_back(info);
        }
        return result;
      }

    private:
      std::mutex mutex;
      std::unordered_map<std::string, DriverLeaseInfo> leases;
    };

    DriverLeaseTracker &driver_lease_tracker() {
      static DriverLeaseTracker tracker;
      return tracker;
    }

    std::optional<std::uint64_t> generate_driver_lease_id() {
      std::uint64_t lease_id = 0;
      if (RAND_bytes(reinterpret_cast<unsigned char *>(&lease_id), sizeof(lease_id)) != 1) {
        BOOST_LOG(error) << "Unable to generate a cryptographic virtual display lease identifier.";
        return std::nullopt;
      }
      return VDISPLAY::policy::normalize_opaque_lease_id(
        lease_id,
        sunshine_driver::kMinOpaqueLeaseId
      );
    }

    bool is_missing_lease_error(DWORD error_code) {
      return error_code == ERROR_FILE_NOT_FOUND ||
             error_code == ERROR_NOT_FOUND;
    }

    bool clear_virtual_display_recovery_entry(const uuid_util::uuid_t &guid);

    bool track_virtual_display_created(const uuid_util::uuid_t &guid) {
      if (!render_adapter_request_provenance_matches(
            guid,
            "Sunshine virtual display publication")) {
        return false;
      }
      active_virtual_display_tracker().add(guid);
      return true;
    }

    void track_virtual_display_removed(const uuid_util::uuid_t &guid) {
      driver_lease_tracker().remove(guid);
      active_virtual_display_tracker().remove(guid);
      erase_render_adapter_request_provenance(guid);
      if (!clear_virtual_display_recovery_entry(guid)) {
        BOOST_LOG(warning) << "Unable to clear protected virtual display recovery state for guid="
                           << guid.string() << '.';
      }
    }

    bool is_virtual_display_guid_tracked(const uuid_util::uuid_t &guid) {
      return active_virtual_display_tracker().contains(guid);
    }

    std::vector<uuid_util::uuid_t> collect_conflicting_virtual_displays(const uuid_util::uuid_t &guid) {
      return active_virtual_display_tracker().other_than(guid);
    }

    bool teardown_conflicting_virtual_displays(const uuid_util::uuid_t &guid, std::stop_token stop_token = {}) {
      auto conflicts = collect_conflicting_virtual_displays(guid);
      for (const auto &entry : conflicts) {
        if (stop_token.stop_requested()) {
          return false;
        }
        GUID native_guid = uuid_to_guid(entry);
        (void) remove_virtual_display_impl(native_guid, true, stop_token);
      }
      return !stop_token.stop_requested();
    }

    std::optional<std::wstring> resolve_virtual_display_name_from_devices_for_client(const char *client_name);

    bool release_retained_ensure_display_for_stream(
      const GUID &guid,
      const char *client_uid,
      std::stop_token stop_token = {}
    ) {
      if (stop_token.stop_requested()) {
        return false;
      }
      if (!VDISPLAY::policy::should_release_retained_probe_display(is_ensure_display_client(client_uid))) {
        return true;
      }

      std::unique_lock<std::mutex> acquire_lock(g_ensure_display_acquire_mutex);
      GUID guid_to_remove = uuid_to_guid(persistentVirtualDisplayUuid());
      bool should_remove = false;
      std::uint64_t removal_generation = 0;
      {
        std::unique_lock<std::mutex> lock(g_ensure_display_state_mutex);
        while (g_ensure_display_lifetime.active_probes() != 0 ||
               g_ensure_display_lifetime.removal_in_progress()) {
          if (stop_token.stop_requested()) {
            return false;
          }
          g_ensure_display_state_cv.wait_for(lock, std::chrono::milliseconds(50));
        }
        if (const auto generation = g_ensure_display_lifetime.begin_idle_removal()) {
          guid_to_remove = g_ensure_display_guid;
          removal_generation = *generation;
          should_remove = true;
        }
      }

      if (!should_remove && resolve_virtual_display_name_from_devices_for_client("Sunshine Temporary")) {
        should_remove = true;
      }

      if (!should_remove) {
        return !stop_token.stop_requested();
      }

      BOOST_LOG(info) << "Removing encoder-probe virtual display before creating stream display guid="
                      << guid_to_uuid(guid).string() << '.';
      const bool removed = remove_virtual_display_impl(guid_to_remove, true, stop_token);
      if (removal_generation != 0) {
        std::lock_guard<std::mutex> lock(g_ensure_display_state_mutex);
        g_ensure_display_lifetime.complete_removal(removal_generation, removed);
        if (removed && !g_ensure_display_lifetime.retained()) {
          std::memset(&g_ensure_display_guid, 0, sizeof(g_ensure_display_guid));
        }
        g_ensure_display_state_cv.notify_all();
      }
      if (!removed) {
        if (stop_token.stop_requested()) {
          return false;
        }
        BOOST_LOG(warning) << "Failed to remove retained encoder-probe virtual display before stream creation.";
      }
      return !stop_token.stop_requested();
    }

    bool adopt_existing_driver_lease(
      sunshine_driver::ControlClient &client,
      const uuid_util::uuid_t &guid,
      std::uint64_t display_id,
      const std::optional<std::wstring> &display_name,
      const std::optional<std::string> &device_id,
      const std::optional<std::wstring> &monitor_device_path
    ) {
      if (const auto tracked = driver_lease_tracker().get(guid);
          tracked && tracked->display_id == display_id) {
        sunshine_driver::LeaseRequest lease {};
        lease.lease_id = tracked->lease_id;
        lease.requested_timeout_ms = DRIVER_LEASE_TIMEOUT_MS;
        const auto fed = client.feed_lease(lease);
        if (!fed.ok()) {
          log_control_failure("Sunshine virtual display lease feed during reuse", fed.status, fed.native_error);
          return false;
        }
        driver_lease_tracker().update_identity(guid, display_name, device_id, monitor_device_path);
        (void) ensure_watchdog_thread_active_for_lease();
        BOOST_LOG(info) << "Reused securely tracked Sunshine virtual display for guid=" << guid.string()
                        << " display_id=" << display_id << '.';
        return true;
      }

      const auto state = client.query_display_state();
      if (!state.ok()) {
        log_control_failure("Sunshine virtual display state query", state.status, state.native_error);
        return false;
      }

      for (std::uint32_t i = 0; i < state.value.entry_count && i < sunshine_driver::kMaxDisplayStateEntries; ++i) {
        const auto &entry = state.value.entries[i];
        if (entry.kind != sunshine_driver::kDisplayStateKindTemporary || entry.display_id != display_id) {
          continue;
        }

        if (entry.lease_id == 0) {
          BOOST_LOG(warning) << "Sunshine virtual display reuse found display_id=" << display_id
                             << " but no securely reclaimable ownership was available.";
          return false;
        }

        sunshine_driver::LeaseRequest lease {};
        lease.lease_id = entry.lease_id;
        lease.requested_timeout_ms = DRIVER_LEASE_TIMEOUT_MS;
        const auto query = client.query_lease(lease);
        if (!query.ok() || query.value.lease_exists == 0) {
          if (!query.ok()) {
            log_control_failure("Sunshine virtual display lease query", query.status, query.native_error);
          }
          BOOST_LOG(warning) << "Sunshine virtual display reuse found display_id=" << display_id
                             << " with an unavailable legacy lease.";
          return false;
        }

        const auto fed = client.feed_lease(lease);
        if (!fed.ok()) {
          log_control_failure("Sunshine virtual display lease feed during reuse", fed.status, fed.native_error);
          return false;
        }

        driver_lease_tracker().put(
          guid,
          DriverLeaseInfo {
            entry.display_id,
            entry.lease_id,
            display_name,
            device_id,
            monitor_device_path
          }
        );
        (void) ensure_watchdog_thread_active_for_lease();
        BOOST_LOG(info) << "Adopted existing Sunshine virtual display lease for guid=" << guid.string()
                        << " display_id=" << entry.display_id << '.';
        return true;
      }

      BOOST_LOG(warning) << "Sunshine virtual display reuse could not find display_id=" << display_id
                         << " in driver state.";
      return false;
    }

    bool equals_ci(const std::string &lhs, const std::string &rhs) {
      if (lhs.size() != rhs.size()) {
        return false;
      }
      for (size_t i = 0; i < lhs.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) != std::tolower(static_cast<unsigned char>(rhs[i]))) {
          return false;
        }
      }
      return true;
    }

    std::string normalize_display_name(std::string name) {
      auto trim = [](std::string &inout) {
        size_t start = 0;
        while (start < inout.size() && std::isspace(static_cast<unsigned char>(inout[start]))) {
          ++start;
        }
        size_t end = inout.size();
        while (end > start && std::isspace(static_cast<unsigned char>(inout[end - 1]))) {
          --end;
        }
        if (start > 0 || end < inout.size()) {
          inout = inout.substr(start, end - start);
        }
      };

      trim(name);

      std::string upper;
      upper.reserve(name.size());
      for (char ch : name) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
      }

      if (upper.size() >= 4 && upper[0] == '\\' && upper[1] == '\\' && upper[2] == '.' && upper[3] == '\\') {
        upper.erase(0, 4);
      }

      return upper;
    }

    fs::path default_color_profile_directory() {
      wchar_t system_root[MAX_PATH] = {};
      if (GetSystemWindowsDirectoryW(system_root, _countof(system_root)) == 0) {
        return fs::path(L"C:\\Windows\\System32\\spool\\drivers\\color");
      }
      fs::path root(system_root);
      return root / L"System32" / L"spool" / L"drivers" / L"color";
    }

    std::wstring normalize_profile_key(std::wstring value) {
      auto trim = [](std::wstring &inout) {
        size_t start = 0;
        while (start < inout.size() && std::iswspace(inout[start])) {
          ++start;
        }
        size_t end = inout.size();
        while (end > start && std::iswspace(inout[end - 1])) {
          --end;
        }
        if (start > 0 || end < inout.size()) {
          inout = inout.substr(start, end - start);
        }
      };

      trim(value);

      std::wstring upper;
      upper.reserve(value.size());
      for (wchar_t ch : value) {
        upper.push_back(static_cast<wchar_t>(std::towupper(ch)));
      }
      return upper;
    }

    std::mutex g_physical_hdr_profile_restore_mutex;
    std::unordered_map<std::wstring, std::optional<std::wstring>> g_physical_hdr_profile_restore;

    enum class color_profile_scope_e {
      current_user,
      system_wide,
    };

    const char *color_profile_scope_label(color_profile_scope_e scope) {
      switch (scope) {
        case color_profile_scope_e::current_user:
          return "current_user";
        case color_profile_scope_e::system_wide:
          return "system_wide";
      }
      return "unknown";
    }

    struct scoped_reg_key_t {
      HKEY key = nullptr;
      bool close = false;

      scoped_reg_key_t() = default;

      scoped_reg_key_t(HKEY k, bool should_close):
          key(k),
          close(should_close) {}

      scoped_reg_key_t(const scoped_reg_key_t &) = delete;
      scoped_reg_key_t &operator=(const scoped_reg_key_t &) = delete;

      scoped_reg_key_t(scoped_reg_key_t &&other) noexcept {
        key = other.key;
        close = other.close;
        other.key = nullptr;
        other.close = false;
      }

      scoped_reg_key_t &operator=(scoped_reg_key_t &&other) noexcept {
        if (this != &other) {
          if (close && key) {
            RegCloseKey(key);
          }
          key = other.key;
          close = other.close;
          other.key = nullptr;
          other.close = false;
        }
        return *this;
      }

      ~scoped_reg_key_t() {
        if (close && key) {
          RegCloseKey(key);
        }
      }
    };

    scoped_reg_key_t open_color_profile_registry_root(color_profile_scope_e scope, REGSAM sam_desired) {
      if (scope == color_profile_scope_e::system_wide) {
        return scoped_reg_key_t {HKEY_LOCAL_MACHINE, false};
      }

      HKEY key = nullptr;
      const LSTATUS status = RegOpenCurrentUser(sam_desired, &key);
      if (status != ERROR_SUCCESS || !key) {
        BOOST_LOG(debug) << "HDR profile: RegOpenCurrentUser failed (status=" << status << ").";
        return scoped_reg_key_t {};
      }
      return scoped_reg_key_t {key, true};
    }

    std::optional<fs::path> find_hdr_profile_by_selection(const std::string &selection_utf8) {
      if (selection_utf8.empty()) {
        return std::nullopt;
      }

      const auto selection_w = platf::from_utf8(selection_utf8);
      if (selection_w.empty()) {
        return std::nullopt;
      }

      const fs::path color_dir = default_color_profile_directory();

      // Only allow selecting a filename in the system color profile directory.
      const auto selection_name = fs::path(selection_w).filename().wstring();
      if (selection_name.empty()) {
        return std::nullopt;
      }

      const auto normalized = normalize_profile_key(selection_name);
      if (normalized.empty()) {
        return std::nullopt;
      }

      const auto has_extension = selection_name.find(L'.') != std::wstring::npos;
      const auto make_candidates = [&]() {
        std::vector<std::wstring> names;
        names.push_back(selection_name);
        if (!has_extension) {
          names.push_back(selection_name + L".icm");
          names.push_back(selection_name + L".icc");
        }
        return names;
      };

      for (const auto &name : make_candidates()) {
        fs::path candidate = color_dir / name;
        std::error_code ec;
        if (fs::exists(candidate, ec) && fs::is_regular_file(candidate, ec)) {
          return candidate;
        }
      }

      try {
        for (const auto &entry : fs::directory_iterator(color_dir)) {
          std::error_code ec;
          if (!entry.is_regular_file(ec)) {
            continue;
          }
          const auto file_name = entry.path().filename().wstring();
          if (normalize_profile_key(file_name) == normalized || normalize_profile_key(entry.path().stem().wstring()) == normalized) {
            return entry.path();
          }
        }
      } catch (...) {
      }

      return std::nullopt;
    }

    std::optional<std::wstring> primary_gdi_display_name() {
      POINT pt {0, 0};
      HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
      if (!mon) {
        return std::nullopt;
      }
      MONITORINFOEXW info {};
      info.cbSize = sizeof(info);
      if (!GetMonitorInfoW(mon, &info)) {
        return std::nullopt;
      }
      if (info.szDevice[0] == L'\0') {
        return std::nullopt;
      }
      return std::wstring(info.szDevice);
    }

    // Forward declaration for the retrying resolver
    std::optional<std::wstring> resolve_monitor_device_path(
      const std::optional<std::wstring> &display_name,
      const std::optional<std::string> &device_id,
      int attempts = 5,
      std::chrono::milliseconds delay = std::chrono::milliseconds(100),
      const std::optional<std::string> &client_name = std::nullopt,
      std::stop_token stop_token = {}
    );

    std::optional<std::wstring> resolve_virtual_display_name_from_devices();

    // Helper to compute the registry path for color profile associations from a device path
    std::optional<std::wstring> get_color_profile_registry_path(const std::wstring &device_path) {
      // Parse the device path to extract the instance ID
      // Format: \\?\DISPLAY#SDD5000#1&28a6823a&2&UID265#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}
      size_t first_hash = device_path.find(L'#');
      if (first_hash == std::wstring::npos) {
        return std::nullopt;
      }
      size_t second_hash = device_path.find(L'#', first_hash + 1);
      if (second_hash == std::wstring::npos) {
        return std::nullopt;
      }
      size_t third_hash = device_path.find(L'#', second_hash + 1);
      if (third_hash == std::wstring::npos) {
        return std::nullopt;
      }

      std::wstring device_type = device_path.substr(first_hash + 1, second_hash - first_hash - 1);
      std::wstring instance_id = device_path.substr(second_hash + 1, third_hash - second_hash - 1);

      std::wstring enum_path = L"SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\" + device_type + L"\\" + instance_id;
      HKEY enum_key = nullptr;
      if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, enum_path.c_str(), 0, KEY_READ, &enum_key) != ERROR_SUCCESS) {
        return std::nullopt;
      }

      wchar_t driver_value[256] = {};
      DWORD driver_size = sizeof(driver_value);
      DWORD driver_type = 0;
      LSTATUS status = RegQueryValueExW(enum_key, L"Driver", nullptr, &driver_type, reinterpret_cast<LPBYTE>(driver_value), &driver_size);
      RegCloseKey(enum_key);

      if (status != ERROR_SUCCESS || driver_type != REG_SZ) {
        return std::nullopt;
      }

      std::wstring driver_str(driver_value);
      size_t backslash = driver_str.rfind(L'\\');
      if (backslash == std::wstring::npos) {
        return std::nullopt;
      }
      std::wstring key_number = driver_str.substr(backslash + 1);

      return L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ICM\\ProfileAssociations\\Display\\" +
             driver_str.substr(0, backslash) + L"\\" + key_number;
    }

    // Read the current color profile from registry for a display
    std::optional<std::wstring> read_color_profile_from_registry(const std::wstring &device_path, color_profile_scope_e scope) {
      auto profile_path = get_color_profile_registry_path(device_path);
      if (!profile_path) {
        return std::nullopt;
      }

      auto root = open_color_profile_registry_root(scope, KEY_READ);
      if (!root.key) {
        return std::nullopt;
      }

      HKEY profile_key = nullptr;
      const LSTATUS open_status = RegOpenKeyExW(root.key, profile_path->c_str(), 0, KEY_READ, &profile_key);
      if (open_status != ERROR_SUCCESS) {
        return std::nullopt;
      }

      wchar_t profile_value[512] = {};
      DWORD profile_size = sizeof(profile_value);
      DWORD profile_type = 0;
      LSTATUS status = RegQueryValueExW(profile_key, L"ICMProfileAC", nullptr, &profile_type, reinterpret_cast<LPBYTE>(profile_value), &profile_size);
      RegCloseKey(profile_key);

      if (status != ERROR_SUCCESS || (profile_type != REG_MULTI_SZ && profile_type != REG_SZ)) {
        return std::nullopt;
      }

      // REG_MULTI_SZ is null-terminated, return first string
      if (profile_value[0] == L'\0') {
        return std::nullopt;
      }

      return std::wstring(profile_value);
    }

    // Clear the color profile association from registry for a display
    bool clear_color_profile_from_registry(const std::wstring &device_path, color_profile_scope_e scope) {
      auto profile_path = get_color_profile_registry_path(device_path);
      if (!profile_path) {
        return false;
      }

      auto root = open_color_profile_registry_root(scope, KEY_SET_VALUE);
      if (!root.key) {
        return false;
      }

      HKEY profile_key = nullptr;
      const LSTATUS open_status = RegOpenKeyExW(root.key, profile_path->c_str(), 0, KEY_SET_VALUE, &profile_key);
      if (open_status != ERROR_SUCCESS) {
        BOOST_LOG(debug) << "HDR profile: failed to open registry key for clearing (scope=" << color_profile_scope_label(scope)
                         << ", status=" << open_status << ", path='" << platf::to_utf8(*profile_path) << "').";
        return false;
      }

      // Delete the ICMProfileAC value
      LSTATUS status = RegDeleteValueW(profile_key, L"ICMProfileAC");
      RegCloseKey(profile_key);

      if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        BOOST_LOG(debug) << "HDR profile: failed to clear registry association (scope=" << color_profile_scope_label(scope)
                         << ", status=" << status << ", path='" << platf::to_utf8(*profile_path) << "').";
      }

      return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    }

    // Write color profile association directly to registry for a virtual display
    bool write_color_profile_to_registry(
      const std::wstring &device_path,
      const std::wstring &profile_filename,
      color_profile_scope_e scope,
      LSTATUS *out_status = nullptr
    ) {
      auto profile_assoc_path = get_color_profile_registry_path(device_path);
      if (!profile_assoc_path) {
        if (out_status) {
          *out_status = ERROR_PATH_NOT_FOUND;
        }
        return false;
      }

      auto root = open_color_profile_registry_root(scope, KEY_CREATE_SUB_KEY | KEY_SET_VALUE | KEY_QUERY_VALUE);
      if (!root.key) {
        if (out_status) {
          *out_status = ERROR_ACCESS_DENIED;
        }
        return false;
      }

      HKEY profile_key = nullptr;
      LSTATUS status = RegCreateKeyExW(root.key, profile_assoc_path->c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &profile_key, nullptr);
      if (status != ERROR_SUCCESS) {
        if (out_status) {
          *out_status = status;
        }
        BOOST_LOG(debug) << "HDR profile: failed to open/create registry key (scope=" << color_profile_scope_label(scope)
                         << ", status=" << status << ", path='" << platf::to_utf8(*profile_assoc_path) << "').";
        return false;
      }

      // Write UsePerUserProfiles = 1
      DWORD use_per_user = 1;
      (void) RegSetValueExW(profile_key, L"UsePerUserProfiles", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&use_per_user), sizeof(use_per_user));

      // Write ICMProfileAC as REG_MULTI_SZ
      std::vector<wchar_t> multi_sz(profile_filename.begin(), profile_filename.end());
      multi_sz.push_back(L'\0');
      multi_sz.push_back(L'\0');

      status = RegSetValueExW(profile_key, L"ICMProfileAC", 0, REG_MULTI_SZ, reinterpret_cast<const BYTE *>(multi_sz.data()), static_cast<DWORD>(multi_sz.size() * sizeof(wchar_t)));
      RegCloseKey(profile_key);

      if (out_status) {
        *out_status = status;
      }

      if (status != ERROR_SUCCESS) {
        BOOST_LOG(debug) << "HDR profile: failed to write registry association (scope=" << color_profile_scope_label(scope)
                         << ", status=" << status << ", path='" << platf::to_utf8(*profile_assoc_path) << "').";
      }

      return status == ERROR_SUCCESS;
    }

    bool is_system_wide_profile_scope(const color_profile_scope_e scope) {
      return scope == color_profile_scope_e::system_wide;
    }

    std::optional<std::wstring> read_color_profile_association(
      const std::wstring &device_path,
      const color_profile_scope_e scope
    ) {
      if (auto profile = VDISPLAY::get_advanced_color_profile(device_path, is_system_wide_profile_scope(scope))) {
        return profile;
      }
      return read_color_profile_from_registry(device_path, scope);
    }

    bool clear_color_profile_association(
      const std::wstring &device_path,
      const std::optional<std::wstring> &profile_name,
      const color_profile_scope_e scope
    ) {
      bool api_success = false;
      if (profile_name && !profile_name->empty()) {
        const auto result = VDISPLAY::remove_advanced_color_profile(
          device_path,
          fs::path(*profile_name).filename().wstring(),
          is_system_wide_profile_scope(scope)
        );
        api_success = result.success;
        if (result.attempted && !result.success) {
          BOOST_LOG(debug) << "HDR profile: Advanced Color disassociation failed (hr=0x"
                           << std::hex << static_cast<unsigned long>(result.association_status) << std::dec
                           << ", scope=" << color_profile_scope_label(scope) << ").";
        }
      }
      return clear_color_profile_from_registry(device_path, scope) || api_success;
    }

    bool write_color_profile_association(
      const std::wstring &device_path,
      const std::wstring &profile_filename,
      const color_profile_scope_e scope,
      LSTATUS *out_status = nullptr
    ) {
      // Keep the legacy value populated first. Besides supporting older Windows builds,
      // this preserves the existing scope-selection state before the modern API activates it.
      LSTATUS registry_status = ERROR_SUCCESS;
      const bool registry_success = write_color_profile_to_registry(
        device_path,
        profile_filename,
        scope,
        &registry_status
      );
      const auto result = VDISPLAY::set_advanced_color_profile(
        device_path,
        profile_filename,
        is_system_wide_profile_scope(scope)
      );
      if (out_status) {
        *out_status = registry_status;
      }
      if (result.success) {
        return true;
      }
      if (result.attempted) {
        BOOST_LOG(warning) << "HDR profile: Advanced Color activation failed (add=0x"
                           << std::hex << static_cast<unsigned long>(result.association_status)
                           << ", default=0x" << static_cast<unsigned long>(result.default_status) << std::dec
                           << ", scope=" << color_profile_scope_label(scope) << "); retained registry association only.";
      } else if (result.api_available && !result.target_found) {
        BOOST_LOG(warning) << "HDR profile: active DisplayConfig target was unavailable; retained registry association only"
                           << " (scope=" << color_profile_scope_label(scope) << ").";
      }
      return result.api_available ? false : registry_success;
    }

    void apply_hdr_profile_if_available(
      const std::optional<std::wstring> &display_name,
      const std::optional<std::string> &device_id,
      const std::optional<std::wstring> &monitor_device_path,
      const std::optional<std::string> &client_name_utf8,
      const std::optional<std::string> &hdr_profile_utf8,
      bool is_virtual_display = true,
      bool wait_for_completion = false,
      std::stop_token stop_token = {},
      std::optional<std::string> deferred_worker_key = std::nullopt
    ) {
      if (stop_token.stop_requested()) {
        return;
      }
      // Physical outputs are left untouched unless the user explicitly selected
      // a profile. Virtual outputs are different: Windows can reuse a monitor
      // class instance whose registry association belongs to an older display,
      // so an empty selection must actively clear that stale association.
      const bool has_profile_selection = hdr_profile_utf8 && !hdr_profile_utf8->empty();
      if (!has_profile_selection && !is_virtual_display) {
        return;
      }

      const std::string client_name = (client_name_utf8 && !client_name_utf8->empty()) ? *client_name_utf8 : "unknown";

      std::optional<fs::path> profile_path;
      if (has_profile_selection) {
        profile_path = find_hdr_profile_by_selection(*hdr_profile_utf8);
      }
      if (has_profile_selection && !profile_path) {
        BOOST_LOG(warning) << "HDR profile: configured profile '" << *hdr_profile_utf8 << "' not found in '"
                           << platf::to_utf8(default_color_profile_directory().wstring()) << "' for client '" << client_name
                           << "'.";
        return;
      }

      // For virtual displays, clear mismatched associations (Windows can reuse IDs).
      const bool should_clear_mismatched = is_virtual_display;
      // Recovery work is bound to one GUID. If that display disappears, never
      // fall back to an arbitrary active virtual target: it may belong to a
      // newer session that reused the driver while this worker was winding down.
      const bool allow_active_virtual_display_fallback = !deferred_worker_key.has_value();

      auto apply_profile_work = [profile_path,
                                 client_name,
                                 monitor_path = monitor_device_path,
                                 display_name,
                                 device_id,
                                 should_clear_mismatched,
                                 allow_active_virtual_display_fallback](std::stop_token work_stop_token) {
        if (work_stop_token.stop_requested()) {
          return;
        }
        std::optional<std::wstring> device_name_w = monitor_path;
        if (!device_name_w || device_name_w->empty()) {
          // Resolve monitor path - allow up to 5 seconds for display to be enumerable
          if (should_clear_mismatched) {
            // Virtual displays: avoid relying on the client name (it may be stale/incorrect) and instead target the
            // active Sunshine virtual display when present. Prefer the explicit display identifiers first.
            device_name_w = resolve_monitor_device_path(display_name, device_id, 50, std::chrono::milliseconds(100), std::nullopt, work_stop_token);

            if (allow_active_virtual_display_fallback &&
                !work_stop_token.stop_requested() && (!device_name_w || device_name_w->empty())) {
              const auto active_vd_name = resolve_virtual_display_name_from_devices();
              const auto active_vd_device_id = resolveAnyVirtualDisplayDeviceId();
              if (active_vd_name || active_vd_device_id) {
                BOOST_LOG(debug) << "HDR profile: virtual display monitor path unresolved; falling back to active virtual display."
                                 << " active_name='" << (active_vd_name ? platf::to_utf8(*active_vd_name) : std::string("(none)"))
                                 << "' active_device_id='" << (active_vd_device_id ? *active_vd_device_id : std::string("(none)")) << "'.";
                device_name_w = resolve_monitor_device_path(active_vd_name, active_vd_device_id, 50, std::chrono::milliseconds(100), std::nullopt, work_stop_token);
              }
            }
          } else {
            // Physical displays: prefer explicit identifiers (device_id/display_name) and fall back to the current primary.
            std::optional<std::wstring> physical_display_name = display_name;
            std::optional<std::string> physical_device_id = device_id;
            if ((!physical_display_name || physical_display_name->empty()) && (!physical_device_id || physical_device_id->empty())) {
              physical_display_name = primary_gdi_display_name();
              BOOST_LOG(debug) << "HDR profile: applying to primary physical display for client '" << client_name << "'.";
            } else {
              BOOST_LOG(debug) << "HDR profile: applying to physical display for client '" << client_name
                               << "' display_name='" << (physical_display_name ? platf::to_utf8(*physical_display_name) : std::string("(none)"))
                               << "' device_id='" << (physical_device_id ? *physical_device_id : std::string("(none)")) << "'.";
            }
            device_name_w = resolve_monitor_device_path(physical_display_name, physical_device_id, 50, std::chrono::milliseconds(100), std::nullopt, work_stop_token);
          }
        }
        if (work_stop_token.stop_requested()) {
          return;
        }
        if (!device_name_w || device_name_w->empty()) {
          if (profile_path) {
            BOOST_LOG(warning) << "HDR profile: skipped - monitor device path unavailable for '" << client_name << "'.";
            BOOST_LOG(debug) << "HDR profile: resolve context display_name='"
                             << (display_name ? platf::to_utf8(*display_name) : std::string("(none)"))
                             << "' device_id='" << (device_id ? *device_id : std::string("(none)")) << "'.";
          }
          return;
        }

        bool success = false;
        bool already_associated = false;
        bool cleared_mismatched = false;

        const bool running_as_system = platf::is_running_as_system();

        auto apply_profile_for_scope = [&](color_profile_scope_e scope) -> std::pair<bool, bool> {
          if (work_stop_token.stop_requested()) {
            return {false, false};
          }
          bool local_success = false;
          bool local_access_denied = false;

          std::optional<std::wstring> existing;
          if (should_clear_mismatched || profile_path) {
            existing = read_color_profile_association(*device_name_w, scope);
          }

          // For physical displays, remember the pre-stream association so we can restore it on stream end.
          if (scope == color_profile_scope_e::current_user && !should_clear_mismatched && profile_path) {
            const bool has_existing = existing && !existing->empty();
            std::lock_guard<std::mutex> lock(g_physical_hdr_profile_restore_mutex);
            if (g_physical_hdr_profile_restore.find(*device_name_w) == g_physical_hdr_profile_restore.end()) {
              if (has_existing) {
                g_physical_hdr_profile_restore.emplace(*device_name_w, *existing);
              } else {
                g_physical_hdr_profile_restore.emplace(*device_name_w, std::nullopt);
              }
            }
          }

          // Check existing profile and handle mismatches for virtual displays
          if (should_clear_mismatched) {
            if (existing && !existing->empty()) {
              // Determine expected filename
              std::wstring expected_filename;
              if (profile_path) {
                expected_filename = profile_path->filename().wstring();
              }

              // If no profile for this client, or existing doesn't match expected, clear it
              if (expected_filename.empty() ||
                  _wcsicmp(fs::path(*existing).filename().c_str(), expected_filename.c_str()) != 0) {
                if (work_stop_token.stop_requested()) {
                  return {false, false};
                }
                BOOST_LOG(debug) << "HDR profile: clearing mismatched profile '" << platf::to_utf8(*existing)
                                 << "' from virtual display for client '" << client_name << "'.";
                if (clear_color_profile_association(*device_name_w, existing, scope)) {
                  cleared_mismatched = true;
                } else {
                  BOOST_LOG(debug) << "HDR profile: failed to clear mismatched profile association for client '" << client_name
                                   << "' (monitor path: '" << platf::to_utf8(*device_name_w) << "').";
                }
              }
            }
          }

          // If we have a profile to apply, do it
          if (profile_path) {
            if (work_stop_token.stop_requested()) {
              return {false, false};
            }
            const auto profile_filename = profile_path->filename().wstring();

            const bool desired_already_associated =
              !cleared_mismatched &&
              existing &&
              !existing->empty() &&
              _wcsicmp(fs::path(*existing).filename().c_str(), profile_filename.c_str()) == 0;

            if (desired_already_associated) {
              already_associated = true;
            }

            BOOST_LOG(debug) << "HDR profile: applying '" << profile_path->filename().string() << "' for client '" << client_name << "'.";

            // Install the color profile if needed
            if (!InstallColorProfileW(nullptr, profile_path->c_str())) {
              const auto err = GetLastError();
              if (err != ERROR_ALREADY_EXISTS && err != ERROR_FILE_EXISTS) {
                BOOST_LOG(warning) << "HDR profile: InstallColorProfileW failed (" << err << ") for '"
                                   << platf::to_utf8(profile_path->filename().wstring()) << "'; attempting registry association anyway.";
              }
            }

            if (work_stop_token.stop_requested()) {
              return {false, false};
            }

            // Advanced Color associations notify Windows to consume the MHC2 luminance metadata.
            // Keep the registry write only as a compatibility fallback for older systems.
            LSTATUS reg_status = ERROR_SUCCESS;
            local_success = write_color_profile_association(*device_name_w, profile_filename, scope, &reg_status);
            if (!local_success) {
              if (reg_status == ERROR_ACCESS_DENIED) {
                local_access_denied = true;
              }
              BOOST_LOG(warning) << "HDR profile: failed to associate '" << platf::to_utf8(profile_filename)
                                 << "' with monitor '" << platf::to_utf8(*device_name_w) << "' for client '" << client_name
                                 << "' (scope=" << color_profile_scope_label(scope) << ").";
            }
          }
          return {local_success, local_access_denied};
        };

        auto apply_profile = [&]() {
          if (work_stop_token.stop_requested()) {
            return;
          }
          const auto [local_success, local_access_denied] = apply_profile_for_scope(color_profile_scope_e::current_user);
          success = local_success;

          if (!work_stop_token.stop_requested() && !success && should_clear_mismatched && running_as_system && local_access_denied) {
            BOOST_LOG(debug) << "HDR profile: access denied in current-user scope; retrying system-wide association for monitor '"
                             << platf::to_utf8(*device_name_w) << "'.";
            const auto [system_success, _] = apply_profile_for_scope(color_profile_scope_e::system_wide);
            success = system_success;
          }
        };

        HANDLE user_token = platf::retrieve_users_token(false);
        if (user_token) {
          const auto impersonation_ec = platf::impersonate_current_user(user_token, apply_profile);
          if (impersonation_ec) {
            BOOST_LOG(debug) << "HDR profile: impersonation failed (ec=" << impersonation_ec.value() << ") for '" << client_name << "'.";
          }
          CloseHandle(user_token);
        } else {
          DWORD session_id = 0;
          if (!ProcessIdToSessionId(GetCurrentProcessId(), &session_id) || session_id == 0) {
            if (profile_path) {
              BOOST_LOG(warning) << "HDR profile: skipped - unable to retrieve user token for '" << client_name << "'.";
            }
            return;
          }
          BOOST_LOG(debug) << "HDR profile: no user token; applying in current user context for '" << client_name << "'.";
          apply_profile();
        }

        if (work_stop_token.stop_requested()) {
          return;
        }
        if (success && profile_path) {
          if (already_associated) {
            BOOST_LOG(info) << "HDR color profile '" << platf::to_utf8(profile_path->filename().wstring())
                            << "' already associated for client '" << client_name << "'.";
          } else {
            BOOST_LOG(info) << "Applied HDR color profile '" << platf::to_utf8(profile_path->filename().wstring())
                            << "' for client '" << client_name << "'.";
          }
        } else if (cleared_mismatched && !profile_path) {
          BOOST_LOG(info) << "Cleared mismatched HDR color profile association for client '" << client_name << "'.";
        }
      };

      const bool monitor_path_ready = monitor_device_path && !monitor_device_path->empty();
      if (wait_for_completion && monitor_path_ready) {
        apply_profile_work(stop_token);
      } else {
        if (wait_for_completion) {
          // A newly enumerated virtual target may not have a monitor device path until the
          // display helper activates it. The helper cannot receive APPLY until creation
          // returns, so waiting here would only exhaust the resolver's retry budget.
          BOOST_LOG(debug) << "HDR profile: deferring virtual display profile work until the pending monitor path becomes available.";
        }
        if (deferred_worker_key) {
          const auto started = deferred_hdr_profile_workers().start(
            std::move(*deferred_worker_key),
            [work = std::move(apply_profile_work), parent_stop_token = stop_token](std::stop_token worker_stop_token) mutable {
              std::stop_source combined_stop_source;
              std::stop_callback parent_stop_callback {
                parent_stop_token,
                [&combined_stop_source] { combined_stop_source.request_stop(); }
              };
              std::stop_callback worker_stop_callback {
                worker_stop_token,
                [&combined_stop_source] { combined_stop_source.request_stop(); }
              };
              work(combined_stop_source.get_token());
            }
          );
          if (!started) {
            BOOST_LOG(debug) << "HDR profile: deferred recovery work was not started because shutdown is in progress.";
          }
        } else {
          std::thread([work = std::move(apply_profile_work)]() mutable {
            work({});
          }).detach();
        }
      }
    }

    std::uint16_t virtual_display_product_code_from_display_id(const std::uint64_t display_id) {
      return static_cast<std::uint16_t>(0x5000u | (display_id & 0x0fffu));
    }

    std::string virtual_display_product_code_string_from_display_id(const std::uint64_t display_id) {
      std::ostringstream stream;
      stream << std::setfill('0')
             << std::setw(4)
             << std::hex
             << std::uppercase
             << virtual_display_product_code_from_display_id(display_id);
      return stream.str();
    }

    std::uint32_t virtual_display_serial_number_from_display_id(const std::uint64_t display_id) {
      const auto folded = static_cast<std::uint32_t>(display_id) ^
                          static_cast<std::uint32_t>(display_id >> 32);
      return folded == 0 ? 1 : folded;
    }

    bool matches_virtual_display_id_edid(
      const display_device::EnumeratedDevice &device,
      const std::uint64_t display_id
    ) {
      if (!device.m_edid) {
        return false;
      }
      if (!equals_ci(device.m_edid->m_manufacturer_id, "SDD")) {
        return false;
      }

      const auto expected_product_code = virtual_display_product_code_string_from_display_id(display_id);
      const auto expected_serial = virtual_display_serial_number_from_display_id(display_id);
      return equals_ci(device.m_edid->m_product_code, expected_product_code) &&
             device.m_edid->m_serial_number == expected_serial;
    }

    std::wstring virtual_display_dpi_settings_prefix(const std::uint64_t display_id) {
      wchar_t hardware_id[8] {};
      std::swprintf(
        hardware_id,
        std::size(hardware_id),
        L"SDD%04X",
        static_cast<unsigned int>(virtual_display_product_code_from_display_id(display_id))
      );
      return std::wstring {hardware_id} + std::to_wstring(virtual_display_serial_number_from_display_id(display_id));
    }

    bool starts_with(const std::wstring_view value, const std::wstring_view prefix) {
      return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
    }

    struct virtual_display_dpi_snapshot_t {
      std::wstring settings_prefix;
      std::map<std::wstring, uint32_t> source_values;
      uint32_t value {};
    };

    std::optional<uint32_t> read_dpi_value(HKEY key) {
      DWORD value = 0;
      DWORD value_size = sizeof(value);
      const LSTATUS query_status = RegGetValueW(key, nullptr, L"DpiValue", RRF_RT_REG_DWORD, nullptr, &value, &value_size);
      if (query_status != ERROR_SUCCESS || value_size != sizeof(value)) {
        return std::nullopt;
      }
      return value;
    }

    std::optional<virtual_display_dpi_snapshot_t> read_virtual_display_dpi_value(const std::wstring &settings_prefix) {
      HKEY root = nullptr;
      if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Control Panel\\Desktop\\PerMonitorSettings",
            0,
            KEY_ENUMERATE_SUB_KEYS,
            &root
          ) != ERROR_SUCCESS) {
        return std::nullopt;
      }

      std::map<std::wstring, uint32_t> source_values;
      std::optional<uint32_t> snapshot_value;
      bool conflicting_values = false;
      wchar_t name[256];
      for (DWORD index = 0;; ++index) {
        DWORD name_len = _countof(name);
        const LSTATUS enum_status = RegEnumKeyExW(root, index, name, &name_len, nullptr, nullptr, nullptr, nullptr);
        if (enum_status == ERROR_NO_MORE_ITEMS) {
          break;
        }
        if (enum_status != ERROR_SUCCESS) {
          continue;
        }
        std::wstring_view key_name {name, name_len};
        if (!starts_with(key_name, settings_prefix)) {
          continue;
        }

        const std::wstring key_name_string {key_name};
        HKEY subkey = nullptr;
        if (RegOpenKeyExW(root, key_name_string.c_str(), 0, KEY_QUERY_VALUE, &subkey) != ERROR_SUCCESS) {
          continue;
        }
        auto value = read_dpi_value(subkey);
        RegCloseKey(subkey);
        if (value) {
          if (snapshot_value && *snapshot_value != *value) {
            conflicting_values = true;
          } else {
            snapshot_value = *value;
          }
          source_values.emplace(std::move(key_name_string), *value);
        }
      }

      RegCloseKey(root);
      if (conflicting_values) {
        BOOST_LOG(info) << "Sunshine virtual display DPI: skipped cached value for " << platf::to_utf8(settings_prefix)
                        << " because matching settings disagree.";
        return std::nullopt;
      }
      if (!snapshot_value || source_values.empty()) {
        return std::nullopt;
      }
      return virtual_display_dpi_snapshot_t {
        settings_prefix,
        std::move(source_values),
        *snapshot_value
      };
    }

    bool dpi_snapshot_is_still_current(HKEY root, const virtual_display_dpi_snapshot_t &snapshot) {
      for (const auto &[source_key_name, source_value] : snapshot.source_values) {
        HKEY source_key = nullptr;
        if (RegOpenKeyExW(root, source_key_name.c_str(), 0, KEY_QUERY_VALUE, &source_key) != ERROR_SUCCESS) {
          return false;
        }
        const auto current_value = read_dpi_value(source_key);
        RegCloseKey(source_key);
        if (!current_value || *current_value != source_value) {
          return false;
        }
      }
      return true;
    }

    bool apply_virtual_display_dpi_value(const virtual_display_dpi_snapshot_t &snapshot) {
      HKEY root = nullptr;
      if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Control Panel\\Desktop\\PerMonitorSettings",
            0,
            KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE,
            &root
          ) != ERROR_SUCCESS) {
        return false;
      }

      if (!dpi_snapshot_is_still_current(root, snapshot)) {
        RegCloseKey(root);
        BOOST_LOG(info) << "Sunshine virtual display DPI: skipped stale cached value for " << platf::to_utf8(snapshot.settings_prefix)
                        << " because the source setting changed.";
        return false;
      }

      bool applied = false;
      wchar_t name[256];
      for (DWORD index = 0;; ++index) {
        DWORD name_len = _countof(name);
        const LSTATUS enum_status = RegEnumKeyExW(root, index, name, &name_len, nullptr, nullptr, nullptr, nullptr);
        if (enum_status == ERROR_NO_MORE_ITEMS) {
          break;
        }
        if (enum_status != ERROR_SUCCESS) {
          continue;
        }
        std::wstring_view key_name {name, name_len};
        if (!starts_with(key_name, snapshot.settings_prefix)) {
          continue;
        }

        const std::wstring key_name_string {key_name};
        HKEY subkey = nullptr;
        if (RegOpenKeyExW(root, key_name_string.c_str(), 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &subkey) != ERROR_SUCCESS) {
          continue;
        }

        const auto current_value = read_dpi_value(subkey);
        LSTATUS status = ERROR_SUCCESS;
        if (!current_value || *current_value != snapshot.value) {
          if (!dpi_snapshot_is_still_current(root, snapshot)) {
            RegCloseKey(subkey);
            break;
          }

          const DWORD data = snapshot.value;
          status = RegSetValueExW(
            subkey,
            L"DpiValue",
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE *>(&data),
            sizeof(data)
          );
        }
        RegCloseKey(subkey);
        if (status == ERROR_SUCCESS) {
          applied = true;
        }
      }

      RegCloseKey(root);
      if (applied) {
        printf(
          "[SunshineVirtualDisplay] Applied cached virtual display DPI value: %u (%ls)\n",
          static_cast<unsigned int>(snapshot.value),
          snapshot.settings_prefix.c_str()
        );
      }
      return applied;
    }

    fs::path legacy_virtual_display_cache_path() {
      return platf::appdata() / "virtual_display_cache.json";
    }

    namespace pt = boost::property_tree;

    bool contains_ci(const std::string &haystack, const std::string &needle) {
      if (needle.empty()) {
        return true;
      }
      if (haystack.size() < needle.size()) {
        return false;
      }
      for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
          if (std::tolower(static_cast<unsigned char>(haystack[i + j])) != std::tolower(static_cast<unsigned char>(needle[j]))) {
            match = false;
            break;
          }
        }
        if (match) {
          return true;
        }
      }
      return false;
    }

    bool starts_with_ci(const std::string &value, const std::string &prefix) {
      if (value.size() < prefix.size()) {
        return false;
      }
      for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(value[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) {
          return false;
        }
      }
      return true;
    }

    bool is_virtual_display_device(const display_device::EnumeratedDevice &device) {
      if (!device.m_monitor_device_path.empty()) {
        if (contains_ci(device.m_monitor_device_path, "SunshineVirtualDisplay") ||
            contains_ci(device.m_monitor_device_path, "Sunshine Virtual Display")) {
          return true;
        }
      }

      static const std::string sunshineDeviceString = "Sunshine Virtual Display Driver";
      if (equals_ci(device.m_friendly_name, sunshineDeviceString)) {
        return true;
      }

      if (device.m_edid && equals_ci(device.m_edid->m_manufacturer_id, "SDD") &&
          (starts_with_ci(device.m_edid->m_product_code, "4") ||
           starts_with_ci(device.m_edid->m_product_code, "0x4") ||
           starts_with_ci(device.m_edid->m_product_code, "5") ||
           starts_with_ci(device.m_edid->m_product_code, "0x5"))) {
        return true;
      }

      return false;
    }

    bool luid_equals(const LUID &lhs, const LUID &rhs) {
      return lhs.LowPart == rhs.LowPart && lhs.HighPart == rhs.HighPart;
    }

    struct DisplayConfigIdentity {
      std::optional<std::wstring> source_gdi_device_name;
      std::optional<std::wstring> monitor_device_path;
      std::optional<std::wstring> monitor_friendly_device_name;
    };

    struct DisplayConfigTarget {
      LUID AdapterLuid;
      UINT TargetId;
    };

    struct DisplayConfigQuery {
      std::vector<DISPLAYCONFIG_PATH_INFO> paths;
      std::vector<DISPLAYCONFIG_MODE_INFO> modes;
      bool virtual_mode_aware = false;
    };

    VDISPLAY::policy::display_config_target_key target_key(const DisplayConfigTarget &target) {
      return {
        target.AdapterLuid.LowPart,
        target.AdapterLuid.HighPart,
        target.TargetId,
      };
    }

    VDISPLAY::policy::display_config_path_state path_state(const DISPLAYCONFIG_PATH_INFO &path) {
      return {
        {
          path.targetInfo.adapterId.LowPart,
          path.targetInfo.adapterId.HighPart,
          path.targetInfo.id,
        },
        (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0,
        path.targetInfo.targetAvailable != FALSE,
        {
          path.sourceInfo.adapterId.LowPart,
          path.sourceInfo.adapterId.HighPart,
          path.sourceInfo.id,
        },
      };
    }

    LONG query_display_config(const UINT flags, DisplayConfigQuery &query) {
      UINT path_count = 0;
      UINT mode_count = 0;
      auto result = GetDisplayConfigBufferSizes(flags, &path_count, &mode_count);
      if (result != ERROR_SUCCESS) {
        return result;
      }

      if (!VDISPLAY::policy::display_config_buffer_sizes_are_sane(path_count, mode_count)) {
        BOOST_LOG(warning) << "DisplayConfig reported an implausible buffer size (flags=" << flags
                           << ", paths=" << path_count << ", modes=" << mode_count << ").";
        return ERROR_INVALID_DATA;
      }
      // The activation retry loop queries every 50 ms, so only report a topology
      // whose size actually changed. This is the number that silently rejected
      // every activation while the buffer bounds were set too low.
      static std::atomic<std::uint64_t> last_reported_buffer_sizes {~0ull};
      const auto buffer_sizes = (static_cast<std::uint64_t>(path_count) << 32) | mode_count;
      if (last_reported_buffer_sizes.exchange(buffer_sizes) != buffer_sizes) {
        BOOST_LOG(debug) << "DisplayConfig query flags=" << flags << " paths=" << path_count
                         << " modes=" << mode_count << '.';
      }

      query.paths.resize(path_count);
      query.modes.resize(mode_count);
      for (int attempt = 0; attempt < 4; ++attempt) {
        UINT queried_path_count = static_cast<UINT>(query.paths.size());
        UINT queried_mode_count = static_cast<UINT>(query.modes.size());
        result = QueryDisplayConfig(
          flags,
          &queried_path_count,
          queried_path_count ? query.paths.data() : nullptr,
          &queried_mode_count,
          queried_mode_count ? query.modes.data() : nullptr,
          nullptr
        );
        if (result == ERROR_SUCCESS) {
          if (!VDISPLAY::policy::display_config_buffer_sizes_are_sane(queried_path_count, queried_mode_count)) {
            return ERROR_INVALID_DATA;
          }
          query.paths.resize(queried_path_count);
          query.modes.resize(queried_mode_count);
          query.virtual_mode_aware = (flags & QDC_VIRTUAL_MODE_AWARE) != 0;
          return ERROR_SUCCESS;
        }
        if (result != ERROR_INSUFFICIENT_BUFFER ||
            !VDISPLAY::policy::display_config_buffer_sizes_are_sane(queried_path_count, queried_mode_count)) {
          return result;
        }
        query.paths.resize((std::max)(queried_path_count, static_cast<UINT>(query.paths.size() + 1)));
        query.modes.resize((std::max)(queried_mode_count, static_cast<UINT>(query.modes.size() + 1)));
      }
      return result;
    }

    LONG query_all_display_config_paths(DisplayConfigQuery &query) {
      auto result = query_display_config(QDC_ALL_PATHS | QDC_VIRTUAL_MODE_AWARE, query);
      if (result == ERROR_SUCCESS) {
        return result;
      }
      query = {};
      return query_display_config(QDC_ALL_PATHS, query);
    }

    std::optional<UINT> source_mode_index(
      const DISPLAYCONFIG_PATH_INFO &path,
      const bool virtual_mode_aware
    ) {
      if (virtual_mode_aware) {
        if (path.sourceInfo.sourceModeInfoIdx == DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID) {
          return std::nullopt;
        }
        return path.sourceInfo.sourceModeInfoIdx;
      }
      if (path.sourceInfo.modeInfoIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID) {
        return std::nullopt;
      }
      return path.sourceInfo.modeInfoIdx;
    }

    POINTL next_extended_position(const DisplayConfigQuery &query) {
      std::int64_t right_edge = 0;
      for (const auto &path : query.paths) {
        if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0) {
          continue;
        }
        const auto mode_index = source_mode_index(path, query.virtual_mode_aware);
        if (!mode_index || *mode_index >= query.modes.size()) {
          continue;
        }
        const auto &mode = query.modes[*mode_index];
        if (mode.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
          continue;
        }
        right_edge = (std::max)(
          right_edge,
          static_cast<std::int64_t>(mode.sourceMode.position.x) + mode.sourceMode.width
        );
      }
      return {
        static_cast<LONG>((std::min)(right_edge, static_cast<std::int64_t>((std::numeric_limits<LONG>::max)()))),
        0,
      };
    }

    DISPLAYCONFIG_VIDEO_SIGNAL_INFO make_activation_signal_info(
      const std::uint32_t width,
      const std::uint32_t height,
      const std::uint32_t refresh_millihz
    ) {
      DISPLAYCONFIG_VIDEO_SIGNAL_INFO signal {};
      const auto pixel_rate =
        (static_cast<std::uint64_t>(width) * height * refresh_millihz) / 1000ull;
      signal.pixelRate = pixel_rate;
      signal.hSyncFreq.Numerator = static_cast<UINT32>((std::min)(
        static_cast<std::uint64_t>((std::numeric_limits<UINT32>::max)()),
        static_cast<std::uint64_t>(refresh_millihz) * height
      ));
      signal.hSyncFreq.Denominator = 1000;
      signal.vSyncFreq.Numerator = refresh_millihz;
      signal.vSyncFreq.Denominator = 1000;
      signal.activeSize.cx = width;
      signal.activeSize.cy = height;
      signal.totalSize = signal.activeSize;
      signal.AdditionalSignalInfo.videoStandard = 255;
      signal.AdditionalSignalInfo.vSyncFreqDivider = 1;
      signal.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
      return signal;
    }

    LONG activate_display_config_target_once_inner(
      const DisplayConfigTarget &output,
      const std::uint32_t width,
      const std::uint32_t height,
      const std::uint32_t refresh_millihz
    ) {
      if (width == 0 || height == 0 || refresh_millihz == 0) {
        return ERROR_INVALID_PARAMETER;
      }

      DisplayConfigQuery query;
      const auto query_result = query_all_display_config_paths(query);
      if (query_result != ERROR_SUCCESS) {
        return query_result;
      }

      std::vector<VDISPLAY::policy::display_config_path_state> path_states;
      path_states.reserve(query.paths.size());
      for (const auto &path : query.paths) {
        path_states.push_back(path_state(path));
      }

      const auto requested_target = target_key(output);
      const auto activation_plan = VDISPLAY::policy::plan_exact_target_activation(
        path_states,
        requested_target
      );
      if (activation_plan.action == VDISPLAY::policy::exact_target_activation_action::already_active) {
        return ERROR_SUCCESS;
      }
      if (activation_plan.action != VDISPLAY::policy::exact_target_activation_action::activate ||
          activation_plan.path_index >= query.paths.size()) {
        return ERROR_NOT_FOUND;
      }

      std::vector<DISPLAYCONFIG_PATH_INFO> requested_paths;
      requested_paths.reserve(query.paths.size());
      for (const auto &path : query.paths) {
        const auto state = path_state(path);
        if (state.target != requested_target &&
            VDISPLAY::policy::path_active_after_exact_target_activation(state, requested_target)) {
          requested_paths.push_back(path);
        }
      }

      auto requested_target_path = query.paths[activation_plan.path_index];
      requested_target_path.flags |= DISPLAYCONFIG_PATH_ACTIVE;
      requested_target_path.targetInfo.targetAvailable = TRUE;
      requested_target_path.targetInfo.refreshRate = {refresh_millihz, 1000};
      requested_target_path.targetInfo.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;

      auto requested_modes = query.modes;
      const auto source_mode_info_index = static_cast<UINT>(requested_modes.size());
      DISPLAYCONFIG_MODE_INFO source_mode {};
      source_mode.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
      source_mode.id = requested_target_path.sourceInfo.id;
      source_mode.adapterId = requested_target_path.sourceInfo.adapterId;
      source_mode.sourceMode.width = width;
      source_mode.sourceMode.height = height;
      source_mode.sourceMode.pixelFormat = DISPLAYCONFIG_PIXELFORMAT_32BPP;
      source_mode.sourceMode.position = next_extended_position(query);
      requested_modes.push_back(source_mode);

      // A hand-built DISPLAYCONFIG_VIDEO_SIGNAL_INFO cannot match a timing the
      // target actually advertises, so let Windows pick the target mode first and
      // keep the synthesized descriptor as a fallback. The refreshRate and
      // scanLineOrdering set above remain the hint under SDC_ALLOW_CHANGES.
      const auto apply_activation = [&](const bool supply_target_mode) -> LONG {
        auto paths = requested_paths;
        auto modes = requested_modes;
        auto path = requested_target_path;

        UINT target_mode_info_index = 0;
        if (supply_target_mode) {
          target_mode_info_index = static_cast<UINT>(modes.size());
          DISPLAYCONFIG_MODE_INFO target_mode {};
          target_mode.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;
          target_mode.id = path.targetInfo.id;
          target_mode.adapterId = path.targetInfo.adapterId;
          target_mode.targetMode.targetVideoSignalInfo = make_activation_signal_info(width, height, refresh_millihz);
          modes.push_back(target_mode);
        }

        if (query.virtual_mode_aware) {
          path.sourceInfo.sourceModeInfoIdx = source_mode_info_index;
          path.sourceInfo.cloneGroupId = VDISPLAY::policy::exact_target_activation_clone_group_id();
          path.targetInfo.targetModeInfoIdx =
            supply_target_mode ? target_mode_info_index : DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID;
          path.targetInfo.desktopModeInfoIdx = DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;
        } else {
          path.sourceInfo.modeInfoIdx = source_mode_info_index;
          path.targetInfo.modeInfoIdx =
            supply_target_mode ? target_mode_info_index : DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        }
        paths.push_back(path);

        // SDC_ALLOW_PATH_ORDER_CHANGES is only legal alongside SDC_TOPOLOGY_SUPPLIED,
        // which is mutually exclusive with SDC_USE_SUPPLIED_DISPLAY_CONFIG.
        return SetDisplayConfig(
          static_cast<UINT>(paths.size()),
          paths.data(),
          static_cast<UINT>(modes.size()),
          modes.data(),
          SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES |
            (query.virtual_mode_aware ? SDC_VIRTUAL_MODE_AWARE : 0)
        );
      };

      auto apply_result = apply_activation(false);
      if (apply_result != ERROR_SUCCESS) {
        BOOST_LOG(debug) << "Exact virtual display activation: OS-selected target timing was refused (error="
                         << apply_result << "); retrying with an explicit mode descriptor.";
        apply_result = apply_activation(true);
      }
      if (apply_result != ERROR_SUCCESS) {
        return apply_result;
      }

      DisplayConfigQuery verified_query;
      const auto verification_result = query_all_display_config_paths(verified_query);
      if (verification_result != ERROR_SUCCESS) {
        return verification_result;
      }
      std::vector<VDISPLAY::policy::display_config_path_state> verified_states;
      verified_states.reserve(verified_query.paths.size());
      for (const auto &path : verified_query.paths) {
        verified_states.push_back(path_state(path));
      }
      return VDISPLAY::policy::plan_exact_target_activation(verified_states, requested_target).action ==
                 VDISPLAY::policy::exact_target_activation_action::already_active ?
               ERROR_SUCCESS :
               ERROR_RETRY;
    }

    LONG activate_display_config_target_once(
      const DisplayConfigTarget &output,
      const std::uint32_t width,
      const std::uint32_t height,
      const std::uint32_t refresh_millihz
    ) {
      auto result = activate_display_config_target_once_inner(output, width, height, refresh_millihz);
      if (result == ERROR_SUCCESS) {
        return result;
      }

      HANDLE user_token = platf::retrieve_users_token(false);
      if (!user_token) {
        return result;
      }

      const auto impersonation_ec = platf::impersonate_current_user(user_token, [&]() {
        result = activate_display_config_target_once_inner(output, width, height, refresh_millihz);
      });
      CloseHandle(user_token);
      if (impersonation_ec) {
        BOOST_LOG(debug) << "Exact virtual display activation: impersonation failed.";
      }
      return result;
    }

    bool activate_display_config_target(
      const DisplayConfigTarget &output,
      const std::uint32_t width,
      const std::uint32_t height,
      const std::uint32_t refresh_millihz,
      std::stop_token stop_token = {}
    ) {
      const auto deadline = std::chrono::steady_clock::now() + VDISPLAY::policy::enumeration_timeout;
      LONG result = ERROR_NOT_FOUND;
      do {
        if (stop_token.stop_requested()) {
          return false;
        }
        result = activate_display_config_target_once(output, width, height, refresh_millihz);
        if (result == ERROR_SUCCESS) {
          BOOST_LOG(debug) << "Activated exact Sunshine virtual display target " << output.TargetId
                           << " on adapter " << output.AdapterLuid.HighPart << ':' << output.AdapterLuid.LowPart << '.';
          return true;
        }
        if (wait_for_monitor_stop(stop_token, VDISPLAY::policy::readiness_poll_interval)) {
          return false;
        }
      } while (std::chrono::steady_clock::now() < deadline);

      BOOST_LOG(error) << "Unable to activate exact Sunshine virtual display target " << output.TargetId
                       << " on adapter " << output.AdapterLuid.HighPart << ':' << output.AdapterLuid.LowPart
                       << " (error=" << result << ").";
      return false;
    }

    struct AdvancedColorInfo {
      bool supported = false;
      bool active = false;
      bool limited_by_policy = false;
      bool hdr_supported = false;
      bool hdr_enabled = false;
      DISPLAYCONFIG_COLOR_ENCODING color_encoding = DISPLAYCONFIG_COLOR_ENCODING_RGB;
      UINT32 bits_per_color_channel = 0;
      UINT32 active_color_mode = 0;
    };

    struct DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2_LOCAL {
      DISPLAYCONFIG_DEVICE_INFO_HEADER header;
      union {
        struct {
          UINT32 advancedColorSupported : 1;
          UINT32 advancedColorActive : 1;
          UINT32 reserved1 : 1;
          UINT32 advancedColorLimitedByPolicy : 1;
          UINT32 highDynamicRangeSupported : 1;
          UINT32 highDynamicRangeUserEnabled : 1;
          UINT32 wideColorSupported : 1;
          UINT32 wideColorUserEnabled : 1;
          UINT32 reserved : 24;
        };
        UINT32 value;
      };
      DISPLAYCONFIG_COLOR_ENCODING colorEncoding;
      UINT32 bitsPerColorChannel;
      UINT32 activeColorMode;
    };

    struct DISPLAYCONFIG_SET_HDR_STATE_LOCAL {
      DISPLAYCONFIG_DEVICE_INFO_HEADER header;
      union {
        struct {
          UINT32 enableHdr : 1;
          UINT32 reserved : 31;
        };
        UINT32 value;
      };
    };

    std::optional<AdvancedColorInfo> query_advanced_color_inner(const DisplayConfigTarget &output) {
      DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2_LOCAL info {};
      info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2;
      info.header.size = sizeof(info);
      info.header.adapterId = output.AdapterLuid;
      info.header.id = output.TargetId;
      const LONG result = DisplayConfigGetDeviceInfo(&info.header);
      if (result == ERROR_SUCCESS) {
        return AdvancedColorInfo {
          info.advancedColorSupported != 0,
          info.advancedColorActive != 0,
          info.advancedColorLimitedByPolicy != 0,
          info.highDynamicRangeSupported != 0,
          info.highDynamicRangeUserEnabled != 0,
          info.colorEncoding,
          info.bitsPerColorChannel,
          info.activeColorMode
        };
      }

      BOOST_LOG(debug) << "Advanced color v2 query failed for Sunshine virtual display target " << output.TargetId
                       << " (error=" << result << "); falling back to legacy query.";

      DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO fallback {};
      fallback.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
      fallback.header.size = sizeof(fallback);
      fallback.header.adapterId = output.AdapterLuid;
      fallback.header.id = output.TargetId;
      const LONG fallback_result = DisplayConfigGetDeviceInfo(&fallback.header);
      if (fallback_result != ERROR_SUCCESS) {
        BOOST_LOG(debug) << "Advanced color query failed for Sunshine virtual display target " << output.TargetId
                         << " (error=" << fallback_result << ").";
        return std::nullopt;
      }
      return AdvancedColorInfo {
        fallback.advancedColorSupported != 0,
        fallback.advancedColorEnabled != 0,
        fallback.advancedColorForceDisabled != 0,
        false,
        fallback.advancedColorEnabled != 0,
        fallback.colorEncoding,
        fallback.bitsPerColorChannel,
        0
      };
    }

    bool set_hdr_state_inner(const DisplayConfigTarget &output, bool enabled) {
      DISPLAYCONFIG_SET_HDR_STATE_LOCAL state {};
      state.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_HDR_STATE;
      state.header.size = sizeof(state);
      state.header.adapterId = output.AdapterLuid;
      state.header.id = output.TargetId;
      state.enableHdr = enabled ? 1u : 0u;
      const LONG result = DisplayConfigSetDeviceInfo(&state.header);
      if (result != ERROR_SUCCESS) {
        BOOST_LOG(debug) << "HDR set failed for Sunshine virtual display target " << output.TargetId
                         << " enabled=" << enabled << " (error=" << result << ").";
        return false;
      }
      return true;
    }

    bool set_advanced_color_inner(const DisplayConfigTarget &output, bool enabled) {
      DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE state {};
      state.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
      state.header.size = sizeof(state);
      state.header.adapterId = output.AdapterLuid;
      state.header.id = output.TargetId;
      state.enableAdvancedColor = enabled ? 1u : 0u;
      const LONG result = DisplayConfigSetDeviceInfo(&state.header);
      if (result != ERROR_SUCCESS) {
        BOOST_LOG(debug) << "Advanced color set failed for Sunshine virtual display target " << output.TargetId
                         << " enabled=" << enabled << " (error=" << result << ").";
        return false;
      }
      return true;
    }

    std::optional<AdvancedColorInfo> query_advanced_color(const DisplayConfigTarget &output) {
      if (auto result = query_advanced_color_inner(output)) {
        return result;
      }

      HANDLE user_token = platf::retrieve_users_token(false);
      if (!user_token) {
        BOOST_LOG(debug) << "Advanced color query: unable to retrieve user token.";
        return std::nullopt;
      }

      std::optional<AdvancedColorInfo> result;
      const auto impersonation_ec = platf::impersonate_current_user(user_token, [&]() {
        result = query_advanced_color_inner(output);
      });

      CloseHandle(user_token);

      if (impersonation_ec) {
        BOOST_LOG(debug) << "Advanced color query: impersonation failed.";
      }

      return result;
    }

    bool set_advanced_color(const DisplayConfigTarget &output, bool enabled) {
      if (set_advanced_color_inner(output, enabled)) {
        return true;
      }

      HANDLE user_token = platf::retrieve_users_token(false);
      if (!user_token) {
        BOOST_LOG(debug) << "Advanced color set: unable to retrieve user token.";
        return false;
      }

      bool result = false;
      const auto impersonation_ec = platf::impersonate_current_user(user_token, [&]() {
        result = set_advanced_color_inner(output, enabled);
      });

      CloseHandle(user_token);

      if (impersonation_ec) {
        BOOST_LOG(debug) << "Advanced color set: impersonation failed.";
      }

      return result;
    }

    bool set_hdr_state(const DisplayConfigTarget &output, bool enabled) {
      if (set_hdr_state_inner(output, enabled)) {
        return true;
      }

      HANDLE user_token = platf::retrieve_users_token(false);
      if (!user_token) {
        BOOST_LOG(debug) << "HDR set: unable to retrieve user token.";
        return false;
      }

      bool result = false;
      const auto impersonation_ec = platf::impersonate_current_user(user_token, [&]() {
        result = set_hdr_state_inner(output, enabled);
      });

      CloseHandle(user_token);

      if (impersonation_ec) {
        BOOST_LOG(debug) << "HDR set: impersonation failed.";
      }

      return result;
    }

    bool request_hdr10_advanced_color(const DisplayConfigTarget &output, std::stop_token stop_token = {}) {
      if (stop_token.stop_requested()) {
        return false;
      }
      const bool hdr_state_set = set_hdr_state(output, true);
      if (!hdr_state_set) {
        BOOST_LOG(debug) << "Sunshine virtual display HDR: SET_HDR_STATE was not accepted for target " << output.TargetId
                         << "; trying Advanced Color state.";
      }

      const bool advanced_color_set = set_advanced_color(output, true);
      if (!advanced_color_set) {
        BOOST_LOG(debug) << "Sunshine virtual display HDR: SET_ADVANCED_COLOR_STATE was not accepted for target " << output.TargetId << ".";
      }

      if (!hdr_state_set && !advanced_color_set) {
        BOOST_LOG(warning) << "Sunshine virtual display HDR: failed to request HDR/Advanced Color for target " << output.TargetId << ".";
        return false;
      }
      if (stop_token.stop_requested()) {
        return false;
      }

      const auto deadline = std::chrono::steady_clock::now() + VDISPLAY::policy::hdr_activation_timeout;
      do {
        if (stop_token.stop_requested()) {
          return false;
        }
        if (auto info = query_advanced_color(output)) {
          BOOST_LOG(debug) << "Sunshine virtual display HDR: target=" << output.TargetId
                           << " supported=" << info->supported
                           << " active=" << info->active
                           << " limited_by_policy=" << info->limited_by_policy
                           << " hdr_supported=" << info->hdr_supported
                           << " hdr_enabled=" << info->hdr_enabled
                           << " active_color_mode=" << info->active_color_mode
                           << " color_encoding=" << static_cast<unsigned int>(info->color_encoding)
                           << " bits_per_color_channel=" << info->bits_per_color_channel;
          if (VDISPLAY::policy::hdr_target_ready({
                info->supported,
                info->hdr_supported,
                info->hdr_enabled,
                info->limited_by_policy,
                info->bits_per_color_channel,
              })) {
            return true;
          }
        }
        if (wait_for_monitor_stop(stop_token, std::chrono::milliseconds(50))) {
          return false;
        }
      } while (std::chrono::steady_clock::now() < deadline);

      BOOST_LOG(warning) << "Sunshine virtual display HDR: Windows did not report HDR support/enabled at 10-bit for target "
                         << output.TargetId << " after activation request.";
      return false;
    }

    bool reset_hdr_state_for_sdr(const DisplayConfigTarget &output, std::stop_token stop_token = {}) {
      if (stop_token.stop_requested()) {
        return false;
      }

      if (const auto info = query_advanced_color(output)) {
        BOOST_LOG(debug) << "Sunshine virtual display SDR: target=" << output.TargetId
                         << " supported=" << info->supported
                         << " active=" << info->active
                         << " limited_by_policy=" << info->limited_by_policy
                         << " hdr_supported=" << info->hdr_supported
                         << " hdr_enabled=" << info->hdr_enabled
                         << " active_color_mode=" << info->active_color_mode
                         << " color_encoding=" << static_cast<unsigned int>(info->color_encoding)
                         << " bits_per_color_channel=" << info->bits_per_color_channel;
        if (!VDISPLAY::policy::should_reset_hdr_state_for_stream(false, info->hdr_enabled)) {
          return true;
        }
      }

      const bool hdr_state_reset = set_hdr_state(output, false);
      bool advanced_color_reset = false;
      if (!hdr_state_reset) {
        BOOST_LOG(debug) << "Sunshine virtual display SDR: SET_HDR_STATE was not accepted for target " << output.TargetId
                         << "; trying Advanced Color state.";
        advanced_color_reset = set_advanced_color(output, false);
      }

      if (!hdr_state_reset && !advanced_color_reset) {
        BOOST_LOG(warning) << "Sunshine virtual display SDR: failed to reset HDR state for target " << output.TargetId
                           << "; deferring to the display helper.";
        return false;
      }
      if (stop_token.stop_requested()) {
        return false;
      }

      const auto deadline = std::chrono::steady_clock::now() + VDISPLAY::policy::hdr_activation_timeout;
      do {
        if (stop_token.stop_requested()) {
          return false;
        }
        if (const auto info = query_advanced_color(output)) {
          if (!info->hdr_enabled) {
            return true;
          }
        }
        if (wait_for_monitor_stop(stop_token, std::chrono::milliseconds(50))) {
          return false;
        }
      } while (std::chrono::steady_clock::now() < deadline);

      BOOST_LOG(warning) << "Sunshine virtual display SDR: Windows still reports HDR enabled for target "
                         << output.TargetId << " after the reset request; deferring to the display helper.";
      return false;
    }

    std::optional<DisplayConfigIdentity> query_display_config_identity_inner(const DisplayConfigTarget &output) {
      const UINT flags = QDC_VIRTUAL_MODE_AWARE | QDC_DATABASE_CURRENT;
      UINT path_count = 0;
      UINT mode_count = 0;
      if (GetDisplayConfigBufferSizes(flags, &path_count, &mode_count) != ERROR_SUCCESS) {
        return std::nullopt;
      }

      std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
      std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
      if (QueryDisplayConfig(flags, &path_count, path_count ? paths.data() : nullptr, &mode_count, mode_count ? modes.data() : nullptr, nullptr) != ERROR_SUCCESS) {
        return std::nullopt;
      }

      for (UINT i = 0; i < path_count; ++i) {
        const auto &path = paths[i];
        if (!luid_equals(path.targetInfo.adapterId, output.AdapterLuid) || path.targetInfo.id != output.TargetId) {
          continue;
        }

        DisplayConfigIdentity identity;

        DISPLAYCONFIG_SOURCE_DEVICE_NAME source_name {};
        source_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source_name.header.size = sizeof(source_name);
        source_name.header.adapterId = path.sourceInfo.adapterId;
        source_name.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source_name.header) == ERROR_SUCCESS && source_name.viewGdiDeviceName[0] != L'\0') {
          identity.source_gdi_device_name = std::wstring(source_name.viewGdiDeviceName);
        }

        DISPLAYCONFIG_TARGET_DEVICE_NAME target_name {};
        target_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target_name.header.size = sizeof(target_name);
        target_name.header.adapterId = path.targetInfo.adapterId;
        target_name.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target_name.header) == ERROR_SUCCESS) {
          if (target_name.monitorFriendlyDeviceName[0] != L'\0') {
            identity.monitor_friendly_device_name = std::wstring(target_name.monitorFriendlyDeviceName);
          }
          if (target_name.monitorDevicePath[0] != L'\0') {
            identity.monitor_device_path = std::wstring(target_name.monitorDevicePath);
          }
        }

        return identity;
      }

      return std::nullopt;
    }

    std::optional<DisplayConfigIdentity> query_display_config_identity(const DisplayConfigTarget &output) {
      // Try without impersonation first (works if already in user context)
      if (auto result = query_display_config_identity_inner(output)) {
        return result;
      }

      // QueryDisplayConfig requires user session context when running as SYSTEM
      HANDLE user_token = platf::retrieve_users_token(false);
      if (!user_token) {
        BOOST_LOG(debug) << "query_display_config_identity: unable to retrieve user token";
        return std::nullopt;
      }

      std::optional<DisplayConfigIdentity> result;
      const auto impersonation_ec = platf::impersonate_current_user(user_token, [&]() {
        result = query_display_config_identity_inner(output);
      });

      CloseHandle(user_token);

      if (impersonation_ec) {
        BOOST_LOG(debug) << "query_display_config_identity: impersonation failed";
      }

      return result;
    }

    bool display_config_identity_has_display_name(const DisplayConfigIdentity &identity) {
      return (identity.source_gdi_device_name && !identity.source_gdi_device_name->empty()) ||
             (identity.monitor_device_path && !identity.monitor_device_path->empty());
    }

    std::optional<DisplayConfigIdentity> wait_for_display_config_identity(
      const DisplayConfigTarget &output,
      std::chrono::steady_clock::duration timeout = std::chrono::milliseconds(250),
      std::stop_token stop_token = {}
    ) {
      if (stop_token.stop_requested()) {
        return std::nullopt;
      }
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      do {
        if (stop_token.stop_requested()) {
          return std::nullopt;
        }
        if (auto identity = query_display_config_identity(output)) {
          if (display_config_identity_has_display_name(*identity)) {
            return identity;
          }
        }
        if (wait_for_monitor_stop(stop_token, std::chrono::milliseconds(50))) {
          return std::nullopt;
        }
      } while (std::chrono::steady_clock::now() < deadline);

      if (stop_token.stop_requested()) {
        return std::nullopt;
      }
      return query_display_config_identity(output);
    }

    std::optional<std::wstring> display_name_from_identity(const DisplayConfigIdentity &identity) {
      if (identity.source_gdi_device_name && !identity.source_gdi_device_name->empty()) {
        return identity.source_gdi_device_name;
      }
      if (identity.monitor_device_path && !identity.monitor_device_path->empty()) {
        return identity.monitor_device_path;
      }
      return std::nullopt;
    }

    std::array<char, TEMPORARY_DISPLAY_NAME_CHARS> make_temporary_display_name(const char *client_name) {
      std::array<char, TEMPORARY_DISPLAY_NAME_CHARS> name {};
      const char *fallback = "Vibepollo";
      const char *source = (client_name && std::strlen(client_name) > 0) ? client_name : fallback;
      std::size_t out = 0;

      for (std::size_t in = 0; source[in] != '\0' && out + 1 < name.size(); ++in) {
        const auto ch = static_cast<unsigned char>(source[in]);
        if (ch < 0x20 || ch > 0x7e) {
          continue;
        }

        name[out++] = static_cast<char>(ch);
      }

      while (out > 0 && name[out - 1] == ' ') {
        name[--out] = '\0';
      }

      if (out == 0) {
        std::memcpy(name.data(), fallback, std::min<std::size_t>(std::strlen(fallback), name.size() - 1));
      }

      return name;
    }

    std::optional<std::wstring> resolve_monitor_device_path_once(
      const std::optional<std::wstring> &display_name,
      const std::optional<std::string> &device_id,
      const std::optional<std::string> &client_name = std::nullopt
    ) {
      std::optional<std::string> normalized_target;
      if (display_name && !display_name->empty()) {
        normalized_target = normalize_display_name(platf::to_utf8(*display_name));
      }
      std::optional<std::string> normalized_device_id;
      if (device_id && !device_id->empty()) {
        normalized_device_id = normalize_display_name(*device_id);
      }
      std::optional<std::string> normalized_client_name;
      if (client_name && !client_name->empty()) {
        normalized_client_name = normalize_display_name(*client_name);
      }
      const bool has_any_criteria = normalized_target || normalized_device_id || normalized_client_name;

      // Use QDC_ALL_PATHS to include virtual displays that may not be "active" yet
      UINT path_count = 0;
      UINT mode_count = 0;
      UINT flags = QDC_ALL_PATHS;

      LONG buffer_result = GetDisplayConfigBufferSizes(flags, &path_count, &mode_count);
      if (buffer_result != ERROR_SUCCESS) {
        // Fallback to QDC_ONLY_ACTIVE_PATHS
        flags = QDC_ONLY_ACTIVE_PATHS;
        buffer_result = GetDisplayConfigBufferSizes(flags, &path_count, &mode_count);
      }
      if (buffer_result != ERROR_SUCCESS) {
        return std::nullopt;
      }

      std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
      std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
      LONG qdc_result = QueryDisplayConfig(flags, &path_count, path_count ? paths.data() : nullptr, &mode_count, mode_count ? modes.data() : nullptr, nullptr);
      if (qdc_result != ERROR_SUCCESS) {
        return std::nullopt;
      }

      // If no identifiers are provided (e.g., physical output_name unset), default to the primary display.
      if (!has_any_criteria) {
        const auto read_monitor_path = [&](const DISPLAYCONFIG_PATH_INFO &path) -> std::optional<std::wstring> {
          DISPLAYCONFIG_TARGET_DEVICE_NAME target_name {};
          target_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
          target_name.header.size = sizeof(target_name);
          target_name.header.adapterId = path.targetInfo.adapterId;
          target_name.header.id = path.targetInfo.id;
          if (DisplayConfigGetDeviceInfo(&target_name.header) != ERROR_SUCCESS) {
            return std::nullopt;
          }
          if (target_name.monitorDevicePath[0] == L'\0') {
            return std::nullopt;
          }
          return std::wstring(target_name.monitorDevicePath);
        };

        const auto is_primary_path = [&](const DISPLAYCONFIG_PATH_INFO &path) -> bool {
          if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0) {
            return false;
          }
          const auto source_idx = path.sourceInfo.modeInfoIdx;
          if (source_idx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || source_idx >= mode_count) {
            return false;
          }
          const auto &mode = modes[source_idx];
          if (mode.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            return false;
          }
          return mode.sourceMode.position.x == 0 && mode.sourceMode.position.y == 0;
        };

        for (UINT i = 0; i < path_count; ++i) {
          const auto &path = paths[i];
          if (!is_primary_path(path)) {
            continue;
          }
          if (auto found = read_monitor_path(path)) {
            return found;
          }
        }

        for (UINT i = 0; i < path_count; ++i) {
          const auto &path = paths[i];
          if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0) {
            continue;
          }
          if (auto found = read_monitor_path(path)) {
            return found;
          }
        }

        for (UINT i = 0; i < path_count; ++i) {
          if (auto found = read_monitor_path(paths[i])) {
            return found;
          }
        }

        return std::nullopt;
      }

      for (UINT i = 0; i < path_count; ++i) {
        const auto &path = paths[i];

        DISPLAYCONFIG_TARGET_DEVICE_NAME target_name {};
        target_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target_name.header.size = sizeof(target_name);
        target_name.header.adapterId = path.targetInfo.adapterId;
        target_name.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target_name.header) != ERROR_SUCCESS) {
          continue;
        }

        if (target_name.monitorDevicePath[0] == L'\0') {
          continue;
        }

        std::optional<std::string> target_friendly;
        if (target_name.monitorFriendlyDeviceName[0] != L'\0') {
          target_friendly = normalize_display_name(platf::to_utf8(std::wstring(target_name.monitorFriendlyDeviceName)));
        }

        // Match by client name against monitor friendly name (virtual display uses client name as friendly name)
        if (target_friendly && normalized_client_name && *target_friendly == *normalized_client_name) {
          return std::wstring(target_name.monitorDevicePath);
        }

        const bool target_match =
          (target_friendly && normalized_target && *target_friendly == *normalized_target) ||
          (target_friendly && normalized_device_id && *target_friendly == *normalized_device_id);
        if (target_match) {
          return std::wstring(target_name.monitorDevicePath);
        }

        DISPLAYCONFIG_SOURCE_DEVICE_NAME source_name {};
        source_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source_name.header.size = sizeof(source_name);
        source_name.header.adapterId = path.sourceInfo.adapterId;
        source_name.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source_name.header) != ERROR_SUCCESS) {
          continue;
        }

        std::optional<std::string> source_view;
        if (source_name.viewGdiDeviceName[0] != L'\0') {
          source_view = normalize_display_name(platf::to_utf8(std::wstring(source_name.viewGdiDeviceName)));
        }
        const bool source_match =
          (source_view && normalized_target && *source_view == *normalized_target) ||
          (source_view && normalized_device_id && *source_view == *normalized_device_id);
        if (source_match) {
          return std::wstring(target_name.monitorDevicePath);
        }
      }

      return std::nullopt;
    }

    std::optional<std::wstring> resolve_monitor_device_path(
      const std::optional<std::wstring> &display_name,
      const std::optional<std::string> &device_id,
      int attempts,
      std::chrono::milliseconds delay,
      const std::optional<std::string> &client_name,
      std::stop_token stop_token
    ) {
      const auto wait_or_stop = [&]() {
        if (!stop_token.stop_possible()) {
          std::this_thread::sleep_for(delay);
          return false;
        }

        const auto deadline = std::chrono::steady_clock::now() + delay;
        while (!stop_token.stop_requested()) {
          const auto now = std::chrono::steady_clock::now();
          if (now >= deadline) {
            return false;
          }
          const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
          std::this_thread::sleep_for(std::min(std::max(remaining, std::chrono::milliseconds(1)), std::chrono::milliseconds(50)));
        }
        return true;
      };

      // Try without impersonation first (faster if already in user context)
      for (int i = 0; i < attempts; ++i) {
        if (stop_token.stop_requested()) {
          return std::nullopt;
        }
        if (auto path = resolve_monitor_device_path_once(display_name, device_id, client_name)) {
          return path;
        }
        if (i + 1 < attempts && wait_or_stop()) {
          return std::nullopt;
        }
      }

      // Fall back to impersonation if direct access failed
      HANDLE user_token = platf::retrieve_users_token(false);
      if (!user_token) {
        return std::nullopt;
      }

      std::optional<std::wstring> result;
      (void) platf::impersonate_current_user(user_token, [&]() {
        for (int i = 0; i < attempts; ++i) {
          if (stop_token.stop_requested()) {
            return;
          }
          if (auto path = resolve_monitor_device_path_once(display_name, device_id, client_name)) {
            result = path;
            return;
          }
          if (i + 1 < attempts && wait_or_stop()) {
            return;
          }
        }
      });

      CloseHandle(user_token);
      return result;
    }

    std::optional<std::wstring> resolve_virtual_display_name_from_devices() {
      auto devices = platf::display_helper::Coordinator::instance().enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
      if (!devices) {
        return std::nullopt;
      }
      for (const auto &device : *devices) {
        if (!is_virtual_display_device(device)) {
          continue;
        }
        if (!device.m_display_name.empty()) {
          return platf::from_utf8(device.m_display_name);
        }
        if (!device.m_device_id.empty()) {
          return platf::from_utf8(device.m_device_id);
        }
      }
      return std::nullopt;
    }

    std::optional<std::wstring> resolve_virtual_display_name_from_devices_for_client(const char *client_name) {
      if (!client_name || std::strlen(client_name) == 0) {
        return std::nullopt;
      }

      auto devices = platf::display_helper::Coordinator::instance().enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
      if (!devices) {
        return std::nullopt;
      }

      std::optional<std::wstring> fallback;
      for (const auto &device : *devices) {
        if (!is_virtual_display_device(device)) {
          continue;
        }
        if (device.m_friendly_name.empty() || !equals_ci(device.m_friendly_name, client_name)) {
          continue;
        }

        if (device.m_info) {
          if (!device.m_display_name.empty()) {
            return platf::from_utf8(device.m_display_name);
          }
          if (!device.m_device_id.empty()) {
            return platf::from_utf8(device.m_device_id);
          }
        }

        if (!fallback) {
          if (!device.m_display_name.empty()) {
            fallback = platf::from_utf8(device.m_display_name);
          } else if (!device.m_device_id.empty()) {
            fallback = platf::from_utf8(device.m_device_id);
          }
        }
      }

      return fallback;
    }

    std::optional<uuid_util::uuid_t> parse_uuid_string(const std::string &value) {
      if (value.empty()) {
        return std::nullopt;
      }
      try {
        return uuid_util::uuid_t::parse(value);
      } catch (...) {
        return std::nullopt;
      }
    }

    std::optional<uuid_util::uuid_t> parse_raw_uuid_string(const std::string &value) {
      if (value.empty()) {
        return std::nullopt;
      }
      try {
        return uuid_util::uuid_t::parse_raw(value);
      } catch (...) {
        return std::nullopt;
      }
    }

    enum class VirtualDisplayRecoveryPhase {
      prepared,
      active,
    };

    struct VirtualDisplayRecoveryEntry {
      uuid_util::uuid_t guid {};
      std::uint64_t display_id {};
      sunshine_driver::OwnerCapability owner_capability {};
      VirtualDisplayRecoveryPhase phase {VirtualDisplayRecoveryPhase::prepared};
    };

    enum class VirtualDisplayRecoveryLoadResult {
      missing,
      loaded,
      failed,
    };

    constexpr std::size_t MAX_VIRTUAL_DISPLAY_RECOVERY_ENTRIES = sunshine_driver::kMaxDisplayStateEntries;
    constexpr std::uint64_t MAX_VIRTUAL_DISPLAY_RECOVERY_FILE_BYTES = 128 * 1024;
    constexpr char VIRTUAL_DISPLAY_RECOVERY_DPAPI_ENTROPY[] = "Vibepollo virtual display owner capability v1";

    fs::path virtual_display_private_state_directory() {
      const auto &state_path_string = statefile::vibeshine_state_path();
      if (state_path_string.empty()) {
        return {};
      }
      const fs::path state_path(state_path_string);
      if (state_path.parent_path().empty()) {
        return {};
      }
      return state_path.parent_path() / "credentials";
    }

    bool ensure_virtual_display_private_state_directory(fs::path &directory) {
      directory = virtual_display_private_state_directory();
      if (directory.empty()) {
        BOOST_LOG(error) << "Virtual display recovery state has no private config directory.";
        return false;
      }

      std::error_code ec;
      fs::create_directories(directory, ec);
      if (ec || !fs::is_directory(directory, ec) || ec) {
        BOOST_LOG(error) << "Unable to create private virtual display recovery directory '"
                         << directory.string() << "': " << ec.message();
        return false;
      }

      if (!statefile::secure_private_directory(directory.string())) {
        BOOST_LOG(error) << "Private virtual display recovery ACL verification failed for '"
                         << directory.string() << "'.";
        return false;
      }
      return true;
    }

    fs::path virtual_display_recovery_journal_path() {
      fs::path directory;
      if (!ensure_virtual_display_private_state_directory(directory)) {
        return {};
      }
      return directory / "virtual_display_recovery.dat";
    }

    class VirtualDisplayProcessLock {
    public:
      ~VirtualDisplayProcessLock() {
        if (handle_ != INVALID_HANDLE_VALUE) {
          OVERLAPPED overlapped {};
          (void) UnlockFileEx(handle_, 0, 1, 0, &overlapped);
          CloseHandle(handle_);
        }
      }

      bool acquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (handle_ != INVALID_HANDLE_VALUE) {
          return true;
        }

        fs::path directory;
        if (!ensure_virtual_display_private_state_directory(directory)) {
          return false;
        }
        const auto lock_path = (directory / "virtual_display_recovery.lock").wstring();
        HANDLE handle = CreateFileW(
          lock_path.c_str(),
          GENERIC_READ | GENERIC_WRITE,
          FILE_SHARE_READ | FILE_SHARE_WRITE,
          nullptr,
          OPEN_ALWAYS,
          FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
          nullptr
        );
        if (handle == INVALID_HANDLE_VALUE) {
          BOOST_LOG(error) << "Unable to open virtual display ownership lock (error=" << GetLastError() << ").";
          return false;
        }

        OVERLAPPED overlapped {};
        if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped)) {
          const auto native_error = GetLastError();
          CloseHandle(handle);
          BOOST_LOG(warning) << "Another Vibepollo process owns virtual display recovery state (error="
                             << native_error << ").";
          return false;
        }

        handle_ = handle;
        return true;
      }

    private:
      std::mutex mutex_;
      HANDLE handle_ {INVALID_HANDLE_VALUE};
    };

    VirtualDisplayProcessLock &virtual_display_process_lock() {
      static VirtualDisplayProcessLock lock;
      return lock;
    }

    bool ensure_virtual_display_process_ownership() {
      return virtual_display_process_lock().acquire();
    }

    std::optional<sunshine_driver::OwnerCapability> generate_owner_capability() {
      sunshine_driver::OwnerCapability capability {};
      if (RAND_bytes(capability.bytes.data(), static_cast<int>(capability.bytes.size())) != 1) {
        BOOST_LOG(error) << "Unable to generate a cryptographic virtual display owner capability.";
        return std::nullopt;
      }
      return capability;
    }

    char hex_digit(const std::uint8_t nibble) {
      return nibble < 10 ? static_cast<char>('0' + nibble) : static_cast<char>('a' + nibble - 10);
    }

    std::string owner_capability_to_hex(const sunshine_driver::OwnerCapability &capability) {
      std::string encoded(capability.bytes.size() * 2, '0');
      for (std::size_t index = 0; index < capability.bytes.size(); ++index) {
        encoded[index * 2] = hex_digit(static_cast<std::uint8_t>(capability.bytes[index] >> 4));
        encoded[index * 2 + 1] = hex_digit(static_cast<std::uint8_t>(capability.bytes[index] & 0x0f));
      }
      return encoded;
    }

    std::optional<std::uint8_t> parse_hex_digit(const char digit) {
      if (digit >= '0' && digit <= '9') {
        return static_cast<std::uint8_t>(digit - '0');
      }
      if (digit >= 'a' && digit <= 'f') {
        return static_cast<std::uint8_t>(digit - 'a' + 10);
      }
      if (digit >= 'A' && digit <= 'F') {
        return static_cast<std::uint8_t>(digit - 'A' + 10);
      }
      return std::nullopt;
    }

    std::optional<sunshine_driver::OwnerCapability> owner_capability_from_hex(const std::string &encoded) {
      sunshine_driver::OwnerCapability capability {};
      if (encoded.size() != capability.bytes.size() * 2) {
        return std::nullopt;
      }
      bool any_nonzero = false;
      for (std::size_t index = 0; index < capability.bytes.size(); ++index) {
        const auto high = parse_hex_digit(encoded[index * 2]);
        const auto low = parse_hex_digit(encoded[index * 2 + 1]);
        if (!high || !low) {
          return std::nullopt;
        }
        capability.bytes[index] = static_cast<std::uint8_t>((*high << 4) | *low);
        any_nonzero = any_nonzero || capability.bytes[index] != 0;
      }
      return any_nonzero ? std::optional<sunshine_driver::OwnerCapability> {capability} : std::nullopt;
    }

    std::optional<std::uint64_t> parse_uint64_decimal(const std::string &value) {
      std::uint64_t parsed = 0;
      const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed, 10);
      if (result.ec != std::errc {} || result.ptr != value.data() + value.size()) {
        return std::nullopt;
      }
      return parsed;
    }

    DATA_BLOB virtual_display_recovery_entropy_blob() {
      DATA_BLOB entropy {};
      entropy.cbData = static_cast<DWORD>(sizeof(VIRTUAL_DISPLAY_RECOVERY_DPAPI_ENTROPY) - 1);
      entropy.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(VIRTUAL_DISPLAY_RECOVERY_DPAPI_ENTROPY));
      return entropy;
    }

    bool protect_virtual_display_recovery_data(
      const std::string &plaintext,
      std::vector<std::uint8_t> &ciphertext
    ) {
      if (plaintext.size() > (std::numeric_limits<DWORD>::max)()) {
        return false;
      }
      DATA_BLOB input {};
      input.cbData = static_cast<DWORD>(plaintext.size());
      input.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(plaintext.data()));
      auto entropy = virtual_display_recovery_entropy_blob();
      DATA_BLOB output {};
      if (!CryptProtectData(
            &input,
            L"Vibepollo virtual display recovery",
            &entropy,
            nullptr,
            nullptr,
            CRYPTPROTECT_UI_FORBIDDEN,
            &output
          )) {
        BOOST_LOG(error) << "DPAPI failed to protect virtual display recovery state (error="
                         << GetLastError() << ").";
        return false;
      }

      ciphertext.assign(output.pbData, output.pbData + output.cbData);
      SecureZeroMemory(output.pbData, output.cbData);
      LocalFree(output.pbData);
      return true;
    }

    bool unprotect_virtual_display_recovery_data(
      const std::vector<std::uint8_t> &ciphertext,
      std::vector<std::uint8_t> &plaintext
    ) {
      if (ciphertext.empty() || ciphertext.size() > (std::numeric_limits<DWORD>::max)()) {
        return false;
      }
      DATA_BLOB input {};
      input.cbData = static_cast<DWORD>(ciphertext.size());
      input.pbData = const_cast<BYTE *>(ciphertext.data());
      auto entropy = virtual_display_recovery_entropy_blob();
      DATA_BLOB output {};
      LPWSTR description = nullptr;
      if (!CryptUnprotectData(
            &input,
            &description,
            &entropy,
            nullptr,
            nullptr,
            CRYPTPROTECT_UI_FORBIDDEN,
            &output
          )) {
        BOOST_LOG(error) << "DPAPI failed to unprotect virtual display recovery state (error="
                         << GetLastError() << ").";
        return false;
      }

      if (description) {
        LocalFree(description);
      }
      plaintext.assign(output.pbData, output.pbData + output.cbData);
      SecureZeroMemory(output.pbData, output.cbData);
      LocalFree(output.pbData);
      return true;
    }

    bool write_virtual_display_recovery_blob_atomic(
      const fs::path &target,
      const std::vector<std::uint8_t> &contents
    ) {
      static std::atomic<unsigned> sequence {0};
      fs::path temporary = target;
      temporary += ".tmp." + std::to_string(GetCurrentProcessId()) + "." +
                   std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
      const auto temporary_wide = temporary.wstring();
      HANDLE file = CreateFileW(
        temporary_wide.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_FLAG_WRITE_THROUGH,
        nullptr
      );
      if (file == INVALID_HANDLE_VALUE) {
        BOOST_LOG(error) << "Unable to create temporary virtual display recovery file (error="
                         << GetLastError() << ").";
        return false;
      }

      DWORD bytes_written = 0;
      const bool wrote = contents.size() <= (std::numeric_limits<DWORD>::max)() &&
                         WriteFile(
                           file,
                           contents.data(),
                           static_cast<DWORD>(contents.size()),
                           &bytes_written,
                           nullptr
                         ) &&
                         bytes_written == contents.size() &&
                         FlushFileBuffers(file);
      const auto write_error = wrote ? ERROR_SUCCESS : GetLastError();
      CloseHandle(file);
      if (!wrote) {
        DeleteFileW(temporary_wide.c_str());
        BOOST_LOG(error) << "Unable to durably write virtual display recovery state (error="
                         << write_error << ").";
        return false;
      }

      const auto target_wide = target.wstring();
      if (!MoveFileExW(
            temporary_wide.c_str(),
            target_wide.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
          )) {
        const auto move_error = GetLastError();
        DeleteFileW(temporary_wide.c_str());
        BOOST_LOG(error) << "Unable to atomically replace virtual display recovery state (error="
                         << move_error << ").";
        return false;
      }
      return true;
    }

    bool save_virtual_display_recovery_journal(const std::vector<VirtualDisplayRecoveryEntry> &entries) {
      if (entries.size() > MAX_VIRTUAL_DISPLAY_RECOVERY_ENTRIES) {
        BOOST_LOG(error) << "Refusing to persist too many virtual display recovery entries.";
        return false;
      }
      const auto path = virtual_display_recovery_journal_path();
      if (path.empty()) {
        return false;
      }

      nlohmann::json document;
      document["version"] = 1;
      document["entries"] = nlohmann::json::array();
      for (const auto &entry : entries) {
        document["entries"].push_back(
          {
            {"guid", entry.guid.string()},
            {"display_id", std::to_string(entry.display_id)},
            {"owner_capability", owner_capability_to_hex(entry.owner_capability)},
            {"phase", entry.phase == VirtualDisplayRecoveryPhase::active ? "active" : "prepared"},
          }
        );
      }

      std::string plaintext = document.dump();
      std::vector<std::uint8_t> ciphertext;
      const bool protected_ok = protect_virtual_display_recovery_data(plaintext, ciphertext);
      if (!plaintext.empty()) {
        SecureZeroMemory(plaintext.data(), plaintext.size());
      }
      if (!protected_ok) {
        return false;
      }
      return write_virtual_display_recovery_blob_atomic(path, ciphertext);
    }

    VirtualDisplayRecoveryLoadResult load_virtual_display_recovery_journal(
      std::vector<VirtualDisplayRecoveryEntry> &entries
    ) {
      entries.clear();
      const auto path = virtual_display_recovery_journal_path();
      if (path.empty()) {
        return VirtualDisplayRecoveryLoadResult::failed;
      }

      const auto path_wide = path.wstring();
      HANDLE file = CreateFileW(
        path_wide.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
      );
      if (file == INVALID_HANDLE_VALUE) {
        const auto native_error = GetLastError();
        if (native_error == ERROR_FILE_NOT_FOUND || native_error == ERROR_PATH_NOT_FOUND) {
          return VirtualDisplayRecoveryLoadResult::missing;
        }
        BOOST_LOG(error) << "Unable to open virtual display recovery state (error=" << native_error << ").";
        return VirtualDisplayRecoveryLoadResult::failed;
      }

      LARGE_INTEGER size {};
      if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
          static_cast<std::uint64_t>(size.QuadPart) > MAX_VIRTUAL_DISPLAY_RECOVERY_FILE_BYTES) {
        const auto native_error = GetLastError();
        CloseHandle(file);
        BOOST_LOG(error) << "Virtual display recovery state has an invalid size (error=" << native_error << ").";
        return VirtualDisplayRecoveryLoadResult::failed;
      }

      std::vector<std::uint8_t> ciphertext(static_cast<std::size_t>(size.QuadPart));
      DWORD bytes_read = 0;
      const bool read_ok = ReadFile(
                             file,
                             ciphertext.data(),
                             static_cast<DWORD>(ciphertext.size()),
                             &bytes_read,
                             nullptr
                           ) &&
                           bytes_read == ciphertext.size();
      const auto read_error = read_ok ? ERROR_SUCCESS : GetLastError();
      CloseHandle(file);
      if (!read_ok) {
        BOOST_LOG(error) << "Unable to read virtual display recovery state (error=" << read_error << ").";
        return VirtualDisplayRecoveryLoadResult::failed;
      }

      std::vector<std::uint8_t> plaintext;
      if (!unprotect_virtual_display_recovery_data(ciphertext, plaintext)) {
        return VirtualDisplayRecoveryLoadResult::failed;
      }
      auto document = nlohmann::json::parse(plaintext.begin(), plaintext.end(), nullptr, false);
      if (!plaintext.empty()) {
        SecureZeroMemory(plaintext.data(), plaintext.size());
      }
      if (document.is_discarded() || !document.is_object() ||
          !document.contains("version") || !document["version"].is_number_integer() ||
          document["version"] != 1 ||
          !document.contains("entries") || !document["entries"].is_array() ||
          document["entries"].size() > MAX_VIRTUAL_DISPLAY_RECOVERY_ENTRIES) {
        BOOST_LOG(error) << "Virtual display recovery state is malformed.";
        return VirtualDisplayRecoveryLoadResult::failed;
      }

      std::unordered_map<std::string, bool> seen_guids;
      std::unordered_map<std::uint64_t, bool> seen_display_ids;
      for (const auto &serialized : document["entries"]) {
        if (!serialized.is_object() ||
            !serialized.contains("guid") || !serialized["guid"].is_string() ||
            !serialized.contains("display_id") || !serialized["display_id"].is_string() ||
            !serialized.contains("owner_capability") || !serialized["owner_capability"].is_string() ||
            !serialized.contains("phase") || !serialized["phase"].is_string()) {
          BOOST_LOG(error) << "Virtual display recovery entry is malformed.";
          return VirtualDisplayRecoveryLoadResult::failed;
        }

        const auto guid_string = serialized["guid"].get<std::string>();
        // The journal stores uuid_t::string(), which preserves the GUID's raw
        // memory bytes rather than the canonical GUID field order accepted by
        // parse_uuid_string(). Keep that exact byte sequence so the persisted
        // display_id continues to validate after restart.
        const auto guid = parse_raw_uuid_string(guid_string);
        const auto display_id = parse_uint64_decimal(serialized["display_id"].get<std::string>());
        const auto capability = owner_capability_from_hex(serialized["owner_capability"].get<std::string>());
        const auto phase_string = serialized["phase"].get<std::string>();
        if (!guid || !display_id || *display_id == 0 || !capability ||
            (phase_string != "prepared" && phase_string != "active") ||
            *display_id != client_uuid_to_virtual_display_id(uuid_to_guid(*guid)) ||
            seen_guids.contains(guid_string) || seen_display_ids.contains(*display_id)) {
          BOOST_LOG(error) << "Virtual display recovery entry failed validation.";
          return VirtualDisplayRecoveryLoadResult::failed;
        }

        seen_guids.emplace(guid_string, true);
        seen_display_ids.emplace(*display_id, true);
        entries.push_back(
          VirtualDisplayRecoveryEntry {
            *guid,
            *display_id,
            *capability,
            phase_string == "active" ? VirtualDisplayRecoveryPhase::active : VirtualDisplayRecoveryPhase::prepared
          }
        );
      }
      return VirtualDisplayRecoveryLoadResult::loaded;
    }

    std::optional<bool> driver_has_temporary_display(
      sunshine_driver::ControlClient &client,
      const std::optional<std::uint64_t> display_id = std::nullopt
    ) {
      const auto state = client.query_display_state();
      if (!state.ok()) {
        log_control_failure("Sunshine virtual display state query", state.status, state.native_error);
        return std::nullopt;
      }
      for (std::uint32_t index = 0;
           index < state.value.entry_count && index < sunshine_driver::kMaxDisplayStateEntries;
           ++index) {
        const auto &entry = state.value.entries[index];
        if (entry.kind == sunshine_driver::kDisplayStateKindTemporary &&
            (!display_id || entry.display_id == *display_id)) {
          return true;
        }
      }
      return false;
    }

    bool load_virtual_display_recovery_journal_for_driver(
      sunshine_driver::ControlClient &client,
      std::vector<VirtualDisplayRecoveryEntry> &entries
    ) {
      const auto loaded = load_virtual_display_recovery_journal(entries);
      if (loaded == VirtualDisplayRecoveryLoadResult::missing ||
          loaded == VirtualDisplayRecoveryLoadResult::loaded) {
        return true;
      }

      const auto any_driver_display = driver_has_temporary_display(client);
      if (!any_driver_display || *any_driver_display) {
        BOOST_LOG(error) << "Refusing to replace unreadable virtual display recovery state while a driver display may exist.";
        return false;
      }

      entries.clear();
      BOOST_LOG(warning) << "Resetting unreadable virtual display recovery state because the driver has no temporary displays.";
      return save_virtual_display_recovery_journal(entries);
    }

    auto find_virtual_display_recovery_entry(
      std::vector<VirtualDisplayRecoveryEntry> &entries,
      const uuid_util::uuid_t &guid
    ) {
      return std::find_if(entries.begin(), entries.end(), [&](const auto &entry) {
        return entry.guid == guid;
      });
    }

    bool upsert_virtual_display_recovery_entry(
      std::vector<VirtualDisplayRecoveryEntry> &entries,
      VirtualDisplayRecoveryEntry replacement
    ) {
      entries.erase(
        std::remove_if(entries.begin(), entries.end(), [&](const auto &entry) {
          return entry.guid == replacement.guid || entry.display_id == replacement.display_id;
        }),
        entries.end()
      );
      entries.push_back(std::move(replacement));
      return save_virtual_display_recovery_journal(entries);
    }

    bool erase_virtual_display_recovery_entry(
      std::vector<VirtualDisplayRecoveryEntry> &entries,
      const uuid_util::uuid_t &guid
    ) {
      const auto old_size = entries.size();
      entries.erase(
        std::remove_if(entries.begin(), entries.end(), [&](const auto &entry) {
          return entry.guid == guid;
        }),
        entries.end()
      );
      return entries.size() == old_size || save_virtual_display_recovery_journal(entries);
    }

    bool clear_virtual_display_recovery_entry(const uuid_util::uuid_t &guid) {
      std::vector<VirtualDisplayRecoveryEntry> entries;
      const auto loaded = load_virtual_display_recovery_journal(entries);
      if (loaded == VirtualDisplayRecoveryLoadResult::missing) {
        return true;
      }
      if (loaded != VirtualDisplayRecoveryLoadResult::loaded) {
        return false;
      }
      return erase_virtual_display_recovery_entry(entries, guid);
    }

    std::optional<bool> driver_supports_secure_reclaim(sunshine_driver::ControlClient &client) {
      const auto version = client.query_protocol_version();
      if (!version.ok()) {
        log_control_failure("Sunshine virtual display protocol query", version.status, version.native_error);
        return std::nullopt;
      }
      return version.value.major == sunshine_driver::kProtocolVersionMajor &&
             version.value.minor >= SECURE_RECLAIM_DRIVER_PROTOCOL_MINOR;
    }

    enum class VirtualDisplayReclaimResult {
      reclaimed,
      absent,
      failed,
    };

    VirtualDisplayReclaimResult reclaim_virtual_display_recovery_entry(
      sunshine_driver::ControlClient &client,
      std::vector<VirtualDisplayRecoveryEntry> &entries,
      const uuid_util::uuid_t &guid,
      const std::optional<std::wstring> &display_name,
      const std::optional<std::string> &device_id,
      const std::optional<std::wstring> &monitor_device_path
    ) {
      const auto recovery = find_virtual_display_recovery_entry(entries, guid);
      if (recovery == entries.end()) {
        return VirtualDisplayReclaimResult::absent;
      }

      sunshine_driver::ReclaimTemporaryDisplayRequest request {};
      request.display_id = recovery->display_id;
      const auto new_lease_id = generate_driver_lease_id();
      if (!new_lease_id) {
        return VirtualDisplayReclaimResult::failed;
      }
      request.new_lease_id = *new_lease_id;
      request.requested_timeout_ms = DRIVER_LEASE_TIMEOUT_MS;
      request.owner_capability = recovery->owner_capability;
      const auto reclaimed = client.reclaim_temporary_display(request);
      if (!reclaimed.ok()) {
        const auto display_exists = driver_has_temporary_display(client, recovery->display_id);
        if (display_exists && !*display_exists) {
          const auto stale_guid = recovery->guid;
          if (!erase_virtual_display_recovery_entry(entries, stale_guid)) {
            return VirtualDisplayReclaimResult::failed;
          }
          return VirtualDisplayReclaimResult::absent;
        }
        log_control_failure("Sunshine virtual display secure reclaim", reclaimed.status, reclaimed.native_error);
        BOOST_LOG(warning) << "Secure reclaim failed closed for guid=" << guid.string()
                           << " while its driver display may still exist.";
        return VirtualDisplayReclaimResult::failed;
      }

      driver_lease_tracker().put(
        guid,
        DriverLeaseInfo {
          reclaimed.value.display_id,
          reclaimed.value.lease_id,
          display_name,
          device_id,
          monitor_device_path
        }
      );
      recovery->phase = VirtualDisplayRecoveryPhase::active;
      if (!save_virtual_display_recovery_journal(entries)) {
        BOOST_LOG(warning) << "Secure reclaim succeeded but the prepared recovery marker could not be promoted for guid="
                           << guid.string() << ".";
      }
      (void) ensure_watchdog_thread_active_for_lease();
      BOOST_LOG(info) << "Securely reclaimed existing Sunshine virtual display for guid=" << guid.string()
                      << " display_id=" << reclaimed.value.display_id << '.';
      return VirtualDisplayReclaimResult::reclaimed;
    }

    std::optional<uuid_util::uuid_t> load_guid_from_state_locked() {
      statefile::migrate_recent_state_keys();
      const auto &path_str = statefile::vibeshine_state_path();
      if (path_str.empty()) {
        return std::nullopt;
      }

      std::lock_guard<std::mutex> lock(statefile::state_mutex());
      const fs::path path(path_str);
      if (!fs::exists(path)) {
        return std::nullopt;
      }

      try {
        pt::ptree tree;
        pt::read_json(path.string(), tree);
        if (auto guid_str = tree.get_optional<std::string>("root.virtual_display_guid")) {
          if (auto parsed = parse_uuid_string(*guid_str)) {
            return parsed;
          }
        }
      } catch (...) {
      }
      return std::nullopt;
    }

    std::optional<uuid_util::uuid_t> load_guid_from_legacy_cache_locked() {
      const auto path = legacy_virtual_display_cache_path();
      if (!fs::exists(path)) {
        return std::nullopt;
      }

      try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
          return std::nullopt;
        }
        nlohmann::json json = nlohmann::json::parse(stream, nullptr, false);
        if (!json.is_object()) {
          return std::nullopt;
        }
        if (auto guid_it = json.find("guid"); guid_it != json.end() && guid_it->is_string()) {
          if (auto parsed = parse_uuid_string(guid_it->get<std::string>())) {
            return parsed;
          }
        }
      } catch (...) {
      }
      return std::nullopt;
    }

    void write_guid_to_state_locked(const uuid_util::uuid_t &uuid) {
      statefile::migrate_recent_state_keys();
      const auto &path_str = statefile::vibeshine_state_path();
      if (path_str.empty()) {
        return;
      }

      std::lock_guard<std::mutex> lock(statefile::state_mutex());
      const fs::path path(path_str);
      pt::ptree tree;
      if (!statefile::load_json_for_update(path.string(), tree)) {
        return;
      }

      tree.put("root.virtual_display_guid", uuid.string());

      try {
        statefile::write_json_atomic(path.string(), tree);
      } catch (...) {
      }
    }

    [[maybe_unused]] uuid_util::uuid_t ensure_persistent_guid() {
      static std::mutex guid_mutex;
      static std::optional<uuid_util::uuid_t> cached;

      std::lock_guard<std::mutex> lg(guid_mutex);
      if (cached) {
        return *cached;
      }

      if (auto existing = load_guid_from_state_locked()) {
        cached = *existing;
        return *cached;
      }

      if (auto legacy = load_guid_from_legacy_cache_locked()) {
        cached = *legacy;
        write_guid_to_state_locked(*legacy);
        return *cached;
      }

      auto generated = uuid_util::uuid_t::generate();
      cached = generated;
      write_guid_to_state_locked(generated);
      return *cached;
    }

    constexpr auto RECOVERY_STABLE_REQUIREMENT = std::chrono::seconds(2);
    constexpr auto RECOVERY_CHECK_INTERVAL = std::chrono::milliseconds(150);
    constexpr auto RECOVERY_RETRY_DELAY = std::chrono::milliseconds(350);
    constexpr auto RECOVERY_MISSING_GRACE = std::chrono::milliseconds(500);
    // A configured virtual display goes inactive while the display helper switches
    // topology, mode, or HDR state, and recreating it there races the helper into
    // another exclusive-layout transition. The helper's own apply/verify path runs a
    // re-apply ladder out to 5.5s, so tolerate an inactive target well past that
    // before treating it as driver loss. A truly missing output is a separate,
    // much shorter condition (RECOVERY_MISSING_GRACE).
    constexpr auto RECOVERY_INACTIVE_GRACE = std::chrono::seconds(12);
    constexpr auto RECOVERY_NO_ACTIVE_GRACE = std::chrono::seconds(10);
    // An empty libdisplaydevice result is ambiguous because it also represents a failed
    // CCD query. The monitor only enters the no-devices state after a separate successful
    // active-path query returns zero paths. Keep a longer grace for that confirmed
    // topology gap so session teardown can restore the physical layout, while a genuinely
    // headless host still recovers after the zero-path result persists.
    constexpr auto RECOVERY_NO_DEVICES_GRACE = std::chrono::seconds(20);
    constexpr auto RECOVERY_INITIAL_SETTLE_GRACE = std::chrono::seconds(6);
    constexpr auto RECOVERY_POST_SUCCESS_GRACE = std::chrono::seconds(3);
    constexpr auto RECOVERY_MAX_ATTEMPTS_BACKOFF = std::chrono::seconds(5);
    constexpr auto RECOVERY_MAX_BACKOFF = std::chrono::seconds(60);
    constexpr auto DRIVER_RECOVERY_WARMUP_DELAY = std::chrono::milliseconds(500);

    vdisplay_recovery::monitor_registry_t &recovery_monitors() {
      // Function-local lifetime ensures the fallback destructor runs before
      // later-initialized driver transport globals are destroyed.
      static vdisplay_recovery::monitor_registry_t monitors;
      return monitors;
    }

    void abort_recovery_monitor(const uuid_util::uuid_t &guid_uuid) {
      // Do not join here: callers can hold the virtual-display operation
      // mutex while the monitor is waiting to acquire that same mutex.
      recovery_monitors().request_stop(guid_uuid.string());
      deferred_hdr_profile_workers().request_stop(guid_uuid.string());
      deferred_scale_workers().request_stop(guid_uuid.string());
    }

    void abort_all_recovery_monitors() {
      recovery_monitors().request_stop_all();
      deferred_hdr_profile_workers().request_stop_all();
      deferred_scale_workers().request_stop_all();
    }

    struct RecoveryMonitorState {
      VirtualDisplayRecoveryParams params;
      uuid_util::uuid_t guid_uuid;
      bool confirmed_active_at_schedule = false;
      std::optional<std::wstring> current_display_name;
      std::optional<std::string> normalized_display_name;
      std::optional<std::string> current_device_id;
      std::optional<std::wstring> current_monitor_device_path;
      std::optional<std::string> normalized_monitor_device_path;

      explicit RecoveryMonitorState(const VirtualDisplayRecoveryParams &p):
          params(p),
          guid_uuid(guid_to_uuid(p.guid)),
          current_display_name(p.display_name),
          current_device_id(p.device_id),
          current_monitor_device_path(p.monitor_device_path) {
        if (current_display_name && !current_display_name->empty()) {
          normalized_display_name = normalize_display_name(platf::to_utf8(*current_display_name));
        }
        if (current_monitor_device_path && !current_monitor_device_path->empty()) {
          normalized_monitor_device_path = normalize_display_name(platf::to_utf8(*current_monitor_device_path));
        }
      }

      void update_identifiers(
        const std::optional<std::wstring> &display_name,
        const std::optional<std::string> &device_id,
        const std::optional<std::wstring> &monitor_device_path
      ) {
        current_display_name = display_name;
        current_device_id = device_id;
        current_monitor_device_path = monitor_device_path;
        normalized_display_name.reset();
        normalized_monitor_device_path.reset();
        if (current_display_name && !current_display_name->empty()) {
          normalized_display_name = normalize_display_name(platf::to_utf8(*current_display_name));
        }
        if (current_monitor_device_path && !current_monitor_device_path->empty()) {
          normalized_monitor_device_path = normalize_display_name(platf::to_utf8(*current_monitor_device_path));
        }
      }

      std::string describe_target() const {
        std::string description;
        if (current_device_id && !current_device_id->empty()) {
          description += "device_id='" + *current_device_id + "'";
        }
        if (current_monitor_device_path && !current_monitor_device_path->empty()) {
          if (!description.empty()) {
            description += ' ';
          }
          description += "monitor_device_path='" + platf::to_utf8(*current_monitor_device_path) + "'";
        }
        if (current_display_name && !current_display_name->empty()) {
          if (!description.empty()) {
            description += ' ';
          }
          description += "display_name='" + platf::to_utf8(*current_display_name) + "'";
        }
        if (description.empty()) {
          description = "guid=" + guid_uuid.string();
        }
        return description;
      }
    };

    bool monitor_should_abort(const RecoveryMonitorState &state, std::stop_token stop_token) {
      return stop_token.stop_requested() || (state.params.should_abort && state.params.should_abort());
    }

    bool wait_for_monitor_stop(std::stop_token stop_token, std::chrono::steady_clock::duration duration) {
      const auto deadline = std::chrono::steady_clock::now() + std::max(duration, std::chrono::steady_clock::duration::zero());
      while (!stop_token.stop_requested()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          return false;
        }
        const auto remaining = deadline - now;
        const auto sleep_for = std::max(
          std::chrono::duration_cast<std::chrono::milliseconds>(remaining),
          std::chrono::milliseconds(1)
        );
        std::this_thread::sleep_for(std::min(sleep_for, std::chrono::milliseconds(50)));
      }
      return true;
    }

    bool lock_recovery_operation(
      std::unique_lock<std::recursive_mutex> &operation_lock,
      const RecoveryMonitorState &state,
      std::stop_token stop_token
    ) {
      while (!operation_lock.try_lock()) {
        if (monitor_should_abort(state, stop_token) || wait_for_monitor_stop(stop_token, std::chrono::milliseconds(25))) {
          return false;
        }
      }
      if (monitor_should_abort(state, stop_token)) {
        operation_lock.unlock();
        return false;
      }
      return true;
    }

    enum class MonitorTargetPresence {
      missing,
      no_devices,
      present_inactive,
      present_active,
      unknown,
    };

    std::optional<bool> ccd_has_active_paths() {
      constexpr UINT flags = QDC_ONLY_ACTIVE_PATHS;
      constexpr int MAX_QUERY_ATTEMPTS = 3;

      for (int attempt = 0; attempt < MAX_QUERY_ATTEMPTS; ++attempt) {
        UINT32 path_count = 0;
        UINT32 mode_count = 0;
        if (GetDisplayConfigBufferSizes(flags, &path_count, &mode_count) != ERROR_SUCCESS) {
          return std::nullopt;
        }

        // QueryDisplayConfig requires both array pointers to be non-null even
        // when a headless topology reports zero entries.
        path_count = std::max<UINT32>(path_count, 1);
        mode_count = std::max<UINT32>(mode_count, 1);
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
        const LONG query_result = QueryDisplayConfig(
          flags,
          &path_count,
          paths.data(),
          &mode_count,
          modes.data(),
          nullptr
        );
        if (query_result == ERROR_INSUFFICIENT_BUFFER) {
          continue;
        }
        if (query_result != ERROR_SUCCESS) {
          return std::nullopt;
        }
        return path_count != 0;
      }

      return std::nullopt;
    }

    MonitorTargetPresence monitor_target_presence(RecoveryMonitorState &state) {
      auto devices = platf::display_helper::Coordinator::instance().enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
      if (!devices) {
        // Enumeration threw, so the target's presence cannot be determined yet.
        return MonitorTargetPresence::unknown;
      }
      if (devices->empty()) {
        // libdisplaydevice collapses both query failure and a genuinely empty topology
        // onto an empty list. Only accept a separate successful zero-active-path query as
        // evidence of a headless topology; otherwise interrupt the recovery grace.
        const auto has_active_paths = ccd_has_active_paths();
        if (!has_active_paths || *has_active_paths) {
          return MonitorTargetPresence::unknown;
        }
        return MonitorTargetPresence::no_devices;
      }

      const bool has_stable_identity =
        (state.current_device_id && !state.current_device_id->empty()) ||
        state.normalized_monitor_device_path.has_value();
      bool matched_strong_inactive = false;
      std::optional<display_device::EnumeratedDevice> unique_weak_candidate;
      bool weak_candidate_conflict = false;

      for (const auto &device : *devices) {
        if (!is_virtual_display_device(device)) {
          continue;
        }

        bool strong_match = false;
        if (state.normalized_monitor_device_path && !device.m_monitor_device_path.empty()) {
          const auto normalized_path = normalize_display_name(device.m_monitor_device_path);
          if (!normalized_path.empty() && normalized_path == *state.normalized_monitor_device_path) {
            strong_match = true;
          }
        }
        if (!strong_match && state.current_device_id && !state.current_device_id->empty() && !device.m_device_id.empty()) {
          strong_match = equals_ci(device.m_device_id, *state.current_device_id);
        }
        if (!strong_match && state.normalized_display_name && !device.m_display_name.empty()) {
          const auto normalized_display = normalize_display_name(device.m_display_name);
          if (!normalized_display.empty() && normalized_display == *state.normalized_display_name) {
            strong_match = true;
          }
        }

        if (strong_match) {
          if (!device.m_display_name.empty()) {
            return MonitorTargetPresence::present_active;
          }
          matched_strong_inactive = true;
          continue;
        }

        if (!has_stable_identity) {
          bool weak_match =
            !state.params.client_name.empty() &&
            !device.m_friendly_name.empty() &&
            equals_ci(device.m_friendly_name, state.params.client_name);
          if (!weak_match && state.normalized_display_name && !device.m_friendly_name.empty()) {
            const auto normalized_friendly = normalize_display_name(device.m_friendly_name);
            weak_match = !normalized_friendly.empty() && normalized_friendly == *state.normalized_display_name;
          }
          if (weak_match && !weak_candidate_conflict) {
            if (!unique_weak_candidate) {
              unique_weak_candidate = device;
            } else {
              weak_candidate_conflict = true;
              unique_weak_candidate.reset();
            }
          }
        }
      }

      if (matched_strong_inactive) {
        return MonitorTargetPresence::present_inactive;
      }

      if (!weak_candidate_conflict && unique_weak_candidate) {
        const auto &device = *unique_weak_candidate;
        auto adopted_display_name = state.current_display_name;
        if (!device.m_display_name.empty()) {
          adopted_display_name = platf::from_utf8(device.m_display_name);
        }
        auto adopted_device_id = state.current_device_id;
        if (!device.m_device_id.empty()) {
          adopted_device_id = device.m_device_id;
        }
        auto adopted_monitor_device_path = state.current_monitor_device_path;
        if (!device.m_monitor_device_path.empty()) {
          adopted_monitor_device_path = platf::from_utf8(device.m_monitor_device_path);
        }

        const auto before = state.describe_target();
        state.update_identifiers(adopted_display_name, adopted_device_id, adopted_monitor_device_path);
        BOOST_LOG(debug) << "Virtual display recovery monitor adopted unique identifiers via client_name '"
                         << state.params.client_name << "': " << before << " -> " << state.describe_target();
        return !device.m_display_name.empty() ?
                 MonitorTargetPresence::present_active :
                 MonitorTargetPresence::present_inactive;
      }

      return MonitorTargetPresence::missing;
    }

    bool attempt_virtual_display_recovery(RecoveryMonitorState &state, std::stop_token stop_token) {
      if (!release_retained_ensure_display_for_stream(
            state.params.guid,
            state.params.client_uid.c_str(),
            stop_token)) {
        return false;
      }
      std::unique_lock<std::recursive_mutex> operation_lock(g_virtual_display_operation_mutex, std::defer_lock);
      if (!lock_recovery_operation(operation_lock, state, stop_token)) {
        return false;
      }
      if (!ensure_driver_is_ready_impl(RestartCooldownBehavior::skip, stop_token)) {
        BOOST_LOG(warning) << "Virtual display recovery: driver not ready for " << state.describe_target();
        return false;
      }
      if (monitor_should_abort(state, stop_token)) {
        return false;
      }

      proc::vDisplayDriverStatus.store(
        open_vdisplay_device_impl(stop_token, OpenRecoveryBehavior::transport_only),
        std::memory_order_release
      );
      const auto driver_status = proc::vDisplayDriverStatus.load(std::memory_order_acquire);
      if (driver_status != DRIVER_STATUS::OK) {
        BOOST_LOG(warning) << "Virtual display recovery: failed to reopen driver (status="
                           << static_cast<int>(driver_status) << ") for "
                           << state.describe_target();
        return false;
      }
      if (monitor_should_abort(state, stop_token)) {
        return false;
      }

      // Restart the watchdog ping thread with the new driver handle.
      // The old ping thread is still feeding a stale duplicated handle;
      // startPingThread stops it and duplicates the freshly opened handle.
      if (auto watchdog_fail_cb = copy_watchdog_fail_cb(); watchdog_fail_cb) {
        if (!start_ping_thread_impl(
              std::move(watchdog_fail_cb),
              stop_token,
              OpenRecoveryBehavior::transport_only
            )) {
          BOOST_LOG(warning) << "Virtual display recovery: failed to restart watchdog ping thread for "
                             << state.describe_target();
        }
      }
      if (monitor_should_abort(state, stop_token)) {
        return false;
      }

      setWatchdogFeedingEnabled(true);
      auto recreation = create_virtual_display_with_stop(
        state.params.client_uid.c_str(),
        state.params.client_name.c_str(),
        state.params.hdr_profile ? state.params.hdr_profile->c_str() : nullptr,
        state.params.width,
        state.params.height,
        state.params.fps,
        state.params.guid,
        state.params.base_fps_millihz,
        state.params.framegen_refresh_active,
        state.params.framegen_refresh_multiplier,
        state.params.hdr_requested,
        false,
        true,
        stop_token
      );
      if (!recreation) {
        BOOST_LOG(warning) << "Virtual display recovery: createVirtualDisplay failed for " << state.describe_target();
        return false;
      }

      const auto remove_recreated_display = [&] {
        BOOST_LOG(debug) << "Virtual display recovery cleaning up a recreated display for " << state.describe_target();
        if (!remove_virtual_display_impl(state.params.guid, false, stop_token)) {
          BOOST_LOG(warning) << "Virtual display recovery could not remove the cancelled recreation for "
                             << state.describe_target();
        }
      };
      state.update_identifiers(recreation->display_name, recreation->device_id, recreation->monitor_device_path);
      if (monitor_should_abort(state, stop_token)) {
        BOOST_LOG(debug) << "Virtual display recovery aborted after recreation for " << state.describe_target();
        remove_recreated_display();
        return false;
      }
      std::function<void()> rollback_recovery_publication;
      const auto rollback_recovery_publication_if_needed = [&] {
        if (!rollback_recovery_publication) {
          return;
        }

        try {
          rollback_recovery_publication();
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "Virtual display recovery publication rollback failed for "
                           << state.describe_target() << ": " << e.what();
        } catch (...) {
          BOOST_LOG(error) << "Virtual display recovery publication rollback failed for "
                           << state.describe_target() << '.';
        }
        rollback_recovery_publication = {};
      };
      if (state.params.on_recovery_success) {
        try {
          rollback_recovery_publication = state.params.on_recovery_success(*recreation, stop_token);
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "Virtual display recovery callback failed for " << state.describe_target() << ": " << e.what();
          rollback_recovery_publication_if_needed();
          remove_recreated_display();
          return false;
        } catch (...) {
          BOOST_LOG(error) << "Virtual display recovery callback failed for " << state.describe_target() << '.';
          rollback_recovery_publication_if_needed();
          remove_recreated_display();
          return false;
        }
      }
      if (monitor_should_abort(state, stop_token)) {
        BOOST_LOG(debug) << "Virtual display recovery aborted after callback for " << state.describe_target();
        rollback_recovery_publication_if_needed();
        remove_recreated_display();
        return false;
      }
      return true;
    }

    void run_virtual_display_recovery_monitor(RecoveryMonitorState state, std::stop_token stop_token) {
      unsigned int attempts = 0;
      unsigned int backoff_cycles = 0;
      constexpr unsigned int MAX_RECOVERY_BACKOFF_CYCLES = 5;
      bool observed_active = state.confirmed_active_at_schedule;
      std::optional<std::chrono::steady_clock::time_point> active_since =
        state.confirmed_active_at_schedule ? std::make_optional(std::chrono::steady_clock::now()) : std::nullopt;
      std::optional<std::chrono::steady_clock::time_point> inactive_since;
      std::optional<std::chrono::steady_clock::time_point> missing_since;
      std::optional<std::chrono::steady_clock::time_point> no_devices_since;
      auto recovery_cooldown_until = std::chrono::steady_clock::now() + RECOVERY_INITIAL_SETTLE_GRACE;

      // Recovery is intentionally forbidden during the initial settle grace. Avoid
      // contending with the helper's topology work by checking only for cancellation
      // when activity was already confirmed. An initially inactive target still needs
      // presence observations so a brief helper activation establishes recovery history.
      while (std::chrono::steady_clock::now() < recovery_cooldown_until) {
        if (monitor_should_abort(state, stop_token)) {
          BOOST_LOG(debug) << "Virtual display recovery monitor aborted for " << state.describe_target();
          return;
        }
        if (!observed_active && monitor_target_presence(state) == MonitorTargetPresence::present_active) {
          observed_active = true;
          active_since = std::chrono::steady_clock::now();
          BOOST_LOG(debug) << "Virtual display recovery monitor observed initial activation for "
                           << state.describe_target() << ".";
        }
        const auto remaining = std::max(
          std::chrono::milliseconds(1),
          std::chrono::duration_cast<std::chrono::milliseconds>(
            recovery_cooldown_until - std::chrono::steady_clock::now()
          )
        );
        if (wait_for_monitor_stop(stop_token, std::min(RECOVERY_CHECK_INTERVAL, remaining))) {
          return;
        }
      }

      while (true) {
        if (monitor_should_abort(state, stop_token)) {
          BOOST_LOG(debug) << "Virtual display recovery monitor aborted for " << state.describe_target();
          return;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto presence = monitor_target_presence(state);

        if (presence == MonitorTargetPresence::unknown) {
          active_since.reset();
          inactive_since.reset();
          missing_since.reset();
          no_devices_since.reset();
          if (wait_for_monitor_stop(stop_token, RECOVERY_CHECK_INTERVAL)) {
            return;
          }
          continue;
        }

        if (presence == MonitorTargetPresence::present_active) {
          observed_active = true;
          backoff_cycles = 0;
          missing_since.reset();
          no_devices_since.reset();
          inactive_since.reset();
          if (!active_since) {
            active_since = now;
          } else if (now - *active_since >= RECOVERY_STABLE_REQUIREMENT) {
            attempts = 0;
          }
          if (wait_for_monitor_stop(stop_token, RECOVERY_CHECK_INTERVAL)) {
            return;
          }
          continue;
        }

        active_since.reset();

      // A newly created target may be enumerated before the helper activates it.
        // Keep the monitor dormant until this exact target has been observed active;
        // never recover an identity that has not established active history.
        if (!observed_active) {
          if (wait_for_monitor_stop(stop_token, RECOVERY_CHECK_INTERVAL)) {
            return;
          }
          continue;
      }

        // Defer recovery attempts for a short grace window after a successful recovery. This allows
        // the display stack and helper APPLY to stabilize without immediately retriggering recovery.
        if (now < recovery_cooldown_until) {
          if (presence == MonitorTargetPresence::missing) {
            missing_since.reset();
          } else if (presence == MonitorTargetPresence::no_devices) {
            no_devices_since.reset();
          } else {
            inactive_since.reset();
          }
          if (wait_for_monitor_stop(stop_token, RECOVERY_CHECK_INTERVAL)) {
            return;
          }
          continue;
        }

        std::optional<std::chrono::steady_clock::time_point> *issue_since = nullptr;
        std::chrono::steady_clock::duration required_grace {};
        const char *issue_label = "unknown";
        if (presence == MonitorTargetPresence::missing) {
          inactive_since.reset();
          no_devices_since.reset();
          issue_since = &missing_since;
          required_grace = RECOVERY_MISSING_GRACE;
          issue_label = "missing";
        } else if (presence == MonitorTargetPresence::no_devices) {
          // A direct successful CCD query confirmed that the active topology is empty.
          // Keep a longer grace than a target that vanished from an otherwise healthy
          // device list so a session teardown can restore the physical layout.
          inactive_since.reset();
          missing_since.reset();
          issue_since = &no_devices_since;
          required_grace = RECOVERY_NO_DEVICES_GRACE;
          issue_label = "no_devices";
        } else {
          missing_since.reset();
          no_devices_since.reset();
          issue_since = &inactive_since;
          required_grace = observed_active ? RECOVERY_INACTIVE_GRACE : RECOVERY_NO_ACTIVE_GRACE;
          issue_label = "inactive";
        }

        if (!issue_since->has_value()) {
          *issue_since = now;
          if (wait_for_monitor_stop(stop_token, RECOVERY_CHECK_INTERVAL)) {
            return;
          }
          continue;
        }

        const auto issue_for = now - **issue_since;
        if (issue_for < required_grace) {
          if (wait_for_monitor_stop(stop_token, RECOVERY_CHECK_INTERVAL)) {
            return;
          }
          continue;
        }

        if (attempts >= state.params.max_attempts) {
          if (backoff_cycles >= MAX_RECOVERY_BACKOFF_CYCLES) {
            BOOST_LOG(warning) << "Virtual display recovery monitor exhausted retry backoffs for "
                               << state.describe_target() << "; disabling automatic recovery.";
            return;
          }

          const auto base_backoff = RECOVERY_MAX_ATTEMPTS_BACKOFF;
          const auto multiplier = std::min<unsigned int>(backoff_cycles, 4U);
          auto backoff = base_backoff * (1U << multiplier);
          if (backoff > RECOVERY_MAX_BACKOFF) {
            backoff = RECOVERY_MAX_BACKOFF;
          }
          backoff_cycles += 1;

          const auto backoff_ms = std::chrono::duration_cast<std::chrono::milliseconds>(backoff).count();
          BOOST_LOG(warning) << "Virtual display recovery monitor reached max attempts for "
                             << state.describe_target() << "; backing off for " << backoff_ms << "ms.";
          attempts = 0;
          recovery_cooldown_until = std::chrono::steady_clock::now() + backoff;
          inactive_since.reset();
          missing_since.reset();
          no_devices_since.reset();
          if (wait_for_monitor_stop(stop_token, backoff)) {
            return;
          }
          continue;
        }

        attempts += 1;
        const auto issue_ms = std::chrono::duration_cast<std::chrono::milliseconds>(issue_for).count();
        BOOST_LOG(warning) << "Virtual display recovery monitor detected disappearance for "
                           << state.describe_target() << " (attempt "
                           << attempts << '/' << state.params.max_attempts
                           << ", " << issue_label << "_for=" << issue_ms << "ms).";

        if (monitor_should_abort(state, stop_token)) {
          BOOST_LOG(debug) << "Virtual display recovery monitor aborted for " << state.describe_target();
          return;
        }
        const bool recovered = attempt_virtual_display_recovery(state, stop_token);
        inactive_since.reset();
        missing_since.reset();
        no_devices_since.reset();
        active_since.reset();

        if (recovered) {
          observed_active = false;
          recovery_cooldown_until = std::chrono::steady_clock::now() + RECOVERY_POST_SUCCESS_GRACE;
        } else {
          recovery_cooldown_until = std::chrono::steady_clock::now() + RECOVERY_RETRY_DELAY;
        }

        if (wait_for_monitor_stop(stop_token, RECOVERY_RETRY_DELAY)) {
          return;
        }
      }
    }
  }  // namespace

  void applyHdrProfileToOutput(const char *s_client_name, const char *s_hdr_profile, const char *s_device_id) {
    // Only apply HDR profiles when explicitly selected by the user.
    if (!s_hdr_profile || std::strlen(s_hdr_profile) == 0) {
      return;
    }
    std::optional<std::string> device_id;
    if (s_device_id && std::strlen(s_device_id) > 0) {
      device_id = std::string(s_device_id);
    }
    const std::optional<std::string> client_name =
      (s_client_name && std::strlen(s_client_name) > 0) ? std::make_optional(std::string(s_client_name)) : std::nullopt;
    const std::optional<std::string> hdr_profile = std::string(s_hdr_profile);

    // Physical displays: best-effort apply; do not clear mismatched profiles.
    apply_hdr_profile_if_available(
      std::nullopt,
      device_id,
      std::nullopt,
      client_name,
      hdr_profile,
      false
    );
  }

  void restorePhysicalHdrProfiles() {
    std::unordered_map<std::wstring, std::optional<std::wstring>> to_restore;
    {
      std::lock_guard<std::mutex> lock(g_physical_hdr_profile_restore_mutex);
      if (g_physical_hdr_profile_restore.empty()) {
        return;
      }
      to_restore.swap(g_physical_hdr_profile_restore);
    }

    std::thread([entries = std::move(to_restore)]() mutable {
      auto restore_profiles = [&]() {
        for (const auto &[monitor_path, previous] : entries) {
          if (monitor_path.empty()) {
            continue;
          }
          bool ok = false;
          if (previous && !previous->empty()) {
            ok = write_color_profile_association(
              monitor_path,
              fs::path(*previous).filename().wstring(),
              color_profile_scope_e::current_user
            );
          } else {
            const auto current = read_color_profile_association(monitor_path, color_profile_scope_e::current_user);
            ok = clear_color_profile_association(monitor_path, current, color_profile_scope_e::current_user);
          }
          if (ok) {
            BOOST_LOG(info) << "HDR profile: restored physical display color profile association for '"
                            << platf::to_utf8(monitor_path) << "'.";
          } else {
            BOOST_LOG(warning) << "HDR profile: failed to restore physical display color profile association for '"
                               << platf::to_utf8(monitor_path) << "'.";
          }
        }
      };

      HANDLE user_token = platf::retrieve_users_token(false);
      if (user_token) {
        (void) platf::impersonate_current_user(user_token, restore_profiles);
        CloseHandle(user_token);
        return;
      }

      DWORD session_id = 0;
      if (!ProcessIdToSessionId(GetCurrentProcessId(), &session_id) || session_id == 0) {
        BOOST_LOG(warning) << "HDR profile: unable to restore physical display profiles (no user token).";
        return;
      }

      BOOST_LOG(debug) << "HDR profile: no user token; restoring physical display profiles in current user context.";
      restore_profiles();
    }).detach();
  }

  bool is_virtual_display_guid_tracked(const GUID &guid) {
    return is_virtual_display_guid_tracked(guid_to_uuid(guid));
  }

  void schedule_virtual_display_recovery_monitor(const VirtualDisplayRecoveryParams &params) {
    if (params.max_attempts == 0) {
      return;
    }
    if (recovery_monitors().shutdown_requested()) {
      BOOST_LOG(debug) << "Virtual display recovery monitor skipped during process shutdown.";
      return;
    }

    const auto guid_uuid = guid_to_uuid(params.guid);
    const bool has_device_id = params.device_id && !params.device_id->empty();
    const bool has_display_name = params.display_name && !params.display_name->empty();
    const bool has_client_name = !params.client_name.empty();
    if (!has_device_id && !has_display_name && !has_client_name) {
      BOOST_LOG(debug) << "Virtual display recovery monitor skipped: no identifiers available.";
      return;
    }

    RecoveryMonitorState initial_state(params);
    if (monitor_should_abort(initial_state, {})) {
      BOOST_LOG(debug) << "Virtual display recovery monitor skipped for " << initial_state.describe_target()
                       << ": already aborted before scheduling.";
      return;
    }

    initial_state.confirmed_active_at_schedule = params.confirmed_active_at_schedule;

    RecoveryMonitorState state(params);
    state.confirmed_active_at_schedule = initial_state.confirmed_active_at_schedule;
    BOOST_LOG(debug) << "Virtual display recovery monitor scheduled for " << state.describe_target()
                     << " (max_attempts=" << params.max_attempts
                     << (state.confirmed_active_at_schedule ? ", active confirmed)." : ", awaiting first active observation).");
    if (!recovery_monitors().start(
          guid_uuid.string(),
          [state = std::move(state)](std::stop_token stop_token) mutable {
            run_virtual_display_recovery_monitor(std::move(state), stop_token);
          }
        )) {
      BOOST_LOG(warning) << "Virtual display recovery monitor could not be started for " << guid_uuid.string() << '.';
    }
  }

  void cancel_all_virtual_display_recovery_monitors() {
    abort_all_recovery_monitors();
  }

  void request_virtual_display_recovery_shutdown() {
    recovery_monitors().request_shutdown();
    watchdog_failure_callbacks().request_shutdown();
    deferred_hdr_profile_workers().request_shutdown();
  }

  void join_virtual_display_recovery_monitors() {
    recovery_monitors().join_all();
    watchdog_failure_callbacks().join_all();
    deferred_hdr_profile_workers().join_all();
  }

  // {dff7fd29-5b75-41d1-9731-b32a17a17104}
  // static const GUID DEFAULT_DISPLAY_GUID = { 0xdff7fd29, 0x5b75, 0x41d1, { 0x97, 0x31, 0xb3, 0x2a, 0x17, 0xa1, 0x71, 0x04 } };

  uint64_t client_uuid_to_virtual_display_id(const GUID &client_guid) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(&client_guid);
    std::uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < sizeof(GUID); ++i) {
      hash ^= static_cast<std::uint64_t>(bytes[i]);
      hash *= 1099511628211ull;
    }
    return hash == 0 ? 1 : hash;
  }

  uuid_util::uuid_t virtualDisplayUuidFromStableId(const std::string &stable_id) {
    if (auto parsed = parse_uuid_string(stable_id)) {
      return *parsed;
    }

    uuid_util::uuid_t uuid {};
    constexpr std::uint64_t k_fnv_offset = 14695981039346656037ull;
    constexpr std::uint64_t k_fnv_prime = 1099511628211ull;

    auto hash_with_domain = [&](const std::string_view domain) {
      std::uint64_t hash = k_fnv_offset;
      for (const auto ch : domain) {
        hash ^= static_cast<std::uint8_t>(ch);
        hash *= k_fnv_prime;
      }
      for (const auto ch : stable_id) {
        hash ^= static_cast<std::uint8_t>(ch);
        hash *= k_fnv_prime;
      }
      return hash;
    };

    uuid.b64[0] = hash_with_domain("sunshine-virtual-display-a:");
    uuid.b64[1] = hash_with_domain("sunshine-virtual-display-b:");
    uuid.b8[6] = static_cast<std::uint8_t>((uuid.b8[6] & 0x0f) | 0x50);
    uuid.b8[8] = static_cast<std::uint8_t>((uuid.b8[8] & 0x3f) | 0x80);
    if (uuid.b64[0] == 0 && uuid.b64[1] == 0) {
      uuid.b8[15] = 1;
    }
    return uuid;
  }

  GUID sharedVirtualDisplayGuid() {
    return VDISPLAY::sharedVirtualDisplayGuid();
  }

  bool is_sunshine_virtual_display_identity(
    const std::string &device_path,
    const std::string &friendly_name,
    const std::string &edid_manufacturer_id,
    const std::string &edid_product_code
  ) {
    if (contains_ci(device_path, "SunshineVirtualDisplay") ||
        contains_ci(device_path, "Sunshine Virtual Display")) {
      return true;
    }
    if (equals_ci(friendly_name, "Sunshine Virtual Display Driver")) {
      return true;
    }
    if (!equals_ci(edid_manufacturer_id, "SDD")) {
      return false;
    }
    return starts_with_ci(edid_product_code, "4") ||
           starts_with_ci(edid_product_code, "0x4") ||
           starts_with_ci(edid_product_code, "5") ||
           starts_with_ci(edid_product_code, "0x5");
  }

  std::mutex g_control_transport_mutex;
  std::shared_ptr<sunshine_driver::WindowsControlTransport> VIRTUAL_DISPLAY_DRIVER_TRANSPORT;

  std::shared_ptr<sunshine_driver::WindowsControlTransport> control_transport_snapshot() {
    std::lock_guard<std::mutex> lock(g_control_transport_mutex);
    return VIRTUAL_DISPLAY_DRIVER_TRANSPORT;
  }

  std::shared_ptr<sunshine_driver::WindowsControlTransport> replace_control_transport(
    std::unique_ptr<sunshine_driver::WindowsControlTransport> replacement
  ) {
    auto shared = std::shared_ptr<sunshine_driver::WindowsControlTransport> {std::move(replacement)};
    std::shared_ptr<sunshine_driver::WindowsControlTransport> previous;
    {
      std::lock_guard<std::mutex> lock(g_control_transport_mutex);
      previous = std::exchange(VIRTUAL_DISPLAY_DRIVER_TRANSPORT, shared);
    }
    if (previous) {
      previous->cancel_pending_io();
    }
    return shared;
  }

  void clear_control_transport() {
    std::shared_ptr<sunshine_driver::WindowsControlTransport> previous;
    {
      std::lock_guard<std::mutex> lock(g_control_transport_mutex);
      previous = std::move(VIRTUAL_DISPLAY_DRIVER_TRANSPORT);
    }
    if (previous) {
      previous->cancel_pending_io();
    }
  }

  void closeVDisplayDevice() {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(g_watchdog_lifecycle_mutex);
    stop_watchdog_thread();
    set_watchdog_feed_requested(false, false);
    g_watchdog_grace_deadline_ns.store(0, std::memory_order_release);
    clear_control_transport();
  }

  bool ensure_control_transport_responsive(
    std::string_view operation,
    std::stop_token stop_token = {},
    OpenRecoveryBehavior recovery_behavior = OpenRecoveryBehavior::recover_driver
  ) {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(g_watchdog_lifecycle_mutex);
    if (stop_token.stop_requested()) {
      return false;
    }
    auto transport = control_transport_snapshot();
    const bool transport_responsive = driver_transport_responsive(transport.get());
    if (!VDISPLAY::policy::should_reopen_control_transport(
          transport && transport->valid(),
          transport_responsive
        )) {
      return true;
    }

    if (transport) {
      BOOST_LOG(debug) << operation << ": cached Sunshine virtual display driver transport is not responsive; reopening.";
      closeVDisplayDevice();
    }

    const auto status = open_vdisplay_device_impl(stop_token, recovery_behavior);
    if (status != DRIVER_STATUS::OK) {
      BOOST_LOG(warning) << operation << ": failed to open Sunshine virtual display driver transport (status="
                         << static_cast<int>(status) << ").";
      return false;
    }

    transport = control_transport_snapshot();
    if (!driver_transport_responsive(transport.get())) {
      BOOST_LOG(warning) << operation << ": opened Sunshine virtual display driver transport is not responsive.";
      closeVDisplayDevice();
      return false;
    }

    return true;
  }

  void ensureVirtualDisplayRegistryDefaults() {
    // The Sunshine driver is runtime-only in this pass and does not require registry defaults.
  }

  static DRIVER_STATUS open_vdisplay_device_impl(std::stop_token stop_token, OpenRecoveryBehavior recovery_behavior) {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(g_watchdog_lifecycle_mutex);
    std::shared_ptr<sunshine_driver::WindowsControlTransport> transport;
    uint32_t retryInterval = 20;
    bool attempted_recovery = false;
    while (true) {
      if (stop_token.stop_requested()) {
        return DRIVER_STATUS::FAILED;
      }
      auto opened = sunshine_driver::open_first_control_device();
      if (!opened.ok()) {
        if (retryInterval > 320) {
          if (recovery_behavior == OpenRecoveryBehavior::recover_driver && !attempted_recovery) {
            attempted_recovery = true;
            if (ensure_driver_is_ready_impl(RestartCooldownBehavior::wait, stop_token)) {
              retryInterval = 20;
              continue;
            }
          }

          printf("[SunshineVirtualDisplay] Open control device failed (status=%s, error=%lu)!\n",
                 sunshine_driver::to_string(opened.status),
                 static_cast<unsigned long>(opened.native_error));
          return DRIVER_STATUS::FAILED;
        }
        retryInterval *= 2;
        if (wait_for_monitor_stop(stop_token, std::chrono::milliseconds(retryInterval))) {
          return DRIVER_STATUS::FAILED;
        }
        continue;
      }

      transport = replace_control_transport(std::move(opened.transport));
      break;
    }

    sunshine_driver::ControlClient client {*transport};
    const auto version = client.query_protocol_version();
    if (!version.ok()) {
      if (version.status == sunshine_driver::ControlStatus::ProtocolIncompatible &&
          !sunshine_driver::is_valid_api_namespace(version.value.api_namespace)) {
        BOOST_LOG(warning) << "Sunshine virtual display control protocol namespace mismatch.";
      } else if (version.status == sunshine_driver::ControlStatus::ProtocolIncompatible) {
        BOOST_LOG(warning) << "Sunshine virtual display control protocol version "
                           << version.value.major << '.' << version.value.minor << '.' << version.value.patch
                           << " is incompatible; require "
                           << REQUIRED_DRIVER_PROTOCOL_MAJOR << '.' << REQUIRED_DRIVER_PROTOCOL_MINOR << "+.";
      } else {
        log_control_failure("Sunshine virtual display protocol query", version.status, version.native_error);
      }

      printf("[SunshineVirtualDisplay] Control protocol query failed (status=%s, error=%lu)!\n",
             sunshine_driver::to_string(version.status),
             static_cast<unsigned long>(version.native_error));
      closeVDisplayDevice();
      const auto status_class = VDISPLAY::policy::classify_protocol_query(
        false,
        version.status == sunshine_driver::ControlStatus::ProtocolIncompatible
      );
      return status_class == VDISPLAY::policy::driver_status_class::version_incompatible ?
               DRIVER_STATUS::VERSION_INCOMPATIBLE :
               DRIVER_STATUS::FAILED;
    }

    if (!query_driver(client)) {
      printf("[SunshineVirtualDisplay] Control query failed!\n");
      closeVDisplayDevice();
      return DRIVER_STATUS::FAILED;
    }

    if (config::video.dd.virtual_display_permanent_count_configured &&
        !set_permanent_display_count(
          client,
          static_cast<std::uint32_t>(std::clamp(
            config::video.dd.virtual_display_permanent_count,
            0,
            config::SUNSHINE_VIRTUAL_DISPLAY_MAX_PERMANENT_COUNT
          ))
        )) {
      BOOST_LOG(warning) << "Unable to apply configured Sunshine virtual display permanent count; temporary display creation will still be attempted.";
    }

    if (!g_watchdog_start_in_progress && !driver_lease_tracker().all().empty()) {
      (void) ensure_watchdog_thread_active_for_lease(stop_token, recovery_behavior);
    }

    return DRIVER_STATUS::OK;
  }

  DRIVER_STATUS openVDisplayDevice() {
    // proc::initVDisplayDriver() probes/restarts the adapter immediately before
    // this call. Limit this phase to opening the transport so one initialization
    // attempt cannot enter a second PnP recovery cycle.
    return open_vdisplay_device_impl({}, OpenRecoveryBehavior::transport_only);
  }

  static bool ensure_driver_is_ready_impl(RestartCooldownBehavior cooldown_behavior, std::stop_token stop_token) {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(g_watchdog_lifecycle_mutex);
    if (stop_token.stop_requested()) {
      return false;
    }
    auto transport = control_transport_snapshot();
    if (driver_transport_responsive(transport.get())) {
      return true;
    }

    if (transport) {
      closeVDisplayDevice();
    }

    if (probe_driver_responsive_once()) {
      return true;
    }

    // Check if the device is stuck in the disabled state (CM_PROB_DISABLED)
    // before attempting a full restart cycle, which would make things worse.
    {
      auto instance_id = find_virtual_display_device_instance_id();
      if (instance_id && is_device_disabled(*instance_id)) {
        if (try_reenable_disabled_device(*instance_id, stop_token)) {
          if (probe_driver_responsive_once()) {
            BOOST_LOG(info) << "Sunshine virtual display driver responded after re-enabling disabled device.";
            if (wait_for_monitor_stop(stop_token, DRIVER_RECOVERY_WARMUP_DELAY)) {
              return false;
            }
            return true;
          }
        }
      }
    }

    for (int attempt = 1; attempt <= DRIVER_RESTART_MAX_ATTEMPTS; ++attempt) {
      if (stop_token.stop_requested()) {
        return false;
      }
      const auto now = std::chrono::steady_clock::now();
      std::chrono::milliseconds cooldown_remaining {0};
      if (should_skip_restart_attempt(now, cooldown_remaining)) {
        if (cooldown_behavior != RestartCooldownBehavior::wait) {
          BOOST_LOG(warning) << "Skipping Sunshine virtual display restart attempt due to recent failure (cooldown "
                             << cooldown_remaining.count() << " ms remaining).";
          return false;
        }

        BOOST_LOG(info) << "Delaying Sunshine virtual display restart attempt for " << cooldown_remaining.count()
                        << " ms due to restart cooldown.";
        if (wait_for_monitor_stop(stop_token, cooldown_remaining)) {
          return false;
        }
        if (probe_driver_responsive_once()) {
          return true;
        }
      }

      auto instance_id = find_virtual_display_device_instance_id();
      if (!instance_id) {
        BOOST_LOG(error) << "Unable to locate Sunshine virtual display adapter for recovery; streaming will continue with the active display. A reboot may be required.";
        note_restart_failure(std::chrono::steady_clock::now());
        return false;
      }

      BOOST_LOG(info) << "Attempting to restart Sunshine virtual display adapter " << platf::to_utf8(*instance_id) << " (attempt "
                      << attempt << '/' << DRIVER_RESTART_MAX_ATTEMPTS << ").";

      if (stop_token.stop_requested()) {
        return false;
      }
      if (!restart_virtual_display_device(*instance_id)) {
        BOOST_LOG(error) << "Sunshine virtual display adapter restart failed; streaming will continue with the active display. A reboot may be required.";
        note_restart_failure(std::chrono::steady_clock::now());
        continue;
      }
      if (stop_token.stop_requested()) {
        return false;
      }

      const auto deadline = std::chrono::steady_clock::now() + DRIVER_RESTART_TIMEOUT;
      while (std::chrono::steady_clock::now() < deadline) {
        if (stop_token.stop_requested()) {
          return false;
        }
        if (probe_driver_responsive_once()) {
          BOOST_LOG(info) << "Sunshine virtual display driver responded after restart.";
          if (wait_for_monitor_stop(stop_token, DRIVER_RECOVERY_WARMUP_DELAY)) {
            return false;
          }
          return true;
        }
        if (wait_for_monitor_stop(stop_token, DRIVER_RESTART_POLL_INTERVAL)) {
          return false;
        }
      }

      BOOST_LOG(error) << "Sunshine virtual display driver did not respond within the restart timeout; streaming will continue with the active display. A reboot may be required.";
      note_restart_failure(std::chrono::steady_clock::now());
    }

    return false;
  }

  bool ensure_driver_is_ready() {
    return ensure_driver_is_ready_impl(RestartCooldownBehavior::skip);
  }

  static bool start_ping_thread_impl(
    std::function<void()> failCb,
    std::stop_token stop_token,
    OpenRecoveryBehavior recovery_behavior
  ) {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(g_watchdog_lifecycle_mutex);
    if (stop_token.stop_requested()) {
      return false;
    }
    if (g_watchdog_start_in_progress) {
      return watchdog_thread_running();
    }
    g_watchdog_start_in_progress = true;
    auto clear_start_in_progress = util::fail_guard([]() {
      g_watchdog_start_in_progress = false;
    });
    stop_watchdog_thread();

    // Save the callback so recovery can restart the lease feed thread with the same callback.
    store_watchdog_fail_cb(failCb);
    auto failure_cb = std::make_shared<std::function<void()>>(std::move(failCb));

    if (!ensure_control_transport_responsive(
          "Sunshine virtual display lease feed",
          stop_token,
          recovery_behavior
        )) {
      return false;
    }

    auto opened = sunshine_driver::open_first_control_device();
    if (!opened.ok()) {
      printf("[SunshineVirtualDisplay] Lease feed: failed to open control device (status=%s, error=%lu).\n",
             sunshine_driver::to_string(opened.status),
             static_cast<unsigned long>(opened.native_error));
      return false;
    }
    if (stop_token.stop_requested()) {
      return false;
    }

    sunshine_driver::ControlClient ping_client {*opened.transport};
    if (!check_driver_protocol_compatible(ping_client)) {
      return false;
    }
    auto ping_transport = std::shared_ptr<sunshine_driver::WindowsControlTransport> {
      std::move(opened.transport)
    };

    const auto now = std::chrono::steady_clock::now();
    const auto deadline = now + WATCHDOG_INIT_GRACE;
    {
      std::lock_guard<std::mutex> wake_lock(g_watchdog_wake_mutex);
      const bool feed_was_requested = g_watchdog_feed_requested.load(std::memory_order_acquire);
      g_watchdog_stop_requested.store(false, std::memory_order_release);
      g_watchdog_grace_deadline_ns.store(steady_ticks_from_time(deadline), std::memory_order_release);
      g_watchdog_feed_requested.store(feed_was_requested, std::memory_order_release);
      ++g_watchdog_wake_generation;
    }
    g_watchdog_wake_cv.notify_all();

    const auto sleep_duration = std::chrono::milliseconds(DRIVER_LEASE_TIMEOUT_MS / 3);

    std::thread ping_thread([sleep_duration, failure_cb = std::move(failure_cb), ping_transport]() mutable {
      sunshine_driver::ControlClient client {*ping_transport};
      uint8_t fail_count = 0;
      std::uint64_t observed_generation = 0;
      {
        std::lock_guard<std::mutex> wake_lock(g_watchdog_wake_mutex);
        observed_generation = g_watchdog_wake_generation;
      }
      for (;;) {
        if (g_watchdog_stop_requested.load(std::memory_order_acquire)) {
          return;
        }

        const auto leases = driver_lease_tracker().all();
        const auto now_tp = std::chrono::steady_clock::now();
        bool should_feed = !leases.empty() || g_watchdog_feed_requested.load(std::memory_order_acquire);
        if (!should_feed && within_grace_period(now_tp)) {
          should_feed = true;
        }

        if (!should_feed) {
          if (wait_for_watchdog_state_change(sleep_duration, observed_generation)) {
            return;
          }
          continue;
        }

        bool feed_ok = true;
        for (const auto &lease : leases) {
          sunshine_driver::LeaseRequest feed {};
          feed.lease_id = lease.lease_id;
          feed.requested_timeout_ms = DRIVER_LEASE_TIMEOUT_MS;
          const auto fed = client.feed_lease(feed);
          if (!fed.ok()) {
            log_control_failure("Sunshine virtual display lease feed", fed.status, fed.native_error);
            feed_ok = false;
            break;
          }
        }

        if (!feed_ok) {
          fail_count += 1;
          if (fail_count > 3) {
            BOOST_LOG(error) << "Sunshine virtual display lease feed failed repeatedly; dispatching failure recovery.";
            dispatch_watchdog_fail_cb(failure_cb);
            return;
          }
        } else {
          fail_count = 0;
        }

        if (wait_for_watchdog_state_change(sleep_duration, observed_generation)) {
          return;
        }
      }
    });

    {
      std::lock_guard<std::mutex> lock(g_watchdog_thread_mutex);
      g_watchdog_transport = std::move(ping_transport);
      g_watchdog_thread = std::move(ping_thread);
    }

    return true;
  }

  bool startPingThread(std::function<void()> failCb) {
    return start_ping_thread_impl(
      std::move(failCb),
      {},
      OpenRecoveryBehavior::recover_driver
    );
  }

  void setWatchdogFeedingEnabled(bool enable) {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(g_watchdog_lifecycle_mutex);
    set_watchdog_feed_requested(enable, enable);
    if (!enable) {
      return;
    }

    const bool transport_ready = ensure_control_transport_responsive("Sunshine virtual display watchdog feed");
    if (!transport_ready) {
      BOOST_LOG(warning) << "Sunshine virtual display watchdog feed requested but driver transport is unavailable.";
      return;
    }

    // A stale transport reopen performs a full close, which clears the request.
    set_watchdog_feed_requested(true, true);
    (void) ensure_watchdog_thread_active_for_lease();
  }

  namespace {
    struct render_adapter_request_t {
      std::shared_ptr<sunshine_driver::WindowsControlTransport> transport;
      LUID luid {};
    };

    struct render_adapter_request_provenance_t {
      std::weak_ptr<sunshine_driver::WindowsControlTransport> transport;
      LUID luid {};
    };

    // Records the SetRenderAdapter request accepted by each control transport.
    // The driver keeps this request in memory only, so every transport replacement
    // must receive it again. This is request provenance, not observation of the
    // adapter later selected by IddCx for AssignSwapChain.
    std::mutex g_render_adapter_request_mutex;
    std::weak_ptr<sunshine_driver::WindowsControlTransport> g_current_render_adapter_request_transport;
    LUID g_current_render_adapter_request_luid {};
    std::unordered_map<std::string, render_adapter_request_provenance_t> g_render_adapter_request_provenance;

    bool luid_equal(const LUID &lhs, const LUID &rhs) {
      return lhs.LowPart == rhs.LowPart && lhs.HighPart == rhs.HighPart;
    }

    bool render_adapter_request_is_current(
      const std::shared_ptr<sunshine_driver::WindowsControlTransport> &transport,
      const LUID &adapter_luid
    ) {
      if (!transport) {
        return false;
      }
      std::lock_guard<std::mutex> lock(g_render_adapter_request_mutex);
      return g_current_render_adapter_request_transport.lock() == transport &&
             luid_equal(g_current_render_adapter_request_luid, adapter_luid);
    }

    std::optional<render_adapter_request_t> current_render_adapter_request() {
      const auto transport = control_transport_snapshot();
      if (!transport || !transport->valid()) {
        return std::nullopt;
      }
      std::lock_guard<std::mutex> lock(g_render_adapter_request_mutex);
      if (g_current_render_adapter_request_transport.lock() != transport) {
        return std::nullopt;
      }
      return render_adapter_request_t {
        .transport = transport,
        .luid = g_current_render_adapter_request_luid,
      };
    }

    bool render_adapter_request_matches(const render_adapter_request_t &expected) {
      const auto current = current_render_adapter_request();
      return current &&
             current->transport == expected.transport &&
             luid_equal(current->luid, expected.luid);
    }

    bool record_render_adapter_request_provenance(
      const uuid_util::uuid_t &guid,
      const render_adapter_request_t &creation_request
    ) {
      std::lock_guard<std::mutex> lock(g_render_adapter_request_mutex);
      if (g_current_render_adapter_request_transport.lock() != creation_request.transport ||
          !luid_equal(g_current_render_adapter_request_luid, creation_request.luid)) {
        BOOST_LOG(error) << "Cannot record Sunshine virtual display render-adapter request provenance for guid="
                         << guid.string() << " because the creation-time request is no longer current.";
        return false;
      }
      g_render_adapter_request_provenance[guid.string()] = render_adapter_request_provenance_t {
        .transport = creation_request.transport,
        .luid = creation_request.luid,
      };
      return true;
    }

    void erase_render_adapter_request_provenance(const uuid_util::uuid_t &guid) {
      std::lock_guard<std::mutex> lock(g_render_adapter_request_mutex);
      g_render_adapter_request_provenance.erase(guid.string());
    }

    bool render_adapter_request_provenance_matches(
      const uuid_util::uuid_t &guid,
      const std::string_view context
    ) {
      const auto transport = control_transport_snapshot();
      std::lock_guard<std::mutex> lock(g_render_adapter_request_mutex);
      const auto current_transport = g_current_render_adapter_request_transport.lock();
      const auto provenance = g_render_adapter_request_provenance.find(guid.string());
      const bool matches =
        transport &&
        current_transport == transport &&
        provenance != g_render_adapter_request_provenance.end() &&
        provenance->second.transport.lock() == transport &&
        luid_equal(provenance->second.luid, g_current_render_adapter_request_luid);
      if (!matches) {
        BOOST_LOG(error) << "Sunshine virtual display render-adapter request provenance is absent or stale for "
                         << context << " (guid=" << guid.string() << ").";
      }
      return matches;
    }

    bool render_adapter_request_provenance_matches(
      const uuid_util::uuid_t &guid,
      const LUID &requested_luid,
      const std::string_view context
    ) {
      const auto transport = control_transport_snapshot();
      std::lock_guard<std::mutex> lock(g_render_adapter_request_mutex);
      const auto provenance = g_render_adapter_request_provenance.find(guid.string());
      const bool matches =
        transport &&
        g_current_render_adapter_request_transport.lock() == transport &&
        luid_equal(g_current_render_adapter_request_luid, requested_luid) &&
        provenance != g_render_adapter_request_provenance.end() &&
        provenance->second.transport.lock() == transport &&
        luid_equal(provenance->second.luid, requested_luid);
      if (!matches) {
        BOOST_LOG(error)
          << "Sunshine virtual display render-adapter request provenance does not match the requested LUID for "
          << context << " (guid=" << guid.string()
          << "). SetRenderAdapter acceptance does not observe AssignSwapChain.";
      }
      return matches;
    }

    bool set_render_adapter_luid(const LUID &adapter_luid, const std::wstring &adapter_name, SIZE_T dedicated_memory, SIZE_T shared_memory) {
      std::lock_guard<std::recursive_mutex> operation_lock(g_virtual_display_operation_mutex);
      auto transport = control_transport_snapshot();
      if (!transport || !transport->valid()) {
        return false;
      }

      if (render_adapter_request_is_current(transport, adapter_luid)) {
        BOOST_LOG(debug) << "Sunshine virtual display render-adapter request for '"
                         << platf::to_utf8(adapter_name)
                         << "' is already current on this driver transport.";
        return true;
      }

      {
        // A failed or timed-out SetRenderAdapter is not transactional: the
        // driver may have changed its preferred LUID before reporting failure.
        // Invalidate the prior accepted-request record before issuing a
        // different request and restore it only after confirmed acceptance.
        std::lock_guard<std::mutex> lock(g_render_adapter_request_mutex);
        g_current_render_adapter_request_transport.reset();
        g_current_render_adapter_request_luid = {};
      }

      sunshine_driver::ControlClient client {*transport};
      sunshine_driver::SetRenderAdapterRequest request {};
      request.adapter_luid = sunshine_driver::from_windows_luid(adapter_luid);
      const auto result = client.set_render_adapter(request);
      if (!result.ok()) {
        BOOST_LOG(warning) << "Sunshine virtual display SetRenderAdapter request failed for '"
                           << platf::to_utf8(adapter_name)
                           << "' (status=" << sunshine_driver::to_string(result.status)
                           << ", native_error=" << result.native_error << ").";
        return false;
      }

      {
        std::lock_guard<std::mutex> lock(g_render_adapter_request_mutex);
        g_current_render_adapter_request_transport = transport;
        g_current_render_adapter_request_luid = adapter_luid;
      }

      const unsigned long long dedicated_mib = static_cast<unsigned long long>(dedicated_memory / (1024ull * 1024ull));
      const unsigned long long shared_mib = static_cast<unsigned long long>(shared_memory / (1024ull * 1024ull));
      BOOST_LOG(info) << "Sunshine virtual display SetRenderAdapter request accepted for '"
                      << platf::to_utf8(adapter_name)
                      << "' (dedicated=" << dedicated_mib
                      << " MiB, shared=" << shared_mib
                      << " MiB); this does not identify the adapter later used by AssignSwapChain.";
      return true;
    }
  }  // namespace

  bool renderAdapterRequestProvenanceMatches(
    const GUID &guid,
    const LUID &requested_luid,
    const std::string_view context
  ) {
    return render_adapter_request_provenance_matches(
      guid_to_uuid(guid),
      requested_luid,
      context
    );
  }

  bool setRenderAdapterByLuid(
    const LUID &adapter_luid,
    const std::wstring &adapter_name,
    const std::uint64_t dedicated_video_memory,
    const std::uint64_t shared_system_memory
  ) {
    return set_render_adapter_luid(
      adapter_luid,
      adapter_name,
      static_cast<SIZE_T>(dedicated_video_memory),
      static_cast<SIZE_T>(shared_system_memory)
    );
  }

  bool setRenderAdapterByName(const std::wstring &adapterName) {
    const auto transport = control_transport_snapshot();
    if (!transport || !transport->valid()) {
      return false;
    }

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
      return false;
    }

    for (UINT index = 0;; ++index) {
      Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
      const HRESULT enum_result = factory->EnumAdapters1(index, adapter.GetAddressOf());
      if (enum_result == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      if (FAILED(enum_result) || !adapter) {
        BOOST_LOG(warning) << "DXGI adapter enumeration failed while resolving the configured virtual-display adapter.";
        return false;
      }

      DXGI_ADAPTER_DESC1 desc {};
      if (FAILED(adapter->GetDesc1(&desc))) {
        BOOST_LOG(warning) << "DXGI adapter description query failed while resolving the configured virtual-display adapter.";
        return false;
      }
      if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
        continue;
      }
      if (std::wstring_view(desc.Description) != adapterName) {
        continue;
      }

      return set_render_adapter_luid(desc.AdapterLuid, desc.Description, desc.DedicatedVideoMemory, desc.SharedSystemMemory);
    }

    BOOST_LOG(warning) << "Sunshine virtual display render adapter named '"
                       << platf::to_utf8(adapterName)
                       << "' was not found.";
    return false;
  }

  bool setRenderAdapterWithMostDedicatedMemory() {
    const auto transport = control_transport_snapshot();
    if (!transport || !transport->valid()) {
      return false;
    }

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
      return false;
    }

    SIZE_T best_dedicated = 0;
    SIZE_T best_shared = 0;
    LUID best_luid {};
    std::wstring best_name;
    bool found = false;

    for (UINT index = 0;; ++index) {
      Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
      const HRESULT enum_result = factory->EnumAdapters1(index, adapter.GetAddressOf());
      if (enum_result == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      if (FAILED(enum_result) || !adapter) {
        BOOST_LOG(warning) << "DXGI adapter enumeration failed during virtual-display auto-selection.";
        return false;
      }

      DXGI_ADAPTER_DESC1 desc {};
      if (FAILED(adapter->GetDesc1(&desc))) {
        BOOST_LOG(warning) << "DXGI adapter description query failed during virtual-display auto-selection.";
        return false;
      }
      if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
        continue;
      }

      const SIZE_T dedicated = desc.DedicatedVideoMemory;
      const SIZE_T shared = desc.SharedSystemMemory;
      if (!found || dedicated > best_dedicated || (dedicated == best_dedicated && shared > best_shared)) {
        best_dedicated = dedicated;
        best_shared = shared;
        best_luid = desc.AdapterLuid;
        best_name.assign(desc.Description);
        found = true;
      }
    }

    if (!found) {
      return false;
    }

    return set_render_adapter_luid(best_luid, best_name, best_dedicated, best_shared);
  }

  bool apply_configured_render_adapter_preference(std::string_view context = "display ensure") {
    return VDISPLAY::applyConfiguredRenderAdapterPreference(context);
  }

  bool wait_for_virtual_display_ready(
    const std::optional<std::wstring> &display_name,
    std::optional<std::string> &device_id,
    uint32_t width,
    uint32_t height,
    const DisplayConfigIdentity *display_config_identity = nullptr,
    bool *confirmed_active = nullptr,
    std::stop_token stop_token = {}
  ) {
    if (stop_token.stop_requested()) {
      return false;
    }
    if (confirmed_active) {
      *confirmed_active = false;
    }
    std::optional<std::string> normalized_name;
    if (display_name && !display_name->empty()) {
      normalized_name = normalize_display_name(platf::to_utf8(*display_name));
    }

    std::optional<std::string> monitor_path_hint;
    std::optional<std::string> gdi_name_hint;
    std::optional<std::string> friendly_name_hint;
    if (display_config_identity) {
      if (display_config_identity->monitor_device_path && !display_config_identity->monitor_device_path->empty()) {
        monitor_path_hint = platf::to_utf8(*display_config_identity->monitor_device_path);
      }
      if (display_config_identity->source_gdi_device_name && !display_config_identity->source_gdi_device_name->empty()) {
        gdi_name_hint = normalize_display_name(platf::to_utf8(*display_config_identity->source_gdi_device_name));
      }
      if (display_config_identity->monitor_friendly_device_name && !display_config_identity->monitor_friendly_device_name->empty()) {
        friendly_name_hint = normalize_display_name(platf::to_utf8(*display_config_identity->monitor_friendly_device_name));
      }
    }

    const auto start = std::chrono::steady_clock::now();
    const auto requested_device_id = device_id;
    std::optional<std::chrono::steady_clock::time_point> enumerated_at;
    const auto enumeration_timeout = VDISPLAY::policy::enumeration_timeout;
    const auto poll_interval = VDISPLAY::policy::readiness_poll_interval;
    const bool has_dynamic_hints =
      (device_id && !device_id->empty()) || normalized_name || monitor_path_hint || gdi_name_hint || friendly_name_hint;
    const bool has_strong_identity_hints =
      (requested_device_id && !requested_device_id->empty()) || monitor_path_hint || gdi_name_hint;

    while (true) {
      if (stop_token.stop_requested()) {
        return false;
      }
      const auto now = std::chrono::steady_clock::now();
      if (!enumerated_at && now - start >= enumeration_timeout) {
        BOOST_LOG(warning) << "Timed out waiting for Windows to enumerate virtual display.";
        return false;
      }
      if (enumerated_at && VDISPLAY::policy::accept_enumerated_target(now - *enumerated_at)) {
        BOOST_LOG(debug) << "Virtual display was enumerated before final activation/mode details settled; continuing so the display helper can apply the session mode.";
        return true;
      }

      auto attempt_candidate = [&](const display_device::EnumeratedDevice &candidate, bool exact_target, bool adopt_identity) -> bool {
        if (adopt_identity && !candidate.m_device_id.empty()) {
          if (!device_id || !equals_ci(candidate.m_device_id, *device_id)) {
            device_id = candidate.m_device_id;
          }
        }

        if (!enumerated_at) {
          enumerated_at = now;
        }

        if (candidate.m_info && candidate.m_display_name.empty()) {
          if (exact_target) {
            BOOST_LOG(debug) << "Virtual display target is enumerated without a usable GDI name; continuing so the display helper can activate it.";
            return true;
          }
          return false;
        }

        if (candidate.m_info) {
          if (candidate.m_info->m_resolution.m_width == width &&
              candidate.m_info->m_resolution.m_height == height) {
            if (confirmed_active) {
              *confirmed_active = true;
            }
            return true;
          }

          if (exact_target) {
            if (confirmed_active) {
              *confirmed_active = true;
            }
            BOOST_LOG(debug) << "Virtual display target is enumerated and active; continuing immediately so the display helper can apply the requested mode.";
            return true;
          }

          if (confirmed_active) {
            *confirmed_active = true;
          }

          BOOST_LOG(debug) << "Virtual display candidate "
                           << (candidate.m_display_name.empty() ? candidate.m_device_id : candidate.m_display_name)
                           << " is active at " << candidate.m_info->m_resolution.m_width << 'x'
                           << candidate.m_info->m_resolution.m_height << "; waiting for "
                           << width << 'x' << height << '.';
          if (enumerated_at && VDISPLAY::policy::accept_enumerated_target(now - *enumerated_at)) {
            BOOST_LOG(debug) << "Virtual display is active before the requested mode settled; continuing so the display helper can apply the session mode.";
            return true;
          }
          return false;
        }

        if (exact_target) {
          BOOST_LOG(debug) << "Virtual display target is enumerated but not active yet; continuing immediately so the display helper can activate it.";
          return true;
        }

        if (enumerated_at && VDISPLAY::policy::accept_enumerated_target(now - *enumerated_at)) {
          BOOST_LOG(debug) << "Virtual display is enumerated but not active yet; continuing so the display helper can apply the session mode.";
          return true;
        }

        return false;
      };

      auto devices = platf::display_helper::Coordinator::instance().enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
      if (devices) {
        std::optional<display_device::EnumeratedDevice> unique_resolution_candidate;
        std::optional<display_device::EnumeratedDevice> unique_weak_identity_candidate;
        bool resolution_conflict = false;
        bool weak_identity_conflict = false;

        for (const auto &candidate : *devices) {
          const bool is_virtual = is_virtual_display_device(candidate);
          if (!has_dynamic_hints && !is_virtual) {
            continue;
          }

          if (is_virtual && candidate.m_info && candidate.m_info->m_resolution.m_width == width &&
              candidate.m_info->m_resolution.m_height == height) {
            if (!resolution_conflict) {
              if (!unique_resolution_candidate) {
                unique_resolution_candidate = candidate;
              } else {
                resolution_conflict = true;
                unique_resolution_candidate.reset();
              }
            }
          }

          bool strong_identity_match = false;
          bool weak_identity_match = false;
          if (requested_device_id && !requested_device_id->empty() && !candidate.m_device_id.empty()) {
            strong_identity_match = equals_ci(candidate.m_device_id, *requested_device_id);
          }

          const auto candidate_display_name = !candidate.m_display_name.empty() ? std::make_optional(normalize_display_name(candidate.m_display_name)) : std::nullopt;
          const auto candidate_friendly_name = !candidate.m_friendly_name.empty() ? std::make_optional(normalize_display_name(candidate.m_friendly_name)) : std::nullopt;

          if (!strong_identity_match && monitor_path_hint && !candidate.m_monitor_device_path.empty()) {
            strong_identity_match = equals_ci(candidate.m_monitor_device_path, *monitor_path_hint);
          }

          if (!strong_identity_match && gdi_name_hint) {
            if (candidate_display_name && *candidate_display_name == *gdi_name_hint) {
              strong_identity_match = true;
            }
          }

          if (!strong_identity_match && friendly_name_hint) {
            if (candidate_friendly_name && *candidate_friendly_name == *friendly_name_hint) {
              weak_identity_match = true;
            }
          }

          if (!strong_identity_match && normalized_name) {
            if (!candidate.m_display_name.empty() &&
                candidate_display_name && *candidate_display_name == *normalized_name) {
              strong_identity_match = true;
            } else if (!candidate.m_friendly_name.empty() &&
                       candidate_friendly_name && *candidate_friendly_name == *normalized_name) {
              weak_identity_match = true;
            }
          }

          if (!strong_identity_match && weak_identity_match) {
            if (has_strong_identity_hints || !is_virtual) {
              continue;
            }
            if (!weak_identity_conflict) {
              if (!unique_weak_identity_candidate) {
                unique_weak_identity_candidate = candidate;
              } else {
                weak_identity_conflict = true;
                unique_weak_identity_candidate.reset();
              }
            }
            continue;
          }

          if (!strong_identity_match && has_dynamic_hints) {
            continue;
          }

          if (!has_dynamic_hints) {
            // No identity is available, so defer selection until the complete
            // enumeration proves there is exactly one virtual resolution match.
            continue;
          }

          if (attempt_candidate(candidate, strong_identity_match, strong_identity_match || !has_dynamic_hints)) {
            return true;
          }
        }

        if (!has_strong_identity_hints && !weak_identity_conflict && unique_weak_identity_candidate) {
          if (attempt_candidate(*unique_weak_identity_candidate, false, true)) {
            return true;
          }
        }

        if (!has_strong_identity_hints && !resolution_conflict && unique_resolution_candidate) {
          if (attempt_candidate(*unique_resolution_candidate, false, true)) {
            return true;
          }
        }
      }

      if (wait_for_monitor_stop(stop_token, poll_interval)) {
        return false;
      }
    }
  }

  bool wait_for_virtual_display_teardown(
    const std::wstring &display_name,
    std::chrono::steady_clock::duration timeout,
    std::stop_token stop_token = {}
  ) {
    if (display_name.empty()) {
      return true;
    }

    const auto normalized = normalize_display_name(platf::to_utf8(display_name));
    if (normalized.empty()) {
      return true;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (stop_token.stop_requested()) {
        return false;
      }
      bool present = false;
      if (auto devices = platf::display_helper::Coordinator::instance().enumerate_devices(display_device::DeviceEnumerationDetail::Minimal)) {
        for (const auto &device : *devices) {
          if (!is_virtual_display_device(device)) {
            continue;
          }

          const auto device_name = normalize_display_name(device.m_display_name);
          const auto friendly_name = normalize_display_name(device.m_friendly_name);
          if ((!device_name.empty() && device_name == normalized) ||
              (!friendly_name.empty() && friendly_name == normalized)) {
            present = true;
            break;
          }
        }
      }

      if (!present) {
        return true;
      }

      if (wait_for_monitor_stop(stop_token, std::chrono::milliseconds(100))) {
        return false;
      }
    }

    return false;
  }

  namespace {

    constexpr auto VIRTUAL_DISPLAY_STABILITY_RECHECK_DELAY = std::chrono::milliseconds(125);

    enum class ExistingVirtualDisplayPresence {
      missing,
      inactive,
      active,
    };

    ExistingVirtualDisplayPresence existing_virtual_display_presence(const std::uint64_t display_id) {
      const auto devices = platf::display_helper::Coordinator::instance().enumerate_devices(
        display_device::DeviceEnumerationDetail::Minimal
      );
      if (!devices) {
        return ExistingVirtualDisplayPresence::missing;
      }

      for (const auto &device : *devices) {
        if (!is_virtual_display_device(device) ||
            !matches_virtual_display_id_edid(device, display_id)) {
          continue;
        }
        return device.m_info ?
                 ExistingVirtualDisplayPresence::active :
                 ExistingVirtualDisplayPresence::inactive;
      }
      return ExistingVirtualDisplayPresence::missing;
    }

    std::optional<std::string> existing_virtual_display_device_id(const std::uint64_t display_id) {
      const auto devices = platf::display_helper::Coordinator::instance().enumerate_devices(
        display_device::DeviceEnumerationDetail::Minimal
      );
      if (!devices) {
        return std::nullopt;
      }
      for (const auto &device : *devices) {
        if (is_virtual_display_device(device) &&
            matches_virtual_display_id_edid(device, display_id) &&
            !device.m_device_id.empty()) {
          return device.m_device_id;
        }
      }
      return std::nullopt;
    }

    bool existing_virtual_display_supports_refresh(
      const std::uint64_t display_id,
      const std::uint32_t requested_refresh_millihz
    ) {
      if (requested_refresh_millihz == 0) {
        return false;
      }
      const auto devices = platf::display_helper::Coordinator::instance().enumerate_devices(
        display_device::DeviceEnumerationDetail::Full
      );
      if (!devices) {
        return false;
      }
      for (const auto &device : *devices) {
        if (!is_virtual_display_device(device) ||
            !matches_virtual_display_id_edid(device, display_id)) {
          continue;
        }
        return std::any_of(
          device.m_supported_refresh_rates.begin(),
          device.m_supported_refresh_rates.end(),
          [&](const display_device::Rational &refresh) {
            if (refresh.m_denominator == 0) {
              return false;
            }
            const auto rounded_millihz =
              (static_cast<std::uint64_t>(refresh.m_numerator) * 1000ull + refresh.m_denominator / 2ull) /
              refresh.m_denominator;
            const auto requested = static_cast<std::uint64_t>(requested_refresh_millihz);
            return rounded_millihz <= std::numeric_limits<std::uint32_t>::max() &&
                   VDISPLAY::policy::refresh_matches(
                     static_cast<std::uint32_t>(rounded_millihz),
                     static_cast<std::uint32_t>(requested)
                   );
          }
        );
      }
      return false;
    }

    bool is_virtual_display_present(
      const std::optional<std::wstring> &display_name,
      const std::optional<std::string> &device_id
    ) {
      auto devices = platf::display_helper::Coordinator::instance().enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
      if (!devices) {
        return false;
      }

      std::optional<std::string> normalized_name;
      if (display_name && !display_name->empty()) {
        normalized_name = normalize_display_name(platf::to_utf8(*display_name));
      }

      for (const auto &device : *devices) {
        if (!is_virtual_display_device(device)) {
          continue;
        }

        bool matches = false;
        if (device_id && !device_id->empty() && !device.m_device_id.empty()) {
          matches = equals_ci(device.m_device_id, *device_id);
        }

        if (!matches && normalized_name) {
          const auto device_name = normalize_display_name(device.m_display_name);
          const auto friendly_name = normalize_display_name(device.m_friendly_name);
          if ((!device_name.empty() && device_name == *normalized_name) ||
              (!friendly_name.empty() && friendly_name == *normalized_name)) {
            matches = true;
          }
        }

        if (!matches && !device_id && !normalized_name) {
          matches = true;
        }

        if (matches) {
          return true;
        }
      }

      return false;
    }

    bool confirm_virtual_display_persistence(
      const VirtualDisplayCreationResult &result,
      uint32_t width,
      uint32_t height,
      std::stop_token stop_token = {}
    ) {
      (void) width;
      (void) height;

      const auto name_utf8 = result.display_name ? platf::to_utf8(*result.display_name) : std::string("(pending)");
      const auto device_utf8 = result.device_id ? *result.device_id : std::string("(unknown)");
      const auto delay_ms = std::chrono::duration_cast<std::chrono::milliseconds>(VIRTUAL_DISPLAY_STABILITY_RECHECK_DELAY).count();

      if (stop_token.stop_requested() || !is_virtual_display_present(result.display_name, result.device_id)) {
        BOOST_LOG(warning) << "Virtual display '" << name_utf8 << "' device_id='" << device_utf8
                           << "' missing immediately after creation.";
        return false;
      }

      if (wait_for_monitor_stop(stop_token, VIRTUAL_DISPLAY_STABILITY_RECHECK_DELAY)) {
        return false;
      }

      if (stop_token.stop_requested() || !is_virtual_display_present(result.display_name, result.device_id)) {
        BOOST_LOG(warning) << "Virtual display '" << name_utf8 << "' device_id='" << device_utf8
                           << "' disappeared within " << delay_ms << "ms of confirmation.";
        return false;
      }

      BOOST_LOG(debug) << "Virtual display '" << name_utf8 << "' device_id='" << device_utf8
                       << "' remained present after " << delay_ms << "ms stability recheck.";
      return true;
    }

    bool is_gdi_display_name(const std::wstring &name) {
      return name.size() >= 4 && name[0] == L'\\' && name[1] == L'\\' && name[2] == L'.' && name[3] == L'\\';
    }

    std::optional<VirtualDisplayCreationResult> create_virtual_display_once(
      const char *s_hdr_profile,
      const char *s_client_uid,
      const char *s_client_name,
      uint32_t width,
      uint32_t height,
      uint32_t fps,
      const GUID &guid,
      uint32_t base_fps_millihz,
      bool framegen_refresh_active,
      int framegen_refresh_multiplier,
      bool hdr_requested,
      bool allow_pending_enumeration,
      bool replace_existing,
      bool &allow_driver_recovery,
      std::stop_token stop_token = {}
    ) {
      if (stop_token.stop_requested()) {
        return std::nullopt;
      }
      auto transport = control_transport_snapshot();
      if (!transport || !transport->valid()) {
        return std::nullopt;
      }

      uuid_util::uuid_t requested_uuid {};
      std::memcpy(requested_uuid.b8, &guid, sizeof(requested_uuid.b8));
      const std::optional<std::string> deferred_hdr_profile_worker_key =
        stop_token.stop_possible() ? std::make_optional(requested_uuid.string()) : std::nullopt;

      // Log entry and inputs for deeper diagnostics
      BOOST_LOG(debug) << "createVirtualDisplay called: client_uid='" << (s_client_uid ? s_client_uid : "(null)")
                       << "' client_name='" << (s_client_name ? s_client_name : "(null)")
                       << "' hdr_profile='" << (s_hdr_profile ? s_hdr_profile : "(null)")
                       << "' width=" << width << " height=" << height << " fps=" << fps
                       << " hdr_requested=" << hdr_requested
                       << " guid=" << requested_uuid.string();

      if (!teardown_conflicting_virtual_displays(requested_uuid, stop_token)) {
        return std::nullopt;
      }
      BOOST_LOG(debug) << "teardown_conflicting_virtual_displays completed for guid=" << requested_uuid.string();
      if (!enforce_teardown_cooldown_if_needed(stop_token)) {
        return std::nullopt;
      }

      // Conflict teardown may have reopened the global control handle. Refresh
      // this operation's shared snapshot before issuing any driver requests.
      transport = control_transport_snapshot();
      if (!transport || !transport->valid()) {
        return std::nullopt;
      }
      // A reopened handle means the caller submitted the configured render-adapter request
      // on the previous transport, while a restarted driver returns to its default request.
      // Submit it again on the refreshed transport before creating the display below. This
      // is a no-op when teardown left the transport alone.
      if (!VDISPLAY::policy::adapter_preference_allows_creation(
            apply_configured_render_adapter_preference("virtual display creation (post-teardown)")
          )) {
        return std::nullopt;
      }
      const auto creation_render_request = current_render_adapter_request();
      if (!creation_render_request || creation_render_request->transport != transport) {
        BOOST_LOG(error) << "Sunshine virtual display render-adapter request changed before creation.";
        return std::nullopt;
      }
      sunshine_driver::ControlClient client {*transport};
      if (config::video.dd.virtual_display_permanent_count_configured &&
          !set_permanent_display_count(
            client,
            static_cast<std::uint32_t>(std::clamp(
              config::video.dd.virtual_display_permanent_count,
              0,
              config::SUNSHINE_VIRTUAL_DISPLAY_MAX_PERMANENT_COUNT
            ))
          )) {
        BOOST_LOG(warning) << "Unable to apply configured Sunshine virtual display permanent count before creating temporary display.";
      }

      const uint32_t requested_fps = apply_refresh_overrides(fps, base_fps_millihz, framegen_refresh_active ? framegen_refresh_multiplier : 1);
      // The driver derives its complete mode catalog from the descriptor timing. Keep the
      // descriptor at the client/base rate so it can advertise both that mode and 4x; the
      // display helper selects requested_fps after the monitor arrives when a fixed refresh
      // policy needs to start above the base rate.
      const uint32_t descriptor_fps = base_fps_millihz > 0 ? base_fps_millihz : requested_fps;
      const auto display_id = client_uuid_to_virtual_display_id(guid);
      const auto secure_reclaim_supported = driver_supports_secure_reclaim(client);
      if (!secure_reclaim_supported) {
        return std::nullopt;
      }
      if (!*secure_reclaim_supported) {
        BOOST_LOG(error) << "Sunshine temporary displays require virtual display control protocol "
                         << sunshine_driver::kProtocolVersionMajor << '.'
                         << SECURE_RECLAIM_DRIVER_PROTOCOL_MINOR
                         << "+ so ownership can be recovered safely after restart.";
        allow_driver_recovery = false;
        return std::nullopt;
      }

      std::vector<VirtualDisplayRecoveryEntry> recovery_entries;
      std::optional<sunshine_driver::OwnerCapability> owner_capability;
      bool reclaimed_for_reuse = false;
      if (*secure_reclaim_supported) {
        if (!load_virtual_display_recovery_journal_for_driver(client, recovery_entries)) {
          allow_driver_recovery = false;
          return std::nullopt;
        }

        if (auto existing = find_virtual_display_recovery_entry(recovery_entries, requested_uuid);
            existing != recovery_entries.end()) {
          const auto existing_capability = existing->owner_capability;
          const auto reclaimed = reclaim_virtual_display_recovery_entry(
            client,
            recovery_entries,
            requested_uuid,
            std::nullopt,
            std::nullopt,
            std::nullopt
          );
          if (reclaimed == VirtualDisplayReclaimResult::failed) {
            allow_driver_recovery = false;
            return std::nullopt;
          }
          if (reclaimed == VirtualDisplayReclaimResult::reclaimed) {
            const bool reusable_with_current_provenance =
              render_adapter_request_provenance_matches(
                requested_uuid,
                "securely reclaimed Sunshine virtual display"
              );
            const auto existing_presence = existing_virtual_display_presence(display_id);
            // REVERT can leave the retained target present but inactive. Departing
            // that PnP monitor and immediately arriving the same stable connector
            // is precisely the Windows enumeration wedge reproduced by #397.
            // Reclaim it in place and let the session APPLY reactivate it. Its
            // driver display_id, EDID product/serial, and connector stay stable;
            // the helper still applies the new session resolution and refresh.
            const bool requested_refresh_available =
              existing_presence == ExistingVirtualDisplayPresence::inactive &&
              existing_virtual_display_supports_refresh(display_id, requested_fps);
            const bool resurrect_inactive =
              replace_existing &&
              existing_presence == ExistingVirtualDisplayPresence::inactive &&
              requested_refresh_available;
            const auto reclaimed_plan =
              VDISPLAY::policy::reclaimed_display_plan_for_session(
                replace_existing,
                existing_presence == ExistingVirtualDisplayPresence::inactive,
                requested_refresh_available,
                reusable_with_current_provenance
              );
            if (replace_existing &&
                existing_presence == ExistingVirtualDisplayPresence::inactive &&
                !requested_refresh_available) {
              BOOST_LOG(info) << "Inactive Sunshine virtual display does not advertise the requested "
                              << requested_fps << " mHz refresh; recreating its mode descriptor"
                              << " (guid=" << requested_uuid.string()
                              << ", display_id=" << display_id << ").";
            }
            if (reclaimed_plan.action == VDISPLAY::policy::reclaimed_display_action::recreate) {
              BOOST_LOG(info)
                << "Securely releasing the prior owned virtual display before "
                << (replace_existing ? "replacement" : "render-adapter-safe recreation")
                << " (guid=" << requested_uuid.string() << ").";
              if (!remove_virtual_display_impl(guid, false, stop_token)) {
                allow_driver_recovery = false;
                return std::nullopt;
              }
              // Removal may reopen the transport. Return to the outer bounded
              // retry so adapter selection is applied again immediately before
              // the replacement create on the final transport.
              return std::nullopt;
            } else {
              if (resurrect_inactive) {
                BOOST_LOG(info) << "Securely reclaimed inactive Sunshine virtual display in place for session reactivation"
                                << " (guid=" << requested_uuid.string()
                                << ", display_id=" << display_id << ").";
              }
              owner_capability = existing_capability;
              reclaimed_for_reuse = true;
            }
          }
        }

        if (!reclaimed_for_reuse) {
          const auto exact_display_exists = driver_has_temporary_display(client, display_id);
          if (!exact_display_exists) {
            return std::nullopt;
          }
          if (*exact_display_exists) {
            BOOST_LOG(warning) << "Refusing to claim existing Sunshine virtual display guid="
                               << requested_uuid.string() << " display_id=" << display_id
                               << " because no matching protected owner capability is available.";
            allow_driver_recovery = false;
            return std::nullopt;
          }

          owner_capability = generate_owner_capability();
          if (!owner_capability ||
              !upsert_virtual_display_recovery_entry(
                recovery_entries,
                VirtualDisplayRecoveryEntry {
                  requested_uuid,
                  display_id,
                  *owner_capability,
                  VirtualDisplayRecoveryPhase::prepared
                }
              )) {
            BOOST_LOG(error) << "Refusing to create virtual display without durable protected recovery state for guid="
                             << requested_uuid.string() << '.';
            allow_driver_recovery = false;
            return std::nullopt;
          }
        }
      }

      const auto lease_id = generate_driver_lease_id();
      if (!lease_id) {
        allow_driver_recovery = false;
        return std::nullopt;
      }
      const auto dpi_settings_prefix = virtual_display_dpi_settings_prefix(display_id);
      const auto configured_scale = config::video.dd.virtual_display_scale_percent;
      const auto effective_scale = VDISPLAY::effective_virtual_display_scale_percent(
        configured_scale,
        width,
        height
      );
      const auto dpi_snapshot = configured_scale == 0 ?
        read_virtual_display_dpi_value(dpi_settings_prefix) :
        std::nullopt;

      sunshine_driver::CreateTemporaryDisplayRequest create_request {};
      create_request.lease_id = *lease_id;
      create_request.display_id = display_id;
      create_request.width = width;
      create_request.height = height;
      if (effective_scale > 0) {
        const auto dpi = 96.0 * static_cast<double>(effective_scale) / 100.0;
        create_request.physical_width_mm = std::clamp(
          static_cast<std::uint32_t>(std::lround(static_cast<double>(width) * 25.4 / dpi)),
          sunshine_driver::kMinPhysicalSizeMillimeters,
          sunshine_driver::kMaxPhysicalSizeMillimeters
        );
        create_request.physical_height_mm = std::clamp(
          static_cast<std::uint32_t>(std::lround(static_cast<double>(height) * 25.4 / dpi)),
          sunshine_driver::kMinPhysicalSizeMillimeters,
          sunshine_driver::kMaxPhysicalSizeMillimeters
        );
      }
      create_request.refresh_rate_millihz = descriptor_fps;
      create_request.requested_timeout_ms = DRIVER_LEASE_TIMEOUT_MS;
      create_request.hdr_max_luminance_nits = static_cast<std::uint32_t>(
        std::clamp(config::video.rtx_hdr.peak_brightness, 400, 2000)
      );
      const auto temporary_display_name = make_temporary_display_name(s_client_name);
      std::memcpy(create_request.display_name, temporary_display_name.data(), temporary_display_name.size());

      BOOST_LOG(debug) << "Calling Sunshine temporary display create (driver transport present, display_id="
                       << display_id
                       << ", HDR peak=" << create_request.hdr_max_luminance_nits << " nits"
                       << ", scale=" << effective_scale << "%"
                       << ", physical=" << create_request.physical_width_mm << 'x'
                       << create_request.physical_height_mm << " mm).";
      sunshine_driver::ControlResult<sunshine_driver::CreateTemporaryDisplayResult> create_result;
      if (reclaimed_for_reuse) {
        // Ownership is already established in-place. Enter the existing-display
        // completion path without issuing a create that is guaranteed to collide.
        create_result.status = sunshine_driver::ControlStatus::TransportFailed;
        create_result.native_error = ERROR_SUCCESS;
      } else {
        sunshine_driver::CreateTemporaryDisplayOwnedRequest owned_request {};
        owned_request.display = create_request;
        owned_request.owner_capability = *owner_capability;
        create_result = client.create_temporary_display_owned(owned_request);
      }
      if (!create_result.ok()) {
        const DWORD error_code = create_result.native_error;
        if (!reclaimed_for_reuse &&
            (error_code == ERROR_BUSY ||
             error_code == ERROR_INVALID_PARAMETER ||
             error_code == ERROR_NOT_SUPPORTED ||
             error_code == ERROR_NOT_ENOUGH_MEMORY ||
             error_code == ERROR_NO_SYSTEM_RESOURCES)) {
          allow_driver_recovery = false;
          if (!erase_virtual_display_recovery_entry(recovery_entries, requested_uuid)) {
            BOOST_LOG(warning) << "Unable to discard unused virtual display recovery marker after a non-retryable create failure for guid="
                               << requested_uuid.string() << '.';
          }
        }
        if (reclaimed_for_reuse) {
          BOOST_LOG(debug) << "Completing securely reclaimed Sunshine virtual display reuse for guid="
                           << requested_uuid.string() << " display_id=" << display_id << '.';
        } else {
          BOOST_LOG(warning) << "Sunshine temporary display create failed: status="
                             << sunshine_driver::to_string(create_result.status)
                             << " error=" << error_code
                             << " guid=" << requested_uuid.string() << " display_id=" << display_id;
        }

        if (replace_existing && !reclaimed_for_reuse) {
          BOOST_LOG(warning) << "Sunshine temporary display create could not safely replace existing state for guid="
                             << requested_uuid.string() << "; no unowned display was evicted.";
          return std::nullopt;
        }

        auto reuse_name = resolve_virtual_display_name_from_devices_for_client(s_client_name);
        std::optional<std::string> device_id;
        if (reuse_name) {
          device_id = resolveVirtualDisplayDeviceId(*reuse_name);
          BOOST_LOG(debug) << "resolveVirtualDisplayDeviceId(" << platf::to_utf8(*reuse_name) << ") returned '"
                           << (device_id ? *device_id : std::string("(none)")) << "'";
        }
        if (!device_id) {
          if (s_client_name && std::strlen(s_client_name) > 0) {
            device_id = resolveVirtualDisplayDeviceIdForClient(s_client_name);
          }
        }
        if (!device_id && reclaimed_for_reuse) {
          // Inactive devices have no GDI name, and Windows may omit the friendly
          // name used by the client resolver. The stable EDID product/serial is
          // still present and is the authoritative identity for resurrection.
          device_id = existing_virtual_display_device_id(display_id);
        }

        if (dpi_snapshot) {
          (void) apply_virtual_display_dpi_value(*dpi_snapshot);
        }

        if (reuse_name || device_id) {
          BOOST_LOG(debug) << "Waiting for virtual display ready (reuse). display_name='"
                           << (reuse_name ? platf::to_utf8(*reuse_name) : std::string("(none)"))
                           << "' device_id='" << (device_id ? *device_id : std::string("(none)")) << "'";
          std::optional<std::wstring> display_name = reuse_name;
          bool confirmed_active = false;
          if (wait_for_virtual_display_ready(display_name, device_id, width, height, nullptr, &confirmed_active, stop_token)) {
            if (display_name) {
              if (reclaimed_for_reuse) {
                wprintf(L"[SunshineVirtualDisplay] Reusing securely reclaimed virtual display: %ls\n", display_name->c_str());
              } else {
                wprintf(
                  L"[SunshineVirtualDisplay] Reusing existing virtual display (error=%lu): %ls\n",
                  static_cast<unsigned long>(error_code),
                  display_name->c_str()
                );
              }
            } else {
              if (reclaimed_for_reuse) {
                printf("[SunshineVirtualDisplay] Reusing securely reclaimed virtual display.\n");
              } else {
                printf("[SunshineVirtualDisplay] Reusing existing virtual display (error=%lu).\n", static_cast<unsigned long>(error_code));
              }
            }

            BOOST_LOG(info) << "Reused virtual display for guid=" << requested_uuid.string()
                            << " display_name='"
                            << (display_name ? platf::to_utf8(*display_name) : std::string("(none)")) << "' device_id='"
                            << (device_id ? *device_id : std::string("(none)")) << "'";

            const auto ready_since = std::chrono::steady_clock::now();
            VirtualDisplayCreationResult result;
            result.display_name = display_name;
            if (device_id && !device_id->empty()) {
              result.device_id = *device_id;
            }
            if (s_client_name && std::strlen(s_client_name) > 0) {
              result.client_name = std::string(s_client_name);
            }

            // Prefer a real GDI display name (\\.\DISPLAYx) over a GUID-like placeholder when available.
            if ((!result.display_name || result.display_name->empty() || !is_gdi_display_name(*result.display_name))) {
              auto gdi_name = resolve_virtual_display_name_from_devices_for_client(s_client_name);
              if (!gdi_name && (!s_client_name || std::strlen(s_client_name) == 0)) {
                gdi_name = resolve_virtual_display_name_from_devices();
              }
              if (gdi_name) {
                if (!gdi_name->empty() && is_gdi_display_name(*gdi_name)) {
                  BOOST_LOG(debug) << "Virtual display: resolved GDI name '" << platf::to_utf8(*gdi_name) << "' after reuse.";
                  result.display_name = gdi_name;
                }
              }
            }

            if (!reclaimed_for_reuse ||
                !render_adapter_request_provenance_matches(
                  requested_uuid,
                  "existing Sunshine virtual display reuse"
                )) {
              if (reclaimed_for_reuse) {
                BOOST_LOG(warning) << "Replacing the securely reclaimed Sunshine virtual display because its in-process render-adapter request provenance became stale.";
                if (!remove_virtual_display_impl(guid, false, stop_token)) {
                  BOOST_LOG(error) << "Failed to remove the securely owned stale-provenance Sunshine virtual display.";
                  allow_driver_recovery = false;
                }
              } else {
                BOOST_LOG(error) << "Refusing to reuse a Sunshine virtual display without proven ownership and matching in-process render-adapter request provenance.";
                allow_driver_recovery = false;
              }
              return std::nullopt;
            }

            if ((display_name && !display_name->empty()) || (result.device_id && !result.device_id->empty())) {
              result.monitor_device_path = resolve_monitor_device_path(
                display_name,
                result.device_id,
                5,
                std::chrono::milliseconds(100),
                std::nullopt,
                stop_token
              );
            } else {
              BOOST_LOG(debug) << "Sunshine virtual display reuse has no target identity; skipping monitor-path resolution to avoid the physical-primary fallback.";
            }
            if (stop_token.stop_requested()) {
              return std::nullopt;
            }
            result.reused_existing = true;
            result.confirmed_active = confirmed_active;
            if (confirmed_active) {
              result.ready_since = ready_since;
            }
            if (!adopt_existing_driver_lease(client, requested_uuid, display_id, result.display_name, result.device_id, result.monitor_device_path)) {
              BOOST_LOG(warning) << "Refusing to reuse existing Sunshine virtual display for guid="
                                 << requested_uuid.string() << " because its driver lease could not be adopted.";
              return std::nullopt;
            }
            if (stop_token.stop_requested()) {
              return std::nullopt;
            }
            if (dpi_snapshot) {
              (void) apply_virtual_display_dpi_value(*dpi_snapshot);
            }
            std::optional<std::string> hdr_profile;
            if (s_hdr_profile && std::strlen(s_hdr_profile) > 0) {
              hdr_profile = std::string(s_hdr_profile);
            }
            apply_hdr_profile_if_available(
              result.display_name,
              result.device_id,
              result.monitor_device_path,
              result.client_name,
              hdr_profile,
              true,
              true,
              stop_token,
              deferred_hdr_profile_worker_key
            );
            if (stop_token.stop_requested()) {
              return std::nullopt;
            }
            return result;
          }
        }

        printf("[SunshineVirtualDisplay] Failed to add virtual display (status=%s, error=%lu).\n",
               sunshine_driver::to_string(create_result.status),
               static_cast<unsigned long>(error_code));
        return std::nullopt;
      }

      // Publish the exact lease returned by the successful create before any
      // fallible validation. Rollback must be able to prove ownership even if
      // render provenance or Windows enumeration fails immediately afterward.
      driver_lease_tracker().put(
        requested_uuid,
        DriverLeaseInfo {
          create_result.value.display_id != 0 ? create_result.value.display_id : display_id,
          create_result.value.lease_id != 0 ? create_result.value.lease_id : *lease_id,
          std::nullopt,
          std::nullopt,
          std::nullopt
        }
      );

      const auto rollback_created_display = [&] {
        if (!remove_virtual_display_impl(guid, false, stop_token)) {
          BOOST_LOG(warning) << "Virtual display creation cleanup failed for guid=" << requested_uuid.string() << '.';
        }
      };
      if (stop_token.stop_requested()) {
        rollback_created_display();
        return std::nullopt;
      }
      if (!render_adapter_request_matches(*creation_render_request)) {
        BOOST_LOG(error) << "Sunshine virtual display driver transport or render-adapter request changed during creation.";
        rollback_created_display();
        return std::nullopt;
      }
      if (!record_render_adapter_request_provenance(
            requested_uuid,
            *creation_render_request)) {
        rollback_created_display();
        return std::nullopt;
      }

      const DisplayConfigTarget output {
        sunshine_driver::to_windows_luid(create_result.value.os_adapter_luid),
        create_result.value.target_id
      };

      if (!ensure_watchdog_thread_active_for_lease(stop_token) || stop_token.stop_requested()) {
        rollback_created_display();
        return std::nullopt;
      }

      // Driver arrival only makes the target available. Activating it by the
      // driver's low-level identity is what lets names and mode data resolve,
      // which libdisplaydevice intentionally omits for inactive targets. This is
      // a best-effort accelerator, never a precondition: when it fails we keep the
      // display the driver already accepted and fall back to the enumeration and
      // name paths, and ultimately to the display helper's own APPLY.
      bool exact_target_activated = activate_display_config_target(output, width, height, requested_fps, stop_token);
      if (!exact_target_activated && !stop_token.stop_requested()) {
        BOOST_LOG(warning) << "Sunshine temporary display did not reach its exact active-topology postcondition; continuing with enumeration-based readiness for guid="
                           << requested_uuid.string() << '.';
      }
      if (stop_token.stop_requested()) {
        rollback_created_display();
        return std::nullopt;
      }

      if (*secure_reclaim_supported) {
        if (auto recovery = find_virtual_display_recovery_entry(recovery_entries, requested_uuid);
            recovery != recovery_entries.end()) {
          recovery->phase = VirtualDisplayRecoveryPhase::active;
          if (!save_virtual_display_recovery_journal(recovery_entries)) {
            BOOST_LOG(warning) << "Virtual display was activated with a durable prepared recovery marker, but it could not be promoted for guid="
                               << requested_uuid.string() << '.';
          }
        }
      }

      auto display_config_identity = wait_for_display_config_identity(
        output,
        std::chrono::milliseconds(250),
        stop_token
      );
      if (stop_token.stop_requested()) {
        rollback_created_display();
        return std::nullopt;
      }

      std::optional<std::wstring> resolved_display_name;
      if (display_config_identity) {
        resolved_display_name = display_name_from_identity(*display_config_identity);
      }

      if (!resolved_display_name) {
        resolved_display_name = resolve_virtual_display_name_from_devices_for_client(s_client_name);
        if (!resolved_display_name && (!s_client_name || std::strlen(s_client_name) == 0)) {
          resolved_display_name = resolve_virtual_display_name_from_devices();
        }
      }

      std::optional<std::string> device_id;
      if (resolved_display_name) {
        device_id = resolveVirtualDisplayDeviceId(*resolved_display_name);
        BOOST_LOG(debug) << "resolveVirtualDisplayDeviceId(" << platf::to_utf8(*resolved_display_name) << ") returned '"
                         << (device_id ? *device_id : std::string("(none)")) << "'";
      }
      if (!device_id) {
        if (s_client_name && std::strlen(s_client_name) > 0) {
          device_id = resolveVirtualDisplayDeviceIdForClient(s_client_name);
        }
        if (!device_id && (!s_client_name || std::strlen(s_client_name) == 0)) {
          device_id = resolveAnyVirtualDisplayDeviceId();
        }
      }

      if (stop_token.stop_requested()) {
        rollback_created_display();
        return std::nullopt;
      }

      const auto has_target_identity = display_config_identity && display_config_identity_has_display_name(*display_config_identity);
      const auto display_config_ptr = has_target_identity ? &*display_config_identity : nullptr;
      if (!resolved_display_name && !device_id && !has_target_identity) {
        BOOST_LOG(debug) << "Sunshine temporary display created before Windows exposed a target-specific display identity; waiting for virtual display enumeration.";
      }

      bool confirmed_active = false;
      if (!wait_for_virtual_display_ready(resolved_display_name, device_id, width, height, display_config_ptr, &confirmed_active, stop_token)) {
        if (stop_token.stop_requested()) {
          rollback_created_display();
          return std::nullopt;
        }
        if (allow_pending_enumeration) {
          // DisplayConfig state has changed since the first attempt, so one more
          // best-effort activation is worthwhile before we give up on it.
          if (!exact_target_activated) {
            exact_target_activated = activate_display_config_target(output, width, height, requested_fps, stop_token);
          }
          if (stop_token.stop_requested()) {
            rollback_created_display();
            return std::nullopt;
          }
          if (exact_target_activated) {
            BOOST_LOG(warning) << "Sunshine temporary display is active in DisplayConfig, but higher-level display enumeration is unavailable; retaining it for encoder probing.";
          } else {
            BOOST_LOG(warning) << "Sunshine temporary display was accepted by the driver, but Windows display enumeration is unavailable; retaining it for encoder probing.";
          }

          VirtualDisplayCreationResult result;
          result.display_name = resolved_display_name;
          if (device_id && !device_id->empty()) {
            result.device_id = *device_id;
          }
          if (s_client_name && std::strlen(s_client_name) > 0) {
            result.client_name = std::string(s_client_name);
          }
          result.reused_existing = false;
          // Enumeration never succeeded here, so only an observed exact-target
          // activation may claim the display is active. Otherwise leave
          // ready_since unset rather than stamping "now".
          if (exact_target_activated) {
            result.confirmed_active = true;
            result.ready_since = std::chrono::steady_clock::now();
          }
          driver_lease_tracker().update_identity(requested_uuid, result.display_name, result.device_id, result.monitor_device_path);
          return result;
        }

        printf("[SunshineVirtualDisplay] Timed out waiting for Windows to enumerate the new virtual display; reverting creation.\n");
        rollback_created_display();
        return std::nullopt;
      }

      // Enumeration has now succeeded, so DisplayConfig state has materially
      // changed since the first attempt. Failure downgrades to the readiness
      // result we already have rather than discarding a usable display.
      if (!exact_target_activated) {
        exact_target_activated = activate_display_config_target(output, width, height, requested_fps, stop_token);
      }
      confirmed_active = confirmed_active || exact_target_activated;

      if (stop_token.stop_requested()) {
        rollback_created_display();
        return std::nullopt;
      }

      std::optional<bool> effective_hdr_enabled;
      if (hdr_requested) {
        const bool hdr_enabled = request_hdr10_advanced_color(output, stop_token);
        switch (VDISPLAY::policy::hdr_failure_action(hdr_requested, hdr_enabled, confirmed_active)) {
          case VDISPLAY::policy::hdr_activation_failure_action::none:
            effective_hdr_enabled = true;
            break;
          case VDISPLAY::policy::hdr_activation_failure_action::continue_sdr:
            if (!stop_token.stop_requested()) {
              effective_hdr_enabled = false;
              BOOST_LOG(warning) << "Sunshine virtual display HDR: requested HDR display did not become HDR-capable; continuing with SDR capture.";
            }
            break;
          case VDISPLAY::policy::hdr_activation_failure_action::defer_to_display_helper:
            if (!stop_token.stop_requested()) {
              // The target is enumerated but the helper has not activated it yet, so a
              // direct HDR request cannot stick. This is not a failure: the helper's
              // APPLY carries the same HDR request and the capture gate waits for it.
              BOOST_LOG(debug) << "Sunshine virtual display HDR: target is not active yet, so the direct HDR request was not applied; deferring to the display helper's APPLY.";
            }
            break;
        }
      } else if (reset_hdr_state_for_sdr(output, stop_token)) {
        // A stable virtual-display identity can retain Windows' per-monitor HDR
        // user setting from an earlier HDR stream. Confirm the SDR state before
        // the session helper snapshots it, so its APPLY stays a no-op.
        effective_hdr_enabled = false;
      }

      if (stop_token.stop_requested()) {
        rollback_created_display();
        return std::nullopt;
      }

      if (dpi_snapshot) {
        (void) apply_virtual_display_dpi_value(*dpi_snapshot);
      }

      // Prefer a real GDI display name (\\.\DISPLAYx) over GUID placeholders once enumeration is complete.
      if (resolved_display_name && !resolved_display_name->empty() && !is_gdi_display_name(*resolved_display_name)) {
        std::optional<std::wstring> gdi_name;
        if (auto identity = wait_for_display_config_identity(
              output,
              std::chrono::milliseconds(250),
              stop_token
            )) {
          if (identity->source_gdi_device_name && !identity->source_gdi_device_name->empty()) {
            gdi_name = identity->source_gdi_device_name;
            display_config_identity = identity;
          }
        }
        if (stop_token.stop_requested()) {
          rollback_created_display();
          return std::nullopt;
        }
        if (gdi_name && !gdi_name->empty() && is_gdi_display_name(*gdi_name)) {
          BOOST_LOG(debug) << "Virtual display: resolved GDI name '" << platf::to_utf8(*gdi_name) << "' after creation.";
          resolved_display_name = gdi_name;
        }
      }

      if (resolved_display_name) {
        wprintf(L"[SunshineVirtualDisplay] Virtual display added successfully: %ls\n", resolved_display_name->c_str());
      } else {
        wprintf(L"[SunshineVirtualDisplay] Virtual display added; device name pending enumeration (target=%u).\n", output.TargetId);
      }
      printf("[SunshineVirtualDisplay] Configuration: W: %d, H: %d, FPS: %d, DisplayId: %llu\n",
             width,
             height,
             requested_fps,
             static_cast<unsigned long long>(create_result.value.display_id != 0 ? create_result.value.display_id : display_id));

      const auto ready_since = std::chrono::steady_clock::now();
      VirtualDisplayCreationResult result;
      result.display_name = resolved_display_name;
      if (device_id && !device_id->empty()) {
        result.device_id = *device_id;
      }
      if (s_client_name && std::strlen(s_client_name) > 0) {
        result.client_name = std::string(s_client_name);
      }
      if (display_config_identity && display_config_identity->monitor_device_path && !display_config_identity->monitor_device_path->empty()) {
        result.monitor_device_path = display_config_identity->monitor_device_path;
      } else if (auto identity = query_display_config_identity(output)) {
        if (identity->monitor_device_path && !identity->monitor_device_path->empty()) {
          result.monitor_device_path = identity->monitor_device_path;
        }
      }
      if (stop_token.stop_requested()) {
        rollback_created_display();
        return std::nullopt;
      }
      result.reused_existing = false;
      result.confirmed_active = confirmed_active;
      result.hdr_enabled = effective_hdr_enabled;
      if (confirmed_active) {
        result.ready_since = ready_since;
      }
      std::optional<std::string> hdr_profile;
      if (s_hdr_profile && std::strlen(s_hdr_profile) > 0) {
        hdr_profile = std::string(s_hdr_profile);
      }
      apply_hdr_profile_if_available(
        result.display_name,
        result.device_id,
        result.monitor_device_path,
        result.client_name,
        hdr_profile,
        true,
        true,
        stop_token,
        deferred_hdr_profile_worker_key
      );
      if (stop_token.stop_requested()) {
        rollback_created_display();
        return std::nullopt;
      }
      driver_lease_tracker().update_identity(requested_uuid, result.display_name, result.device_id, result.monitor_device_path);
      return result;
    }

  }  // namespace

  static std::optional<VirtualDisplayCreationResult> create_virtual_display_with_stop(
    const char *s_client_uid,
    const char *s_client_name,
    const char *s_hdr_profile,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    const GUID &guid,
    uint32_t base_fps_millihz,
    bool framegen_refresh_active,
    int framegen_refresh_multiplier,
    bool hdr_requested,
    bool allow_pending_enumeration,
    bool replace_existing,
    std::stop_token stop_token
  ) {
    std::lock_guard<std::recursive_mutex> operation_lock(g_virtual_display_operation_mutex);
    if (stop_token.stop_requested()) {
      return std::nullopt;
    }
    if (!ensure_virtual_display_process_ownership()) {
      BOOST_LOG(error) << "Virtual display creation refused because another process owns the recovery journal.";
      return std::nullopt;
    }

    constexpr int kMaxInitializationAttempts = 3;
    const auto requested_uuid = guid_to_uuid(guid);

    for (int attempt = 1; attempt <= kMaxInitializationAttempts; ++attempt) {
      if (stop_token.stop_requested()) {
        return std::nullopt;
      }
      if (!ensure_control_transport_responsive("Sunshine virtual display creation", stop_token)) {
        BOOST_LOG(warning) << "Unable to open Sunshine virtual display driver transport for virtual display creation.";
        return std::nullopt;
      }

      // The launch path applies the configured render adapter before creation, but that
      // only lands when the transport was already open; a transport that is opened or
      // recovered here (including the per-attempt driver restarts below) starts on the
      // driver's default request state. Re-apply the preference now that the transport is
      // known good so creation follows a driver-accepted SetRenderAdapter request. Applying
      // it again against an unchanged transport is a no-op.
      if (!apply_configured_render_adapter_preference("virtual display creation")) {
        return std::nullopt;
      }

      bool allow_driver_recovery = true;
      auto result = create_virtual_display_once(
        s_hdr_profile,
        s_client_uid,
        s_client_name,
        width,
        height,
        fps,
        guid,
        base_fps_millihz,
        framegen_refresh_active,
        framegen_refresh_multiplier,
        hdr_requested,
        allow_pending_enumeration,
        replace_existing,
        allow_driver_recovery,
        stop_token
      );
      if (!result) {
        if (stop_token.stop_requested()) {
          return std::nullopt;
        }
        BOOST_LOG(warning) << "Virtual display creation attempt " << attempt << '/' << kMaxInitializationAttempts
                           << " failed.";

        if (!allow_driver_recovery) {
          BOOST_LOG(warning) << "Virtual display creation failed closed without restarting the adapter.";
          return std::nullopt;
        }

        if (attempt == kMaxInitializationAttempts) {
          BOOST_LOG(error) << "Virtual display could not be created after " << kMaxInitializationAttempts << " attempts.";
          return std::nullopt;
        }

        closeVDisplayDevice();

        if (!ensure_driver_is_ready_impl(RestartCooldownBehavior::wait, stop_token)) {
          BOOST_LOG(warning) << "Driver recovery failed after virtual display creation failure.";
          return std::nullopt;
        }

        if (open_vdisplay_device_impl(stop_token, OpenRecoveryBehavior::transport_only) != DRIVER_STATUS::OK) {
          BOOST_LOG(warning) << "Failed to re-open Sunshine virtual display driver after recovery.";
          return std::nullopt;
        }

        BOOST_LOG(info) << "Retrying Sunshine virtual display initialization (attempt "
                        << (attempt + 1) << '/' << kMaxInitializationAttempts << ").";
        continue;
      }

      if (stop_token.stop_requested()) {
        if (!result->reused_existing) {
          (void) remove_virtual_display_impl(guid, false, stop_token);
        }
        return std::nullopt;
      }
      if (allow_pending_enumeration || confirm_virtual_display_persistence(*result, width, height, stop_token)) {
        if (stop_token.stop_requested()) {
          if (!result->reused_existing) {
            (void) remove_virtual_display_impl(guid, false, stop_token);
          }
          return std::nullopt;
        }
        const auto scale_percent = VDISPLAY::effective_virtual_display_scale_percent(
          config::video.dd.virtual_display_scale_percent,
          width,
          height
        );
        if (scale_percent > 0) {

          auto apply_scale_to_path = [scale_percent](const std::wstring &path) {
            const auto scale_result = VDISPLAY::set_display_scale_percent(path, scale_percent);
            if (scale_result.applied) {
              BOOST_LOG(info) << "Virtual display scale: requested " << scale_result.requested_percent
                              << "%, recommended " << scale_result.recommended_percent
                              << "%, previous " << scale_result.previous_percent
                              << "%, current " << scale_result.current_percent << "%.";
            } else {
              BOOST_LOG(warning) << "Virtual display scale: unable to apply "
                                 << scale_result.requested_percent << "% (status=" << scale_result.status
                                 << ", target_found=" << scale_result.target_found
                                 << ", queried=" << scale_result.queried << ").";
            }
          };

          const bool has_virtual_target_identity =
            (result->display_name && !result->display_name->empty()) ||
            (result->device_id && !result->device_id->empty());
          if ((!result->monitor_device_path || result->monitor_device_path->empty()) && has_virtual_target_identity) {
            result->monitor_device_path = resolve_monitor_device_path(
              result->display_name,
              result->device_id,
              5,
              std::chrono::milliseconds(100),
              std::nullopt,
              stop_token
            );
          }
          if (stop_token.stop_requested()) {
            if (!result->reused_existing) {
              (void) remove_virtual_display_impl(guid, false, stop_token);
            }
            return std::nullopt;
          }

          if ((!result->monitor_device_path || result->monitor_device_path->empty()) && !has_virtual_target_identity) {
            BOOST_LOG(warning) << "Virtual display scale: virtual target identity was unavailable; Windows scale was not applied.";
          } else if (result->confirmed_active && result->monitor_device_path) {
            apply_scale_to_path(*result->monitor_device_path);
          } else {
            // The target is not active yet, so its monitor path either does not
            // resolve or does not belong to a live output. The display helper's
            // APPLY is what activates it, and that cannot happen until creation
            // returns — so finish the scale from a bounded background worker
            // instead of turning a pending activation into a terminal result.
            BOOST_LOG(debug) << "Virtual display scale: target is not active yet; deferring scale application until the display helper activates it.";
            const auto started = deferred_scale_workers().start(
              requested_uuid.string(),
              [display_name = result->display_name,
               device_id = result->device_id,
               apply_scale_to_path](std::stop_token worker_stop_token) mutable {
                constexpr auto kActivationBudget = std::chrono::seconds(15);
                constexpr auto kRetryInterval = std::chrono::milliseconds(250);
                const auto deadline = std::chrono::steady_clock::now() + kActivationBudget;

                if ((!display_name || display_name->empty()) && (!device_id || device_id->empty())) {
                  BOOST_LOG(warning) << "Virtual display scale: deferred application has no virtual target identity; Windows scale was not applied.";
                  return;
                }

                while (!worker_stop_token.stop_requested()) {
                  auto path = resolve_monitor_device_path(
                    display_name,
                    device_id,
                    1,
                    std::chrono::milliseconds(0),
                    std::nullopt,
                    worker_stop_token
                  );
                  if (path && !path->empty()) {
                    apply_scale_to_path(*path);
                    return;
                  }
                  if (std::chrono::steady_clock::now() >= deadline) {
                    BOOST_LOG(warning) << "Virtual display scale: monitor device path did not become available within "
                                       << std::chrono::duration_cast<std::chrono::seconds>(kActivationBudget).count()
                                       << "s; Windows scale was not applied.";
                    return;
                  }
                  if (wait_for_monitor_stop(worker_stop_token, kRetryInterval)) {
                    BOOST_LOG(debug) << "Virtual display scale: deferred application cancelled.";
                    return;
                  }
                }
                BOOST_LOG(debug) << "Virtual display scale: deferred application cancelled.";
              }
            );
            if (!started) {
              BOOST_LOG(debug) << "Virtual display scale: deferred application was not started because shutdown is in progress.";
            }
          }
        }
        if (stop_token.stop_requested()) {
          if (!result->reused_existing) {
            (void) remove_virtual_display_impl(guid, false, stop_token);
          }
          return std::nullopt;
        }
        if (!track_virtual_display_created(requested_uuid)) {
          BOOST_LOG(error) << "Sunshine virtual display survived creation but its render-adapter request provenance could not be validated.";
          (void) remove_virtual_display_impl(guid, false, stop_token);
          return std::nullopt;
        }
        return result;
      }

      if (stop_token.stop_requested()) {
        if (!result->reused_existing) {
          (void) remove_virtual_display_impl(guid, false, stop_token);
        }
        return std::nullopt;
      }

      const auto name_utf8 = result->display_name ? platf::to_utf8(*result->display_name) : std::string("(pending)");
      BOOST_LOG(warning) << "Virtual display '" << name_utf8 << "' vanished after creation attempt "
                         << attempt << '/' << kMaxInitializationAttempts << "; recovering driver.";

      if (attempt == kMaxInitializationAttempts) {
        break;
      }

      closeVDisplayDevice();

      if (!ensure_driver_is_ready_impl(RestartCooldownBehavior::wait, stop_token)) {
        BOOST_LOG(warning) << "Driver recovery failed after virtual display vanished.";
        return std::nullopt;
      }

      if (open_vdisplay_device_impl(stop_token, OpenRecoveryBehavior::transport_only) != DRIVER_STATUS::OK) {
        BOOST_LOG(warning) << "Failed to re-open Sunshine virtual display driver after recovery.";
        return std::nullopt;
      }

      BOOST_LOG(info) << "Retrying Sunshine virtual display initialization (attempt "
                      << (attempt + 1) << '/' << kMaxInitializationAttempts << ").";
    }

    BOOST_LOG(error) << "Virtual display could not be stabilized after " << kMaxInitializationAttempts << " attempts.";
    return std::nullopt;
  }

  std::optional<VirtualDisplayCreationResult> createVirtualDisplay(
    const char *s_client_uid,
    const char *s_client_name,
    const char *s_hdr_profile,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    const GUID &guid,
    uint32_t base_fps_millihz,
    bool framegen_refresh_active,
    int framegen_refresh_multiplier,
    bool hdr_requested,
    bool allow_pending_enumeration,
    bool replace_existing
  ) {
    if (!release_retained_ensure_display_for_stream(guid, s_client_uid)) {
      return std::nullopt;
    }
    return create_virtual_display_with_stop(
      s_client_uid,
      s_client_name,
      s_hdr_profile,
      width,
      height,
      fps,
      guid,
      base_fps_millihz,
      framegen_refresh_active,
      framegen_refresh_multiplier,
      hdr_requested,
      allow_pending_enumeration,
      replace_existing,
      {}
    );
  }

  bool removeAllVirtualDisplays() {
    // Request cancellation before taking the operation lock. A recovery
    // monitor may be waiting to take it after an external driver call.
    abort_all_recovery_monitors();
    std::lock_guard<std::recursive_mutex> operation_lock(g_virtual_display_operation_mutex);
    if (!ensure_virtual_display_process_ownership()) {
      BOOST_LOG(error) << "Virtual display cleanup refused because another process owns the recovery journal.";
      return false;
    }

    auto all_guids = active_virtual_display_tracker().all();
    std::vector<VirtualDisplayRecoveryEntry> recovery_entries;
    const auto recovery_load = load_virtual_display_recovery_journal(recovery_entries);
    if (recovery_load == VirtualDisplayRecoveryLoadResult::failed) {
      BOOST_LOG(error) << "Virtual display cleanup could not read protected recovery state.";
      return false;
    }
    for (const auto &entry : recovery_entries) {
      if (std::find(all_guids.begin(), all_guids.end(), entry.guid) == all_guids.end()) {
        all_guids.push_back(entry.guid);
      }
    }
    if (all_guids.empty()) {
      BOOST_LOG(debug) << "No in-process or securely recoverable virtual display GUIDs to remove.";
      return true;
    }

    bool all_removed = true;
    for (const auto &guid : all_guids) {
      GUID native_guid = uuid_to_guid(guid);
      BOOST_LOG(debug) << "Removing virtual display with GUID " << guid.string();
      if (!VDISPLAY_SUNSHINE::removeVirtualDisplay(native_guid)) {
        all_removed = false;
      }
    }

    if (all_removed) {
      BOOST_LOG(info) << "Virtual display devices have been removed successfully.";
    } else {
      BOOST_LOG(warning) << "Virtual display devices failed to be removed.";
    }

    return all_removed;
  }

  bool removeVirtualDisplay(const GUID &guid) {
    return remove_virtual_display_impl(guid, true);
  }

  static bool remove_virtual_display_impl(
    const GUID &guid,
    bool cancel_recovery_monitor,
    std::stop_token stop_token
  ) {
    const auto guid_uuid = guid_to_uuid(guid);
    // A recovery-owned rollback must complete removal even after its monitor
    // has been stopped. Otherwise a stale transport at exactly that point can
    // orphan the display the cancelled recovery just created. External
    // removal, by contrast, may abandon before opening a new transport.
    const std::stop_token reopen_stop_token = cancel_recovery_monitor ? stop_token : std::stop_token {};
    const auto reopen_recovery_behavior = cancel_recovery_monitor ?
                                            OpenRecoveryBehavior::transport_only :
                                            OpenRecoveryBehavior::recover_driver;
    // Always cancel profile work for a display being removed. The recovery
    // monitor may already have completed, so its stop token alone is not a
    // sufficient lifetime boundary for this deferred work.
    deferred_hdr_profile_workers().request_stop(guid_uuid.string());
    if (cancel_recovery_monitor && stop_token.stop_requested()) {
      return false;
    }
    // Do not make cancellation wait behind the operation mutex held by a
    // monitor that is about to perform its recovery callback.
    if (cancel_recovery_monitor) {
      abort_recovery_monitor(guid_uuid);
    }
    std::lock_guard<std::recursive_mutex> operation_lock(g_virtual_display_operation_mutex);
    if (!ensure_virtual_display_process_ownership()) {
      BOOST_LOG(error) << "Virtual display removal refused because another process owns the recovery journal.";
      return false;
    }

    auto transport = control_transport_snapshot();
    const bool initial_transport_invalid = !driver_transport_responsive(transport.get());
    bool opened_handle = false;

    auto ensure_handle = [&]() -> bool {
      transport = control_transport_snapshot();
      if (driver_transport_responsive(transport.get())) {
        return true;
      }

      if (cancel_recovery_monitor && stop_token.stop_requested()) {
        return false;
      }

      if (transport) {
        printf("[SunshineVirtualDisplay] Cached driver transport is not responsive while removing virtual display; reopening.\n");
        closeVDisplayDevice();
      }

      if (open_vdisplay_device_impl(reopen_stop_token, reopen_recovery_behavior) != DRIVER_STATUS::OK) {
        printf("[SunshineVirtualDisplay] Failed to open driver while removing virtual display.\n");
        return false;
      }
      opened_handle = true;
      transport = control_transport_snapshot();
      if (!driver_transport_responsive(transport.get())) {
        closeVDisplayDevice();
        return false;
      }
      return true;
    };

    if (!ensure_handle()) {
      return false;
    }

    auto lease_info = driver_lease_tracker().get(guid_uuid);
    if (!lease_info) {
      sunshine_driver::ControlClient client {*transport};
      const auto secure_reclaim_supported = driver_supports_secure_reclaim(client);
      if (!secure_reclaim_supported) {
        return false;
      }
      if (*secure_reclaim_supported) {
        std::vector<VirtualDisplayRecoveryEntry> recovery_entries;
        if (!load_virtual_display_recovery_journal_for_driver(client, recovery_entries)) {
          return false;
        }
        const auto reclaimed = reclaim_virtual_display_recovery_entry(
          client,
          recovery_entries,
          guid_uuid,
          std::nullopt,
          std::nullopt,
          std::nullopt
        );
        if (reclaimed == VirtualDisplayReclaimResult::failed) {
          return false;
        }
        lease_info = driver_lease_tracker().get(guid_uuid);
      }
    }

    if (!lease_info) {
      sunshine_driver::ControlClient client {*transport};
      const auto display_id = client_uuid_to_virtual_display_id(guid);
      const auto display_exists = driver_has_temporary_display(client, display_id);
      if (!display_exists) {
        return false;
      }
      if (!*display_exists) {
        track_virtual_display_removed(guid_uuid);
        note_virtual_display_teardown();
        return true;
      }
      BOOST_LOG(warning) << "Refusing to remove Sunshine virtual display guid=" << guid_uuid.string()
                         << " because exact lease ownership is unavailable.";
      return false;
    }

    auto cached_display_name = lease_info->display_name ? lease_info->display_name : resolve_virtual_display_name_from_devices();

    auto perform_remove = [&]() -> std::pair<bool, DWORD> {
      sunshine_driver::ControlClient client {*transport};
      sunshine_driver::LeaseDisplayRequest remove_request {};
      remove_request.lease_id = lease_info->lease_id;
      remove_request.display_id = lease_info->display_id;
      const auto removed = client.remove_temporary_display(remove_request);
      if (removed.ok()) {
        track_virtual_display_removed(guid_uuid);
        note_virtual_display_teardown();
        return std::pair<bool, DWORD> {true, ERROR_SUCCESS};
      }

      const DWORD error_code = removed.native_error;
      if (removed.status == sunshine_driver::ControlStatus::TransportFailed && is_missing_lease_error(error_code)) {
        const auto display_exists = driver_has_temporary_display(client, lease_info->display_id);
        if (display_exists && !*display_exists) {
          track_virtual_display_removed(guid_uuid);
          note_virtual_display_teardown();
          return std::pair<bool, DWORD> {true, error_code};
        }
      }

      return {false, error_code};
    };

    auto [removed, error_code] = perform_remove();
    if (!removed && error_code == ERROR_INVALID_HANDLE) {
      printf("[SunshineVirtualDisplay] Driver transport became invalid while removing virtual display; retrying.\n");
      closeVDisplayDevice();
      if ((!cancel_recovery_monitor || !stop_token.stop_requested()) &&
          open_vdisplay_device_impl(reopen_stop_token, reopen_recovery_behavior) == DRIVER_STATUS::OK) {
        opened_handle = true;
        transport = control_transport_snapshot();
        auto retry_result = perform_remove();
        removed = retry_result.first;
        error_code = retry_result.second;
      } else {
        error_code = ERROR_INVALID_HANDLE;
      }
    }

    if (opened_handle && initial_transport_invalid) {
      closeVDisplayDevice();
    }

    if (removed) {
      printf("[SunshineVirtualDisplay] Virtual display removed successfully.\n");
      if (cached_display_name) {
        constexpr auto teardown_timeout = std::chrono::seconds(2);
        if (!wait_for_virtual_display_teardown(*cached_display_name, teardown_timeout, stop_token)) {
          if (stop_token.stop_requested()) {
            BOOST_LOG(debug) << "Virtual display teardown wait cancelled for '"
                             << platf::to_utf8(*cached_display_name) << "'.";
            return true;
          }
          BOOST_LOG(warning) << "Virtual display '" << platf::to_utf8(*cached_display_name)
                             << "' still reported by Windows after teardown wait.";
        } else {
          BOOST_LOG(debug) << "Virtual display '" << platf::to_utf8(*cached_display_name)
                           << "' removed from enumeration after teardown.";
        }
      }
      return true;
    }

    printf("[SunshineVirtualDisplay] Failed to remove virtual display (error=%lu).\n", static_cast<unsigned long>(error_code));
    return false;
  }

  static bool is_sunshine_driver_installed_passive() {
    // Status/enumeration paths must not repair or restart a missing device node.
    // Recovery remains limited to explicit driver initialization/use.
    const auto transport = control_transport_snapshot();
    if (driver_transport_responsive(transport.get())) {
      return true;
    }

    if (probe_driver_responsive_once()) {
      return true;
    }

    return find_virtual_display_device_instance_id().has_value();
  }

  bool isVirtualDisplayDriverInstalled() {
    return VDISPLAY::policy::passive_install_status(is_sunshine_driver_installed_passive());
  }

  std::optional<std::string> resolveVirtualDisplayDeviceId(const std::wstring &display_name) {
    if (display_name.empty()) {
      return resolveAnyVirtualDisplayDeviceId();
    }

    auto devices = platf::display_helper::Coordinator::instance().enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
    if (!devices) {
      return std::nullopt;
    }

    const auto utf8_name = platf::to_utf8(display_name);
    const auto target = normalize_display_name(utf8_name);
    if (target.empty()) {
      return std::nullopt;
    }

    for (const auto &device : *devices) {
      if (!is_virtual_display_device(device) || device.m_device_id.empty()) {
        continue;
      }

      const auto device_id = normalize_display_name(device.m_device_id);
      const auto display_name = normalize_display_name(device.m_display_name);
      const auto friendly_name = normalize_display_name(device.m_friendly_name);
      if (device_id == target ||
          (!display_name.empty() && display_name == target) ||
          (!friendly_name.empty() && friendly_name == target)) {
        return device.m_device_id;
      }
    }

    return std::nullopt;
  }

  std::optional<std::string> resolveVirtualDisplayDeviceIdForClient(const std::string &client_name) {
    if (client_name.empty()) {
      return std::nullopt;
    }

    auto devices = platf::display_helper::Coordinator::instance().enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
    if (!devices) {
      return std::nullopt;
    }

    std::optional<std::string> active_match;
    std::optional<std::string> any_match;
    for (const auto &device : *devices) {
      if (!is_virtual_display_device(device) || device.m_device_id.empty()) {
        continue;
      }
      if (device.m_friendly_name.empty() || !equals_ci(device.m_friendly_name, client_name)) {
        continue;
      }

      if (!any_match) {
        any_match = device.m_device_id;
      }
      if (device.m_info && !device.m_display_name.empty()) {
        active_match = device.m_device_id;
        break;
      }
    }

    if (active_match) {
      return active_match;
    }
    if (any_match) {
      return any_match;
    }
    return std::nullopt;
  }

  std::optional<std::string> resolveActiveVirtualDisplayDeviceId(
    const std::string &preferred_output_identifier,
    const std::string &client_name,
    bool allow_any_fallback
  ) {
    BOOST_LOG(debug) << "Resolving active virtual display device_id from preferred_output='"
                     << preferred_output_identifier << "' client_name='" << client_name << "'.";
    auto devices = platf::display_helper::Coordinator::instance().enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
    if (!devices) {
      BOOST_LOG(debug) << "Resolving active virtual display device_id failed: device enumeration unavailable.";
      return std::nullopt;
    }

    std::optional<std::string> normalized_output;
    if (!preferred_output_identifier.empty() &&
        !is_virtual_display_selection(preferred_output_identifier)) {
      const auto normalized = normalize_display_name(preferred_output_identifier);
      if (!normalized.empty()) {
        normalized_output = normalized;
      }
    }

    std::optional<std::string> normalized_client_name;
    if (!client_name.empty()) {
      const auto normalized = normalize_display_name(client_name);
      if (!normalized.empty()) {
        normalized_client_name = normalized;
      }
    }

    std::optional<std::string> output_match;
    std::optional<std::string> client_match;
    std::optional<std::string> active_any_match;
    std::optional<std::string> any_match;

    for (const auto &device : *devices) {
      if (!is_virtual_display_device(device) || device.m_device_id.empty()) {
        continue;
      }

      if (!any_match) {
        any_match = device.m_device_id;
      }
      if (!active_any_match && device.m_info && !device.m_display_name.empty()) {
        active_any_match = device.m_device_id;
      }

      const auto candidate_display_name =
        !device.m_display_name.empty() ? std::make_optional(normalize_display_name(device.m_display_name)) : std::nullopt;
      const auto candidate_friendly_name =
        !device.m_friendly_name.empty() ? std::make_optional(normalize_display_name(device.m_friendly_name)) : std::nullopt;

      bool matches_output = false;
      if (normalized_output) {
        matches_output =
          equals_ci(device.m_device_id, preferred_output_identifier) ||
          (candidate_display_name && *candidate_display_name == *normalized_output) ||
          (candidate_friendly_name && *candidate_friendly_name == *normalized_output);
      }

      if (matches_output) {
        if (device.m_info && !device.m_display_name.empty()) {
          BOOST_LOG(debug) << "Resolved active virtual display by preferred output: device_id='" << device.m_device_id << "'.";
          return device.m_device_id;
        }
        if (!output_match) {
          output_match = device.m_device_id;
        }
      }

      bool matches_client_name = false;
      if (normalized_client_name) {
        matches_client_name = candidate_friendly_name && *candidate_friendly_name == *normalized_client_name;
      }

      if (matches_client_name) {
        if (device.m_info && !device.m_display_name.empty()) {
          BOOST_LOG(debug) << "Resolved active virtual display by client name: device_id='" << device.m_device_id << "'.";
          return device.m_device_id;
        }
        if (!client_match) {
          client_match = device.m_device_id;
        }
      }
    }

    if (output_match) {
      BOOST_LOG(debug) << "Resolved inactive virtual display fallback by preferred output: device_id='" << *output_match << "'.";
      return output_match;
    }
    if (client_match) {
      BOOST_LOG(debug) << "Resolved inactive virtual display fallback by client name: device_id='" << *client_match << "'.";
      return client_match;
    }
    if (!allow_any_fallback) {
      BOOST_LOG(debug) << "No exact virtual display match found and generic fallback is disabled.";
      return std::nullopt;
    }
    if (active_any_match) {
      BOOST_LOG(debug) << "Resolved active virtual display fallback: device_id='" << *active_any_match << "'.";
      return active_any_match;
    }
    if (any_match) {
      BOOST_LOG(debug) << "Resolved inactive virtual display fallback: device_id='" << *any_match << "'.";
      return any_match;
    }
    BOOST_LOG(debug) << "No virtual display device_id could be resolved for preferred_output='"
                     << preferred_output_identifier << "' client_name='" << client_name << "'.";
    return std::nullopt;
  }

  std::optional<std::string> resolveActiveVirtualDisplayDeviceIdForStableId(
    const std::string &stable_id,
    const std::string &preferred_output_identifier,
    const std::string &client_name,
    bool allow_any_fallback
  ) {
    if (stable_id.empty()) {
      return resolveActiveVirtualDisplayDeviceId(preferred_output_identifier, client_name, allow_any_fallback);
    }

    const auto stable_uuid = virtualDisplayUuidFromStableId(stable_id);
    const auto stable_guid = uuid_to_guid(stable_uuid);
    const auto expected_display_id = client_uuid_to_virtual_display_id(stable_guid);

    BOOST_LOG(debug) << "Resolving active virtual display device_id from stable_id='"
                     << stable_id << "' display_id=" << expected_display_id
                     << " preferred_output='" << preferred_output_identifier
                     << "' client_name='" << client_name << "'.";

    auto devices = platf::display_helper::Coordinator::instance().enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
    if (!devices) {
      BOOST_LOG(debug) << "Resolving active virtual display device_id by stable id failed: device enumeration unavailable.";
      return resolveActiveVirtualDisplayDeviceId(preferred_output_identifier, client_name, allow_any_fallback);
    }

    std::optional<std::string> inactive_identity_match;
    for (const auto &device : *devices) {
      if (!is_virtual_display_device(device) || device.m_device_id.empty()) {
        continue;
      }
      if (!matches_virtual_display_id_edid(device, expected_display_id)) {
        continue;
      }

      if (device.m_info && !device.m_display_name.empty()) {
        BOOST_LOG(debug) << "Resolved active virtual display by stable EDID identity: device_id='"
                         << device.m_device_id << "'.";
        return device.m_device_id;
      }
      if (!inactive_identity_match) {
        inactive_identity_match = device.m_device_id;
      }
    }

    if (inactive_identity_match) {
      BOOST_LOG(debug) << "Resolved inactive virtual display fallback by stable EDID identity: device_id='"
                       << *inactive_identity_match << "'.";
      return inactive_identity_match;
    }

    BOOST_LOG(debug) << "No virtual display matched stable EDID identity for stable_id='"
                     << stable_id << "'; falling back to exact display/client names.";
    return resolveActiveVirtualDisplayDeviceId(preferred_output_identifier, client_name, allow_any_fallback);
  }

  std::optional<std::string> resolveAnyVirtualDisplayDeviceId() {
    auto devices = platf::display_helper::Coordinator::instance().enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
    std::optional<std::string> active_match;
    std::optional<std::string> any_match;

    if (devices) {
      for (const auto &device : *devices) {
        if (!is_virtual_display_device(device) || device.m_device_id.empty()) {
          continue;
        }

        if (!any_match) {
          any_match = device.m_device_id;
        }
        if (device.m_info && !device.m_display_name.empty()) {
          active_match = device.m_device_id;
          break;
        }
      }
    }

    if (active_match) {
      return active_match;
    }
    if (any_match) {
      return any_match;
    }
    return std::nullopt;
  }

  bool is_virtual_display_output(const std::string &output_identifier) {
    if (output_identifier.empty()) {
      return false;
    }

    const auto devices = platf::display_helper::Coordinator::instance().enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
    if (!devices) {
      return false;
    }

    for (const auto &device : *devices) {
      if (!is_virtual_display_device(device)) {
        continue;
      }

      if (!device.m_device_id.empty() && equals_ci(device.m_device_id, output_identifier)) {
        return true;
      }
      if (!device.m_display_name.empty() && equals_ci(device.m_display_name, output_identifier)) {
        return true;
      }
    }

    return false;
  }

  bool is_virtual_display_selection(const std::string &output_identifier) {
    return VDISPLAY::policy::is_virtual_display_selection(output_identifier, false);
  }

  std::vector<VirtualDisplayInfo> enumerateVirtualDisplays() {
    std::vector<VirtualDisplayInfo> result;

    if (!isVirtualDisplayDriverInstalled()) {
      return result;
    }

    const auto devices = platf::display_helper::Coordinator::instance().enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
    if (!devices) {
      return result;
    }

    for (const auto &device : *devices) {
      if (!is_virtual_display_device(device)) {
        continue;
      }

      VirtualDisplayInfo info;
      info.device_name = !device.m_display_name.empty() ? platf::from_utf8(device.m_display_name) : platf::from_utf8(device.m_device_id.empty() ? device.m_friendly_name : device.m_device_id);
      info.friendly_name = !device.m_friendly_name.empty() ? platf::from_utf8(device.m_friendly_name) : info.device_name;
      // A blank GDI name is not a usable capture target even when Windows
      // publishes mode information for the device.
      info.is_active = !device.m_display_name.empty();
      info.width = 0;
      info.height = 0;

      if (device.m_info && device.m_info->m_resolution.m_width > 0 && device.m_info->m_resolution.m_height > 0) {
        info.width = static_cast<int>(device.m_info->m_resolution.m_width);
        info.height = static_cast<int>(device.m_info->m_resolution.m_height);
      }

      result.push_back(std::move(info));
    }

    return result;
  }

  // END ISOLATED DISPLAY METHODS
}  // namespace VDISPLAY_SUNSHINE

bool VDISPLAY_SUNSHINE::has_active_physical_display() {
  auto devices = platf::display_helper::Coordinator::instance().enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
  BOOST_LOG(debug) << "Enumerated devices count: " << (devices ? devices->size() : 0);
  if (!devices) {
    BOOST_LOG(warning) << "Physical display enumeration is unavailable; preserving fail-open physical-display detection.";
    return true;
  }

  std::vector<std::string> active_physical_displays;
  for (const auto &device : *devices) {
    bool is_virtual = is_virtual_display_device(device);
    if (!is_virtual) {
      bool is_active = !device.m_display_name.empty();
      BOOST_LOG(debug) << "Physical device: " << device.m_display_name << ", is_active: " << is_active;
      if (is_active) {
        active_physical_displays.push_back(device.m_display_name);
      }
    }
  }

  if (active_physical_displays.empty()) {
    BOOST_LOG(debug) << "No active physical display found, returning false";
    return false;
  }

  return platf::configured_capture_adapter_has_output(active_physical_displays);
}

bool VDISPLAY_SUNSHINE::should_auto_enable_virtual_display() {
  if (!isVirtualDisplayDriverInstalled()) {
    BOOST_LOG(warning) << "Virtual display driver not available, not enabling virtual display.";
    return false;
  }

  if (has_active_physical_display()) {
    BOOST_LOG(debug) << "Active physical display detected, not enabling virtual display.";
    return false;
  }

  return true;
}

uuid_util::uuid_t VDISPLAY_SUNSHINE::persistentVirtualDisplayUuid() {
  // Reserved, deterministic identity for the headless encoder-probe / shared "Sunshine Temporary"
  // display. It must never equal a per-client display GUID (those are derived from client stable
  // ids via virtualDisplayUuidFromStableId), otherwise the temporary display becomes an identity
  // twin of a paired client and defeats stable-identity matching. Deriving it from the fixed
  // "sunshine-ensure" sentinel (the same client_uid used to create the temp display) keeps it
  // stable across runs and immune to the state-file contamination that previously let
  // root.virtual_display_guid hold a real client's display GUID.
  return VDISPLAY::persistentVirtualDisplayUuid();
}

namespace {
  struct ensure_target_snapshot_t {
    std::string device_id;
    std::string display_name;
    bool active = false;
  };

  ensure_target_snapshot_t sunshine_ensure_target_snapshot(const GUID &guid) {
    ensure_target_snapshot_t snapshot;
    const auto expected_display_id = VDISPLAY_SUNSHINE::client_uuid_to_virtual_display_id(guid);
    const auto devices =
      platf::display_helper::Coordinator::instance().enumerate_devices(
        display_device::DeviceEnumerationDetail::Minimal
      );
    if (!devices) {
      return snapshot;
    }

    for (const auto &device : *devices) {
      if (!VDISPLAY_SUNSHINE::is_virtual_display_device(device) ||
          !VDISPLAY_SUNSHINE::matches_virtual_display_id_edid(device, expected_display_id)) {
        continue;
      }

      snapshot.device_id = device.m_device_id;
      if (device.m_display_name.empty()) {
        return snapshot;
      }

      snapshot.display_name = device.m_display_name;
      snapshot.active = device.m_info.has_value();
      return snapshot;
    }
    return snapshot;
  }

  void wait_for_sunshine_ensure_target(
    VDISPLAY::ensure_display_result &result,
    std::chrono::steady_clock::duration timeout
  ) {
    result.readiness = VDISPLAY::ensure_display_readiness_e::request_retained;
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (true) {
      const auto snapshot = sunshine_ensure_target_snapshot(result.temporary_guid);
      result.device_id = snapshot.device_id;
      result.display_name = snapshot.display_name;
      if (!snapshot.display_name.empty()) {
        result.readiness = VDISPLAY::ensure_display_readiness_e::target_enumerated;
        if (snapshot.active) {
          const auto dxgi_names = platf::display_names(platf::mem_type_e::dxgi);
          if (std::any_of(dxgi_names.begin(), dxgi_names.end(), [&](const std::string &name) {
                return !name.empty() && boost::iequals(name, snapshot.display_name);
              })) {
            result.readiness = VDISPLAY::ensure_display_readiness_e::target_ready;
            return;
          }
        }
      }

      if (std::chrono::steady_clock::now() >= deadline) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}  // namespace

VDISPLAY_SUNSHINE::ensure_display_result VDISPLAY_SUNSHINE::ensure_display(
  const std::optional<LUID> &required_adapter_luid
) {
  std::lock_guard<std::mutex> acquire_lock(g_ensure_display_acquire_mutex);
  ensure_display_result result;

  if (!required_adapter_luid && has_active_physical_display()) {
    result.readiness = ensure_display_readiness_e::existing_display;
    return result;
  }

  if (required_adapter_luid) {
    const auto configured_adapter = platf::resolve_preferred_render_adapter(
      config::video.adapter_name,
      config::video.adapter_pnp_id
    );
    if (!configured_adapter ||
        !platf::adapter_luid_equal(*required_adapter_luid, *configured_adapter.luid)) {
      BOOST_LOG(error)
        << "Owned Sunshine encoder-probe display request no longer matches the preferred render adapter.";
      return result;
    }
    if (!isVirtualDisplayDriverInstalled()) {
      BOOST_LOG(warning) << "Virtual display driver not available for owned encoder probing.";
      return result;
    }
  } else if (!should_auto_enable_virtual_display()) {
    BOOST_LOG(debug) << "No active physical displays and virtual display auto-enable is disabled.";
    return result;
  }

  if (proc::vDisplayDriverStatus.load(std::memory_order_acquire) != DRIVER_STATUS::OK) {
    proc::initVDisplayDriver();
    const auto driver_status = proc::vDisplayDriverStatus.load(std::memory_order_acquire);
    if (driver_status != DRIVER_STATUS::OK) {
      BOOST_LOG(warning) << "Virtual display driver unavailable for display ensure (status=" << static_cast<int>(driver_status) << "). Continuing with best-effort ensure.";
    }
  }

  auto uuid = persistentVirtualDisplayUuid();
  std::memcpy(&result.temporary_guid, uuid.b8, sizeof(result.temporary_guid));

  bool retained_ensure_display = false;
  {
    std::unique_lock<std::mutex> lock(g_ensure_display_state_mutex);
    g_ensure_display_state_cv.wait(lock, []() {
      return !g_ensure_display_lifetime.removal_in_progress();
    });
    if (g_ensure_display_lifetime.retained() &&
        guid_equal(g_ensure_display_guid, result.temporary_guid)) {
      if (const auto generation = g_ensure_display_lifetime.acquire()) {
        result.tracks_temporary_for_probe = true;
        result.temporary_generation = *generation;
        retained_ensure_display = true;
      }
    }
  }

  if (retained_ensure_display &&
      is_virtual_display_guid_tracked(result.temporary_guid) &&
      VDISPLAY::configuredRenderAdapterMatchesVirtualDisplay(
        result.temporary_guid,
        "retained Sunshine encoder-probe display"
      )) {
    wait_for_sunshine_ensure_target(result, std::chrono::seconds(3));
    BOOST_LOG(info) << "Reusing temporary virtual display for the active encoder probe (readiness="
                    << static_cast<int>(result.readiness)
                    << ", display_name='"
                    << (result.display_name.empty() ? std::string("<pending>") : result.display_name)
                    << "').";
    return result;
  }

  if (retained_ensure_display) {
    VDISPLAY_SUNSHINE::cleanup_ensure_display(result);
    result.tracks_temporary_for_probe = false;
    result.temporary_generation = 0;
    std::lock_guard<std::mutex> lock(g_ensure_display_state_mutex);
    if (g_ensure_display_lifetime.retained()) {
      BOOST_LOG(error) << "Failed to remove stale owned Sunshine encoder-probe display.";
      return result;
    }
    BOOST_LOG(debug) << "Ensure display retention state was stale; creating a fresh temporary display.";
  }

  auto virtual_displays = enumerateVirtualDisplays();
  bool has_active_virtual = std::any_of(
    virtual_displays.begin(),
    virtual_displays.end(),
    [](const VirtualDisplayInfo &info) {
      return info.is_active;
    }
  );

  if (has_active_virtual) {
    const auto permanent_count = static_cast<std::uint32_t>(std::clamp(
      config::video.dd.virtual_display_permanent_count,
      0,
      config::SUNSHINE_VIRTUAL_DISPLAY_MAX_PERMANENT_COUNT
    ));
    std::lock_guard<std::recursive_mutex> operation_lock(g_virtual_display_operation_mutex);
    if (config::video.dd.virtual_display_permanent_count_configured &&
        permanent_count > 0 &&
        apply_configured_render_adapter_preference("active permanent Sunshine virtual display refresh")) {
      const auto transport = control_transport_snapshot();
      if (transport && transport->valid()) {
        sunshine_driver::ControlClient client {*transport};
        if (set_permanent_display_count(client, permanent_count, false)) {
          BOOST_LOG(info) << "Refreshed active permanent Sunshine virtual display(s) after the render-adapter request was accepted.";
          const auto active_display = std::find_if(
            virtual_displays.begin(),
            virtual_displays.end(),
            [](const VirtualDisplayInfo &info) {
              return info.is_active && !info.device_name.empty();
            }
          );
          if (active_display != virtual_displays.end()) {
            result.display_name = platf::to_utf8(active_display->device_name);
            result.readiness = ensure_display_readiness_e::target_enumerated;
            const auto dxgi_names = platf::display_names(platf::mem_type_e::dxgi);
            if (std::any_of(dxgi_names.begin(), dxgi_names.end(), [&](const std::string &name) {
                  return !name.empty() && boost::iequals(name, result.display_name);
                })) {
              result.readiness = ensure_display_readiness_e::target_ready;
            }
          }
          return result;
        }
      }
      BOOST_LOG(warning) << "Could not refresh permanent Sunshine virtual displays after the render-adapter request was accepted.";
    }
    BOOST_LOG(warning) << "Active Sunshine virtual display has no exact owned render-adapter request provenance for encoder probing; attempting to create an owned temporary display.";
  }

  BOOST_LOG(info) << "Creating temporary virtual display to ensure display availability.";
  auto display_info = createVirtualDisplay(
    "sunshine-ensure",
    "Sunshine Temporary",
    nullptr,
    1920u,
    1080u,
    60000u,
    result.temporary_guid,
    60000u,
    false,
    1,
    false,
    true,
    false
  );
  if (!display_info) {
    BOOST_LOG(warning) << "Failed to create temporary virtual display.";
    return result;
  }

  result.created_temporary = true;
  result.tracks_temporary_for_probe = true;
  result.readiness = ensure_display_readiness_e::request_retained;
  bool lifetime_published = false;
  {
    std::lock_guard<std::mutex> lock(g_ensure_display_state_mutex);
    const auto generation = g_ensure_display_lifetime.begin_lifetime();
    if (generation) {
      result.temporary_generation = *generation;
      g_ensure_display_guid = result.temporary_guid;
      lifetime_published = true;
    }
  }
  if (!lifetime_published) {
    result.tracks_temporary_for_probe = false;
    BOOST_LOG(error) << "Could not publish Sunshine encoder-probe display ownership.";
    (void) removeVirtualDisplay(result.temporary_guid);
    return result;
  }

  // CCD and DXGI are distinct enumeration paths. Require the exact retained
  // target to have a non-empty GDI name and appear in DXGI; another output
  // must never satisfy readiness on its behalf.
  wait_for_sunshine_ensure_target(result, std::chrono::seconds(3));
  if (result.ready_for_probe()) {
    BOOST_LOG(info) << "Temporary virtual display ready at " << result.display_name << '.';
  } else {
    BOOST_LOG(warning)
      << "Temporary virtual display remains pending; refusing to probe another output"
      << " (readiness=" << static_cast<int>(result.readiness)
      << ", device_id='" << (result.device_id.empty() ? std::string("<pending>") : result.device_id)
      << "', display_name='" << (result.display_name.empty() ? std::string("<pending>") : result.display_name)
      << "').";
  }
  return result;
}

void VDISPLAY_SUNSHINE::cleanup_ensure_display(const ensure_display_result &result) {
  if (!VDISPLAY::policy::should_cleanup_temporary_probe(result.tracks_temporary_for_probe)) {
    return;
  }

  GUID guid_to_remove {};
  VDISPLAY::policy::probe_display_release_action action =
    VDISPLAY::policy::probe_display_release_action::ignored;
  {
    std::lock_guard<std::mutex> lock(g_ensure_display_state_mutex);

    if (!g_ensure_display_lifetime.retained() ||
        !guid_equal(g_ensure_display_guid, result.temporary_guid)) {
      return;
    }

    action = g_ensure_display_lifetime.release(result.temporary_generation);
    guid_to_remove = g_ensure_display_guid;
  }

  g_ensure_display_state_cv.notify_all();
  if (action != VDISPLAY::policy::probe_display_release_action::remove) {
    return;
  }

  const bool removed = removeVirtualDisplay(guid_to_remove);
  {
    std::lock_guard<std::mutex> lock(g_ensure_display_state_mutex);
    g_ensure_display_lifetime.complete_removal(result.temporary_generation, removed);
    if (removed && !g_ensure_display_lifetime.retained()) {
      std::memset(&g_ensure_display_guid, 0, sizeof(g_ensure_display_guid));
    }
  }
  g_ensure_display_state_cv.notify_all();
  if (!removed) {
    BOOST_LOG(warning) << "Failed to remove temporary virtual display.";
  } else {
    BOOST_LOG(info) << "Removed temporary virtual display.";
  }
}

bool VDISPLAY_SUNSHINE::has_retained_ensure_display() {
  GUID retained_guid {};
  {
    std::lock_guard<std::mutex> lock(g_ensure_display_state_mutex);
    if (!g_ensure_display_lifetime.retained()) {
      return false;
    }
    retained_guid = g_ensure_display_guid;
  }
  return VDISPLAY::policy::retained_target_is_owned(
    is_virtual_display_guid_tracked(retained_guid),
    driver_lease_tracker().get(guid_to_uuid(retained_guid)).has_value()
  );
}

void VDISPLAY_SUNSHINE::cleanup_retained_ensure_display() {
  std::lock_guard<std::mutex> acquire_lock(g_ensure_display_acquire_mutex);
  GUID guid_to_remove {};
  std::uint64_t removal_generation = 0;
  {
    std::lock_guard<std::mutex> lock(g_ensure_display_state_mutex);
    const auto generation = g_ensure_display_lifetime.begin_idle_removal();
    if (!generation) {
      return;
    }
    removal_generation = *generation;
    guid_to_remove = g_ensure_display_guid;
  }

  const bool removed = removeVirtualDisplay(guid_to_remove);
  {
    std::lock_guard<std::mutex> lock(g_ensure_display_state_mutex);
    g_ensure_display_lifetime.complete_removal(removal_generation, removed);
    if (removed && !g_ensure_display_lifetime.retained()) {
      std::memset(&g_ensure_display_guid, 0, sizeof(g_ensure_display_guid));
    }
  }
  g_ensure_display_state_cv.notify_all();
  if (!removed) {
    BOOST_LOG(warning) << "Failed to remove retained Sunshine encoder-probe virtual display.";
    return;
  }
}
