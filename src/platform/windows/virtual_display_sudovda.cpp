#include "virtual_display.h"

#include "virtual_display_recovery_registry.h"

#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/windows/display_helper_coordinator.h"
#include "src/platform/windows/misc.h"
#include "src/process.h"
#include "src/state_storage.h"
#include "src/uuid.h"

#include <algorithm>
#include <atomic>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <cctype>
#include <cfgmgr32.h>
#include <chrono>
#include <cmath>
#include <combaseapi.h>
#include <condition_variable>
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
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <winsock2.h>
#include <windows.h>
#include <winreg.h>
#include <wrl/client.h>

#ifndef FILE_DEVICE_UNKNOWN
  #define FILE_DEVICE_UNKNOWN 0x00000022
#endif

#include <ddk/d4iface.h>
#include <ddk/d4drvif.h>
#include <sudovda/sudovda.h>

#ifndef CPST_EXTENDED_DISPLAY_COLOR_MODE
  // MinGW headers may not expose the extended display color mode constant.
  #define CPST_EXTENDED_DISPLAY_COLOR_MODE 8
#endif

namespace fs = std::filesystem;

using namespace SUDOVDA;

namespace VDISPLAY_SUDOVDA {
  using VDISPLAY::DRIVER_STATUS;
  using VDISPLAY::VirtualDisplayCreationResult;
  using VDISPLAY::VirtualDisplayInfo;
  using VDISPLAY::VirtualDisplayRecoveryParams;
  using VDISPLAY::ensure_display_readiness_e;
  using VDISPLAY::ensure_display_result;

  extern HANDLE SUDOVDA_DRIVER_HANDLE;
  using SudaVDADisplayInfo = VDISPLAY::VirtualDisplayInfo;
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
    bool replace_existing = true
  );
  bool removeVirtualDisplay(const GUID &guid);
  static bool remove_virtual_display_impl(
    const GUID &guid,
    bool cancel_recovery_monitor,
    std::stop_token stop_token = {}
  );
  bool removeAllVirtualDisplays();
  std::optional<std::string> resolveVirtualDisplayDeviceId(const std::wstring &display_name);
  std::optional<std::string> resolveVirtualDisplayDeviceIdForClient(const std::string &client_name);
  std::optional<std::string> resolveActiveVirtualDisplayDeviceId(
    const std::string &preferred_output_identifier,
    const std::string &client_name,
    bool allow_any_fallback
  );
  std::optional<std::string> resolveActiveVirtualDisplayDeviceIdForStableId(
    const std::string &stable_id,
    const std::string &preferred_output_identifier,
    const std::string &client_name,
    bool allow_any_fallback
  );
  std::optional<std::string> resolveAnyVirtualDisplayDeviceId();
  bool is_virtual_display_selection(const std::string &output_identifier);
  uint64_t client_uuid_to_vdd_display_id(const GUID &client_guid);
  GUID sharedVirtualDisplayGuid();
  bool isSudaVDADriverInstalled();
  std::vector<SudaVDADisplayInfo> enumerateSudaVDADisplays();
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

  static bool ensure_driver_is_ready_impl(
    RestartCooldownBehavior cooldown_behavior,
    std::stop_token stop_token = {},
    bool allow_reinstall = true
  );
  static DRIVER_STATUS open_vdisplay_device_impl(std::stop_token stop_token, bool allow_reinstall = true);
  static bool start_ping_thread_impl(std::function<void()> fail_cb, std::stop_token stop_token);
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
    bool replace_existing,
    bool allow_reinstall,
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
    constexpr std::wstring_view SUDOVDA_HARDWARE_ID = L"root\\sudomaker\\sudovda";
    constexpr std::wstring_view SUDOVDA_FRIENDLY_NAME_W = L"SudoMaker Virtual Display Adapter";

    std::atomic<bool> g_watchdog_feed_requested {false};
    std::atomic<bool> g_watchdog_stop_requested {false};
    std::atomic<std::int64_t> g_watchdog_grace_deadline_ns {0};
    std::mutex g_watchdog_thread_mutex;
    std::thread g_watchdog_thread;
    std::function<void()> g_watchdog_fail_cb;
    std::atomic_bool g_watchdog_failure_callback_pending {false};
    std::atomic<std::int64_t> g_last_teardown_ns {0};
    std::atomic<std::int64_t> g_last_restart_failure_ns {0};
    std::atomic<bool> g_reinstall_attempted {false};

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

    // Recovery, stream teardown, and watchdog failure handling all touch the
    // same driver handle. Keep those operations serialized while allowing
    // cancellation to be requested before a remover waits for this lock.
    std::recursive_mutex g_virtual_display_operation_mutex;

    struct render_adapter_request_t {
      std::uint64_t handle_generation = 0;
      LUID luid {};
    };

    struct owned_display_identity_t {
      std::string device_id;
    };

    std::uint64_t g_driver_handle_generation = 0;
    std::optional<render_adapter_request_t> g_current_render_adapter_request;
    std::unordered_map<std::string, render_adapter_request_t> g_render_adapter_request_provenance;
    std::unordered_map<std::string, owned_display_identity_t> g_owned_display_identities;

    bool wait_for_monitor_stop(std::stop_token stop_token, std::chrono::steady_clock::duration duration);

    bool is_ensure_display_client(const char *client_uid) {
      return client_uid && std::strcmp(client_uid, "sunshine-ensure") == 0;
    }

    bool release_retained_ensure_display_for_stream(
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
      GUID guid_to_remove {};
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
        const auto generation = g_ensure_display_lifetime.begin_idle_removal();
        if (!generation) {
          return true;
        }
        removal_generation = *generation;
        guid_to_remove = g_ensure_display_guid;
      }

      BOOST_LOG(info) << "Removing retained SudoVDA encoder-probe display before stream creation.";
      const bool removed = remove_virtual_display_impl(guid_to_remove, true, stop_token);
      {
        std::lock_guard<std::mutex> lock(g_ensure_display_state_mutex);
        g_ensure_display_lifetime.complete_removal(removal_generation, removed);
        if (removed && !g_ensure_display_lifetime.retained()) {
          std::memset(&g_ensure_display_guid, 0, sizeof(g_ensure_display_guid));
        }
      }
      g_ensure_display_state_cv.notify_all();
      if (!removed && !stop_token.stop_requested()) {
        BOOST_LOG(warning) << "Failed to remove retained SudoVDA encoder-probe display before stream creation.";
      }
      return !stop_token.stop_requested();
    }

    bool guid_equal(const GUID &lhs, const GUID &rhs) {
      return std::memcmp(&lhs, &rhs, sizeof(GUID)) == 0;
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
      const auto last_teardown = g_last_teardown_ns.load(std::memory_order_acquire);
      if (last_teardown <= 0) {
        return !stop_token.stop_requested();
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

    void stop_watchdog_thread() {
      g_watchdog_stop_requested.store(true, std::memory_order_release);

      std::thread watchdog_thread;
      {
        std::lock_guard<std::mutex> lock(g_watchdog_thread_mutex);
        if (!g_watchdog_thread.joinable()) {
          return;
        }
        if (g_watchdog_thread.get_id() == std::this_thread::get_id()) {
          // Leave self ownership in static storage for an external lifecycle
          // call to join after this watchdog has returned.
          return;
        }
        watchdog_thread = std::move(g_watchdog_thread);
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

    void dispatch_watchdog_fail_cb(std::shared_ptr<const std::function<void()>> fail_cb) {
      if (!fail_cb || !*fail_cb) {
        return;
      }
      bool expected = false;
      if (!g_watchdog_failure_callback_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        // A callback is already closing this driver. Replacing it would make
        // the watchdog wait on a callback that may itself be joining it.
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
                BOOST_LOG(error) << "SudoVDA watchdog failure callback threw: " << err.what();
              } catch (...) {
                BOOST_LOG(error) << "SudoVDA watchdog failure callback threw an unknown exception.";
              }
              clear_pending();
            }
          )) {
        g_watchdog_failure_callback_pending.store(false, std::memory_order_release);
        BOOST_LOG(error) << "SudoVDA watchdog: failed to queue failure callback.";
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

    bool driver_handle_responsive(HANDLE handle) {
      if (handle == INVALID_HANDLE_VALUE) {
        return false;
      }

      if (!CheckProtocolCompatible(handle)) {
        return false;
      }

      if (!PingDriver(handle)) {
        return false;
      }

      return true;
    }

    bool probe_driver_responsive_once() {
      HANDLE handle = OpenDevice(&SUVDA_INTERFACE_GUID);
      if (handle == INVALID_HANDLE_VALUE) {
        return false;
      }

      const bool responsive = driver_handle_responsive(handle);
      CloseHandle(handle);
      return responsive;
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

    std::optional<std::wstring> find_sudovda_device_instance_id() {
      DevInfoHandle info(SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DIGCF_PRESENT));
      if (!info.valid()) {
        const DWORD err = GetLastError();
        BOOST_LOG(warning) << "Failed to acquire display device info set for SudoVDA lookup (error=" << err << ")";
        return std::nullopt;
      }

      std::vector<wchar_t> hardware_ids;

      for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA device_info {};
        device_info.cbSize = sizeof(device_info);
        if (!SetupDiEnumDeviceInfo(info.get(), index, &device_info)) {
          const DWORD err = GetLastError();
          if (err != ERROR_NO_MORE_ITEMS) {
            BOOST_LOG(warning) << "SetupDiEnumDeviceInfo failed while scanning for SudoVDA (error=" << err << ")";
          }
          break;
        }

        bool matches = false;
        if (load_device_property_multi_sz(info.get(), device_info, SPDRP_HARDWAREID, hardware_ids)) {
          matches = multi_sz_contains_ci(hardware_ids, SUDOVDA_HARDWARE_ID);
        }

        if (!matches) {
          if (auto friendly = load_device_property_string(info.get(), device_info, SPDRP_FRIENDLYNAME)) {
            matches = equals_ci(*friendly, SUDOVDA_FRIENDLY_NAME_W);
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

    /**
     * @brief Attempt to reinstall the SudoVDA driver by running the installer script.
     *
     * This is a last-resort recovery for when the device node is completely missing
     * (e.g., removed during an upgrade and not recreated due to installer guard conditions).
     * Only attempted once per process lifetime.
     */
    bool try_reinstall_sudovda_driver() {
      if (g_reinstall_attempted.exchange(true, std::memory_order_acq_rel)) {
        return false;
      }

      wchar_t module_path[MAX_PATH] = {};
      if (!GetModuleFileNameW(nullptr, module_path, _countof(module_path))) {
        BOOST_LOG(warning) << "SudoVDA reinstall: cannot resolve module path.";
        return false;
      }

      fs::path exe_dir(module_path);
      exe_dir = exe_dir.parent_path();
      fs::path install_script = exe_dir / L"drivers" / L"sudovda" / L"install.ps1";

      if (!fs::exists(install_script)) {
        BOOST_LOG(warning) << "SudoVDA reinstall: installer script not found at " << platf::to_utf8(install_script.wstring());
        return false;
      }

      BOOST_LOG(info) << "SudoVDA device node missing; attempting driver reinstall via " << platf::to_utf8(install_script.wstring());

      std::wstring cmd = L"powershell.exe -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" +
                         install_script.wstring() + L"\"";

      STARTUPINFOW si {};
      si.cb = sizeof(si);
      si.dwFlags = STARTF_USESHOWWINDOW;
      si.wShowWindow = SW_HIDE;
      PROCESS_INFORMATION pi {};

      if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, exe_dir.wstring().c_str(), &si, &pi)) {
        BOOST_LOG(warning) << "SudoVDA reinstall: failed to launch installer (error=" << GetLastError() << ").";
        return false;
      }

      DWORD wait_result = WaitForSingleObject(pi.hProcess, 30000);
      DWORD exit_code = 1;
      if (wait_result == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &exit_code);
      }
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);

      if (wait_result != WAIT_OBJECT_0 || exit_code != 0) {
        BOOST_LOG(warning) << "SudoVDA reinstall: installer exited with code " << exit_code;
        return false;
      }

      BOOST_LOG(info) << "SudoVDA reinstall: installer completed successfully; waiting for device enumeration.";
      std::this_thread::sleep_for(std::chrono::seconds(2));
      return find_sudovda_device_instance_id().has_value();
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
        BOOST_LOG(warning) << "Failed to stage property change for SudoVDA device (state=" << state_change << ", error=" << err << ")";
        return false;
      }

      const BOOL invoked = SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, info_set, &data);
      const DWORD err = invoked ? ERROR_SUCCESS : GetLastError();
      (void) SetupDiSetClassInstallParamsW(info_set, &data, nullptr, 0);

      if (!invoked) {
        if (state_change == DICS_DISABLE && err == ERROR_NOT_DISABLEABLE) {
          BOOST_LOG(info) << "SudoVDA device is not disableable (error=" << err << "); continuing with enable.";
          return true;
        }

        BOOST_LOG(warning) << "Property change request rejected for SudoVDA device (state=" << state_change << ", error=" << err << ")";
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
     * @brief Attempt to re-enable a SudoVDA device that is stuck in the disabled state.
     *
     * Unlike restart_sudovda_device(), this only performs DICS_ENABLE (no disable first)
     * since the device is already disabled.
     */
    bool try_reenable_disabled_device(const std::wstring &instance_id, std::stop_token stop_token) {
      if (stop_token.stop_requested()) {
        return false;
      }
      BOOST_LOG(warning) << "SudoVDA device is stuck disabled (CM_PROB_DISABLED); attempting re-enable.";

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
        BOOST_LOG(error) << "Failed to re-enable disabled SudoVDA device. A reboot may be required.";
        return false;
      }

      // Give the device time to initialize after re-enable
      if (wait_for_monitor_stop(stop_token, DEVICE_RESTART_SETTLE_DELAY * 2)) {
        return false;
      }

      // Verify it's no longer disabled
      if (is_device_disabled(instance_id)) {
        BOOST_LOG(error) << "SudoVDA device still disabled after re-enable attempt. A reboot may be required.";
        return false;
      }

      BOOST_LOG(info) << "SudoVDA device successfully re-enabled from disabled state.";
      return true;
    }

    constexpr int ENABLE_RETRY_MAX = 2;

    bool restart_sudovda_device(const std::wstring &instance_id) {
      DevInfoHandle info(SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES));
      if (!info.valid()) {
        const DWORD err = GetLastError();
        BOOST_LOG(warning) << "Failed to acquire global device info set for SudoVDA restart (error=" << err << ")";
        return false;
      }

      SP_DEVINFO_DATA device_info {};
      device_info.cbSize = sizeof(device_info);
      if (!SetupDiOpenDeviceInfoW(info.get(), instance_id.c_str(), nullptr, 0, &device_info)) {
        const DWORD err = GetLastError();
        BOOST_LOG(warning) << "Failed to open SudoVDA instance " << platf::to_utf8(instance_id) << " (error=" << err << ")";
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

    bool luid_equal(const LUID &lhs, const LUID &rhs) {
      return lhs.LowPart == rhs.LowPart && lhs.HighPart == rhs.HighPart;
    }

    std::optional<render_adapter_request_t> current_render_adapter_request() {
      std::lock_guard<std::recursive_mutex> operation_lock(g_virtual_display_operation_mutex);
      if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE ||
          !g_current_render_adapter_request ||
          g_current_render_adapter_request->handle_generation != g_driver_handle_generation) {
        return std::nullopt;
      }
      return g_current_render_adapter_request;
    }

    bool render_adapter_request_matches(const render_adapter_request_t &expected) {
      const auto current = current_render_adapter_request();
      return current &&
             current->handle_generation == expected.handle_generation &&
             luid_equal(current->luid, expected.luid);
    }

    bool record_render_adapter_request_provenance(
      const uuid_util::uuid_t &guid,
      const render_adapter_request_t &creation_request
    ) {
      std::lock_guard<std::recursive_mutex> operation_lock(g_virtual_display_operation_mutex);
      if (!g_current_render_adapter_request ||
          g_current_render_adapter_request->handle_generation != creation_request.handle_generation ||
          !luid_equal(g_current_render_adapter_request->luid, creation_request.luid)) {
        BOOST_LOG(error) << "Cannot record SudoVDA render-adapter request provenance for guid="
                         << guid.string() << " because the creation-time request is no longer current.";
        return false;
      }
      g_render_adapter_request_provenance[guid.string()] = creation_request;
      return true;
    }

    void erase_render_adapter_request_provenance(const uuid_util::uuid_t &guid) {
      std::lock_guard<std::recursive_mutex> operation_lock(g_virtual_display_operation_mutex);
      g_render_adapter_request_provenance.erase(guid.string());
      g_owned_display_identities.erase(guid.string());
    }

    bool has_render_adapter_request_provenance(const uuid_util::uuid_t &guid) {
      std::lock_guard<std::recursive_mutex> operation_lock(g_virtual_display_operation_mutex);
      return g_render_adapter_request_provenance.contains(guid.string());
    }

    std::vector<uuid_util::uuid_t> owned_render_adapter_request_provenance_guids() {
      std::lock_guard<std::recursive_mutex> operation_lock(g_virtual_display_operation_mutex);
      std::vector<uuid_util::uuid_t> result;
      result.reserve(g_render_adapter_request_provenance.size());
      for (const auto &[guid_string, _] : g_render_adapter_request_provenance) {
        try {
          result.push_back(uuid_util::uuid_t::parse(guid_string));
        } catch (...) {
          BOOST_LOG(error) << "Ignoring malformed in-process SudoVDA ownership GUID '" << guid_string << "'.";
        }
      }
      return result;
    }

    bool render_adapter_request_provenance_matches(
      const uuid_util::uuid_t &guid,
      const std::string_view context
    ) {
      std::lock_guard<std::recursive_mutex> operation_lock(g_virtual_display_operation_mutex);
      const auto provenance = g_render_adapter_request_provenance.find(guid.string());
      const bool matches =
        SUDOVDA_DRIVER_HANDLE != INVALID_HANDLE_VALUE &&
        g_current_render_adapter_request &&
        g_current_render_adapter_request->handle_generation == g_driver_handle_generation &&
        provenance != g_render_adapter_request_provenance.end() &&
        provenance->second.handle_generation == g_driver_handle_generation &&
        luid_equal(provenance->second.luid, g_current_render_adapter_request->luid);
      if (!matches) {
        BOOST_LOG(error) << "SudoVDA render-adapter request provenance is absent or stale for "
                         << context << " (guid=" << guid.string()
                         << "); this records accepted requests, not the adapter observed by AssignSwapChain.";
      }
      return matches;
    }

    bool render_adapter_request_provenance_matches(
      const uuid_util::uuid_t &guid,
      const LUID &requested_luid,
      const std::string_view context
    ) {
      std::lock_guard<std::recursive_mutex> operation_lock(g_virtual_display_operation_mutex);
      const auto provenance = g_render_adapter_request_provenance.find(guid.string());
      const bool matches =
        SUDOVDA_DRIVER_HANDLE != INVALID_HANDLE_VALUE &&
        g_current_render_adapter_request &&
        g_current_render_adapter_request->handle_generation == g_driver_handle_generation &&
        luid_equal(g_current_render_adapter_request->luid, requested_luid) &&
        provenance != g_render_adapter_request_provenance.end() &&
        provenance->second.handle_generation == g_driver_handle_generation &&
        luid_equal(provenance->second.luid, requested_luid);
      if (!matches) {
        BOOST_LOG(error) << "SudoVDA render-adapter request provenance does not match the configured request for "
                         << context << " (guid=" << guid.string()
                         << "); this comparison does not observe AssignSwapChain.";
      }
      return matches;
    }

    bool track_virtual_display_created(
      const uuid_util::uuid_t &guid,
      const VirtualDisplayCreationResult &result
    ) {
      std::lock_guard<std::recursive_mutex> operation_lock(g_virtual_display_operation_mutex);
      if (!render_adapter_request_provenance_matches(guid, "SudoVDA virtual display publication")) {
        return false;
      }
      if (!result.device_id || result.device_id->empty()) {
        BOOST_LOG(error) << "Cannot publish SudoVDA virtual display ownership for guid="
                         << guid.string() << " because no exact device identity was resolved.";
        return false;
      }
      g_owned_display_identities[guid.string()] = owned_display_identity_t {
        .device_id = *result.device_id,
      };
      active_virtual_display_tracker().add(guid);
      return true;
    }

    void track_virtual_display_removed(const uuid_util::uuid_t &guid) {
      active_virtual_display_tracker().remove(guid);
      erase_render_adapter_request_provenance(guid);
    }

    bool is_virtual_display_guid_tracked(const uuid_util::uuid_t &guid) {
      return active_virtual_display_tracker().contains(guid);
    }

    std::vector<uuid_util::uuid_t> collect_conflicting_virtual_displays(const uuid_util::uuid_t &guid) {
      auto result = active_virtual_display_tracker().other_than(guid);
      for (const auto &owned : owned_render_adapter_request_provenance_guids()) {
        if (!(owned == guid) &&
            std::find(result.begin(), result.end(), owned) == result.end()) {
          result.push_back(owned);
        }
      }
      return result;
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
      // Format: \\?\DISPLAY#SMKD1CE#1&28a6823a&2&UID265#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}
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

    std::optional<uint32_t> read_virtual_display_dpi_value() {
      HKEY root = nullptr;
      if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Control Panel\\Desktop\\PerMonitorSettings",
            0,
            KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE,
            &root
          ) != ERROR_SUCCESS) {
        return std::nullopt;
      }

      std::optional<uint32_t> result;
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
        if (name_len < 3 || std::wcsncmp(name, L"SMK", 3) != 0) {
          continue;
        }

        DWORD value = 0;
        DWORD value_size = sizeof(value);
        const LSTATUS query_status = RegGetValueW(root, name, L"DpiValue", RRF_RT_REG_DWORD, nullptr, &value, &value_size);
        if (query_status == ERROR_SUCCESS) {
          result = value;
          break;
        }
      }

      RegCloseKey(root);
      return result;
    }

    bool apply_virtual_display_dpi_value(uint32_t value) {
      HKEY root = nullptr;
      if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Control Panel\\Desktop\\PerMonitorSettings",
            0,
            KEY_ENUMERATE_SUB_KEYS | KEY_SET_VALUE,
            &root
          ) != ERROR_SUCCESS) {
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
        if (name_len < 3 || std::wcsncmp(name, L"SMK", 3) != 0) {
          continue;
        }

        HKEY subkey = nullptr;
        if (RegOpenKeyExW(root, name, 0, KEY_SET_VALUE, &subkey) != ERROR_SUCCESS) {
          continue;
        }

        const DWORD data = value;
        const auto status = RegSetValueExW(
          subkey,
          L"DpiValue",
          0,
          REG_DWORD,
          reinterpret_cast<const BYTE *>(&data),
          sizeof(data)
        );
        RegCloseKey(subkey);
        if (status == ERROR_SUCCESS) {
          applied = true;
        }
      }

      RegCloseKey(root);
      if (applied) {
        printf("[SUDOVDA] Applied cached virtual display DPI value: %u\n", static_cast<unsigned int>(value));
      }
      return applied;
    }

    fs::path legacy_virtual_display_cache_path() {
      return platf::appdata() / "virtual_display_cache.json";
    }

    namespace pt = boost::property_tree;

    bool is_virtual_display_device(const display_device::EnumeratedDevice &device) {
      const auto contains_ci = [](const std::string &haystack, const std::string &needle) {
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
      };

      if (!device.m_monitor_device_path.empty()) {
        // This is the most reliable signal (device instance path contains the driver stack identifiers).
        if (contains_ci(device.m_monitor_device_path, "SUDOVDA") ||
            contains_ci(device.m_monitor_device_path, "SUDOMAKER")) {
          return true;
        }
      }

      // Fallback: some environments may return an adapter-like friendly name instead of the per-display name.
      static const std::string sudoMakerDeviceString = "SudoMaker Virtual Display Adapter";
      if (equals_ci(device.m_friendly_name, sudoMakerDeviceString)) {
        return true;
      }

      // Fallback: SudoVDA's synthetic EDID commonly uses manufacturer "SMK" (SudoMaker).
      if (device.m_edid && equals_ci(device.m_edid->m_manufacturer_id, "SMK")) {
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

    std::optional<DisplayConfigIdentity> query_display_config_identity_inner(const VIRTUAL_DISPLAY_ADD_OUT &output) {
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

    std::optional<DisplayConfigIdentity> query_display_config_identity(const VIRTUAL_DISPLAY_ADD_OUT &output) {
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
      // Try without impersonation first (faster if already in user context)
      for (int i = 0; i < attempts; ++i) {
        if (stop_token.stop_requested()) {
          return std::nullopt;
        }
        if (auto path = resolve_monitor_device_path_once(display_name, device_id, client_name)) {
          return path;
        }
        if (i + 1 < attempts) {
          if (wait_for_monitor_stop(stop_token, delay)) {
            return std::nullopt;
          }
        }
      }

      if (stop_token.stop_requested()) {
        return std::nullopt;
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
          if (i + 1 < attempts) {
            if (wait_for_monitor_stop(stop_token, delay)) {
              return;
            }
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
    constexpr auto RECOVERY_INACTIVE_GRACE = std::chrono::seconds(1);
    constexpr auto RECOVERY_NO_ACTIVE_GRACE = std::chrono::seconds(10);
    constexpr auto RECOVERY_POST_SUCCESS_GRACE = std::chrono::seconds(3);
    constexpr auto RECOVERY_MAX_ATTEMPTS_BACKOFF = std::chrono::seconds(5);
    constexpr auto RECOVERY_MAX_BACKOFF = std::chrono::seconds(60);
    constexpr auto DRIVER_RECOVERY_WARMUP_DELAY = std::chrono::milliseconds(500);

    vdisplay_recovery::monitor_registry_t &recovery_monitors() {
      // A function-local registry is destroyed before later-initialized driver
      // globals if normal shutdown was bypassed.
      static vdisplay_recovery::monitor_registry_t monitors;
      return monitors;
    }

    void abort_recovery_monitor(const uuid_util::uuid_t &guid_uuid) {
      // Do not join here: a caller may be about to acquire the operation lock
      // while the monitor is waiting to acquire that same lock.
      recovery_monitors().request_stop(guid_uuid.string());
      deferred_hdr_profile_workers().request_stop(guid_uuid.string());
    }

    void abort_all_recovery_monitors() {
      recovery_monitors().request_stop_all();
      deferred_hdr_profile_workers().request_stop_all();
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
      present_inactive,
      present_active,
      unknown,
    };

    const char *monitor_target_presence_name(const MonitorTargetPresence presence) {
      switch (presence) {
        case MonitorTargetPresence::missing:
          return "missing";
        case MonitorTargetPresence::present_inactive:
          return "inactive";
        case MonitorTargetPresence::present_active:
          return "active";
        case MonitorTargetPresence::unknown:
          return "unknown";
      }
      return "unknown";
    }

    MonitorTargetPresence monitor_target_presence(RecoveryMonitorState &state) {
      auto devices = platf::display_helper::Coordinator::instance().enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
      if (!devices || devices->empty()) {
        // Either enumeration failed outright (nullopt) or the CCD subsystem returned no valid
        // paths (empty vector, e.g. during transient topology churn).  Treat both as "unknown"
        // rather than letting an empty list fall through to "missing".
        return MonitorTargetPresence::unknown;
      }

      bool matched_inactive = false;
      for (const auto &device : *devices) {
        if (!is_virtual_display_device(device)) {
          continue;
        }

        bool matches = false;
        bool matched_by_client_name = false;
        if (!matches && !state.params.client_name.empty() && !device.m_friendly_name.empty() && equals_ci(device.m_friendly_name, state.params.client_name)) {
          matches = true;
          matched_by_client_name = true;
        }
        if (!matches && state.normalized_monitor_device_path && !device.m_monitor_device_path.empty()) {
          const auto normalized_path = normalize_display_name(device.m_monitor_device_path);
          if (!normalized_path.empty() && normalized_path == *state.normalized_monitor_device_path) {
            matches = true;
          }
        }
        if (!matches && state.current_device_id && !state.current_device_id->empty() && !device.m_device_id.empty()) {
          matches = equals_ci(device.m_device_id, *state.current_device_id);
        }
        if (!matches && state.normalized_display_name) {
          auto normalized_display = normalize_display_name(device.m_display_name);
          if (!normalized_display.empty() && normalized_display == *state.normalized_display_name) {
            matches = true;
          } else {
            auto normalized_friendly = normalize_display_name(device.m_friendly_name);
            if (!normalized_friendly.empty() && normalized_friendly == *state.normalized_display_name) {
              matches = true;
            }
          }
        }
        if (!matches) {
          continue;
        }

        if (matched_by_client_name) {
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

          if (adopted_display_name != state.current_display_name || adopted_device_id != state.current_device_id || adopted_monitor_device_path != state.current_monitor_device_path) {
            const auto before = state.describe_target();
            state.update_identifiers(adopted_display_name, adopted_device_id, adopted_monitor_device_path);
            BOOST_LOG(debug) << "Virtual display recovery monitor adopted updated identifiers via client_name '"
                             << state.params.client_name << "': " << before << " -> " << state.describe_target();
          }
        }

        const bool is_active = !device.m_display_name.empty();
        if (is_active) {
          return MonitorTargetPresence::present_active;
        }
        matched_inactive = true;
      }

      return matched_inactive ? MonitorTargetPresence::present_inactive : MonitorTargetPresence::missing;
    }

    bool attempt_virtual_display_recovery(RecoveryMonitorState &state, std::stop_token stop_token) {
      if (!release_retained_ensure_display_for_stream(
            state.params.client_uid.c_str(),
            stop_token)) {
        return false;
      }
      std::unique_lock<std::recursive_mutex> operation_lock(g_virtual_display_operation_mutex, std::defer_lock);
      if (!lock_recovery_operation(operation_lock, state, stop_token)) {
        return false;
      }
      if (!ensure_driver_is_ready_impl(RestartCooldownBehavior::skip, stop_token, false)) {
        BOOST_LOG(warning) << "Virtual display recovery: driver not ready for " << state.describe_target();
        return false;
      }
      if (monitor_should_abort(state, stop_token)) {
        return false;
      }

      proc::vDisplayDriverStatus.store(open_vdisplay_device_impl(stop_token, false), std::memory_order_release);
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
        if (!start_ping_thread_impl(std::move(watchdog_fail_cb), stop_token)) {
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
        true,
        false,
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
      auto recovery_cooldown_until = std::chrono::steady_clock::now();

      while (true) {
        if (monitor_should_abort(state, stop_token)) {
          BOOST_LOG(debug) << "Virtual display recovery monitor aborted for " << state.describe_target();
          return;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto presence = monitor_target_presence(state);

        if (presence == MonitorTargetPresence::unknown) {
          if (wait_for_monitor_stop(stop_token, RECOVERY_CHECK_INTERVAL)) {
            return;
          }
          continue;
        }

        if (presence == MonitorTargetPresence::present_active) {
          observed_active = true;
          backoff_cycles = 0;
          missing_since.reset();
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

        // Defer recovery attempts for a short grace window after a successful recovery. This allows
        // the display stack and helper APPLY to stabilize without immediately retriggering recovery.
        if (now < recovery_cooldown_until) {
          if (presence == MonitorTargetPresence::missing) {
            missing_since.reset();
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
          issue_since = &missing_since;
          required_grace = RECOVERY_MISSING_GRACE;
          issue_label = "missing";
        } else {
          missing_since.reset();
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

    const auto initial_presence = monitor_target_presence(initial_state);
    if (initial_presence != MonitorTargetPresence::present_active) {
      BOOST_LOG(info) << "Virtual display recovery monitor not armed for " << initial_state.describe_target()
                      << ": display was not confirmed active at schedule time (presence="
                      << monitor_target_presence_name(initial_presence) << ").";
      return;
    }

    RecoveryMonitorState state(params);
    state.confirmed_active_at_schedule = true;
    BOOST_LOG(debug) << "Virtual display recovery monitor scheduled for " << state.describe_target()
                     << " (max_attempts=" << params.max_attempts << ").";
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

  HANDLE SUDOVDA_DRIVER_HANDLE = INVALID_HANDLE_VALUE;

  void closeVDisplayDevice() {
    std::lock_guard<std::recursive_mutex> operation_lock(g_virtual_display_operation_mutex);
    g_watchdog_stop_requested.store(true, std::memory_order_release);
    stop_watchdog_thread();
    g_current_render_adapter_request.reset();
    if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
      setWatchdogFeedingEnabled(false);
      return;
    }

    setWatchdogFeedingEnabled(false);
    g_watchdog_grace_deadline_ns.store(0, std::memory_order_release);
    CloseHandle(SUDOVDA_DRIVER_HANDLE);

    SUDOVDA_DRIVER_HANDLE = INVALID_HANDLE_VALUE;
  }

  void ensureVirtualDisplayRegistryDefaults() {
    constexpr const wchar_t *REG_PATH = L"SOFTWARE\\SudoMaker\\SudoVDA";
    HKEY key = nullptr;
    REGSAM access = KEY_WRITE;
#ifdef KEY_WOW64_64KEY
    access |= KEY_WOW64_64KEY;
#endif
    DWORD disposition = 0;
    const LSTATUS status = RegCreateKeyExW(
      HKEY_LOCAL_MACHINE,
      REG_PATH,
      0,
      nullptr,
      REG_OPTION_NON_VOLATILE,
      access,
      nullptr,
      &key,
      &disposition
    );
    if (status != ERROR_SUCCESS) {
      BOOST_LOG(warning) << "Failed to create SudoVDA registry key (status=" << status << ")";
      return;
    }

    auto set_dword = [key](const wchar_t *name, DWORD value) {
      const DWORD data = value;
      const LSTATUS set_status = RegSetValueExW(
        key,
        name,
        0,
        REG_DWORD,
        reinterpret_cast<const BYTE *>(&data),
        sizeof(data)
      );
      if (set_status != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "Failed to set SudoVDA registry value "
                           << platf::to_utf8(std::wstring(name))
                           << " (status=" << set_status << ")";
      }
    };

    set_dword(L"sdrBits", 10);
    set_dword(L"hdrBits", 12);

    RegCloseKey(key);
  }

  static DRIVER_STATUS open_vdisplay_device_impl(std::stop_token stop_token, bool allow_reinstall) {
    std::lock_guard<std::recursive_mutex> operation_lock(g_virtual_display_operation_mutex);
    if (stop_token.stop_requested()) {
      return DRIVER_STATUS::FAILED;
    }

    // Recovery calls ensure_driver_is_ready_impl() before reopening. If its
    // existing handle is still healthy, reusing it avoids overwriting the
    // global handle and leaking the old kernel handle. If it is stale, close
    // it before assigning a newly opened replacement below.
    if (SUDOVDA_DRIVER_HANDLE != INVALID_HANDLE_VALUE) {
      if (driver_handle_responsive(SUDOVDA_DRIVER_HANDLE)) {
        return DRIVER_STATUS::OK;
      }
      closeVDisplayDevice();
      if (stop_token.stop_requested()) {
        return DRIVER_STATUS::FAILED;
      }
    }

    uint32_t retryInterval = 20;
    bool attempted_recovery = false;
    while (true) {
      if (stop_token.stop_requested()) {
        return DRIVER_STATUS::FAILED;
      }
      SUDOVDA_DRIVER_HANDLE = OpenDevice(&SUVDA_INTERFACE_GUID);
      if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
        if (retryInterval > 320) {
          if (!attempted_recovery) {
            attempted_recovery = true;
            if (ensure_driver_is_ready_impl(RestartCooldownBehavior::wait, stop_token, allow_reinstall)) {
              retryInterval = 20;
              continue;
            }
          }

          printf("[SUDOVDA] Open device failed!\n");
          return DRIVER_STATUS::FAILED;
        }
        retryInterval *= 2;
        if (wait_for_monitor_stop(stop_token, std::chrono::milliseconds(retryInterval))) {
          return DRIVER_STATUS::FAILED;
        }
        continue;
      }

      break;
    }

    if (stop_token.stop_requested()) {
      closeVDisplayDevice();
      return DRIVER_STATUS::FAILED;
    }

    if (!CheckProtocolCompatible(SUDOVDA_DRIVER_HANDLE)) {
      printf("[SUDOVDA] SUDOVDA protocol not compatible with driver!\n");
      closeVDisplayDevice();
      return DRIVER_STATUS::VERSION_INCOMPATIBLE;
    }

    ++g_driver_handle_generation;
    g_current_render_adapter_request.reset();
    return DRIVER_STATUS::OK;
  }

  DRIVER_STATUS openVDisplayDevice() {
    return open_vdisplay_device_impl({});
  }

  static bool ensure_driver_is_ready_impl(
    RestartCooldownBehavior cooldown_behavior,
    std::stop_token stop_token,
    bool allow_reinstall
  ) {
    std::lock_guard<std::recursive_mutex> operation_lock(g_virtual_display_operation_mutex);
    if (stop_token.stop_requested()) {
      return false;
    }
    if (driver_handle_responsive(SUDOVDA_DRIVER_HANDLE)) {
      return true;
    }

    if (SUDOVDA_DRIVER_HANDLE != INVALID_HANDLE_VALUE) {
      closeVDisplayDevice();
    }

    if (probe_driver_responsive_once()) {
      return true;
    }

    // Check if the device is stuck in the disabled state (CM_PROB_DISABLED)
    // before attempting a full restart cycle, which would make things worse.
    {
      auto instance_id = find_sudovda_device_instance_id();
      if (instance_id && is_device_disabled(*instance_id)) {
        if (try_reenable_disabled_device(*instance_id, stop_token)) {
          if (stop_token.stop_requested()) {
            return false;
          }
          if (probe_driver_responsive_once()) {
            BOOST_LOG(info) << "SudoVDA driver responded after re-enabling disabled device.";
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
          BOOST_LOG(warning) << "Skipping SudoVDA restart attempt due to recent failure (cooldown "
                             << cooldown_remaining.count() << " ms remaining).";
          return false;
        }

        BOOST_LOG(info) << "Delaying SudoVDA restart attempt for " << cooldown_remaining.count()
                        << " ms due to restart cooldown.";
        if (wait_for_monitor_stop(stop_token, cooldown_remaining)) {
          return false;
        }
        if (probe_driver_responsive_once()) {
          return true;
        }
      }

      auto instance_id = find_sudovda_device_instance_id();
      if (!instance_id) {
        // Device node is completely missing. Attempt to reinstall the driver
        // as a last resort before giving up (once per process lifetime).
        if (!allow_reinstall) {
          BOOST_LOG(warning) << "SudoVDA device node is missing during cancellation-aware recovery; refusing to reinstall it automatically.";
          return false;
        }
        if (try_reinstall_sudovda_driver()) {
          BOOST_LOG(info) << "SudoVDA device node restored via reinstall; retrying recovery.";
          instance_id = find_sudovda_device_instance_id();
        }
        if (!instance_id) {
          BOOST_LOG(error) << "Unable to locate SudoVDA adapter for recovery; streaming will continue with the active display. A reboot may be required.";
          note_restart_failure(std::chrono::steady_clock::now());
          return false;
        }
      }

      BOOST_LOG(info) << "Attempting to restart SudoVDA adapter " << platf::to_utf8(*instance_id) << " (attempt "
                      << attempt << '/' << DRIVER_RESTART_MAX_ATTEMPTS << ").";

      if (stop_token.stop_requested()) {
        return false;
      }
      if (!restart_sudovda_device(*instance_id)) {
        BOOST_LOG(error) << "SudoVDA adapter restart failed; streaming will continue with the active display. A reboot may be required.";
        note_restart_failure(std::chrono::steady_clock::now());
        continue;
      }
      // Once restart_sudovda_device() has disabled the device it must complete
      // its enable sequence, but cancellation is honored immediately after.
      if (stop_token.stop_requested()) {
        return false;
      }

      const auto deadline = std::chrono::steady_clock::now() + DRIVER_RESTART_TIMEOUT;
      while (std::chrono::steady_clock::now() < deadline) {
        if (stop_token.stop_requested()) {
          return false;
        }
        if (probe_driver_responsive_once()) {
          BOOST_LOG(info) << "SudoVDA driver responded after restart.";
          if (wait_for_monitor_stop(stop_token, DRIVER_RECOVERY_WARMUP_DELAY)) {
            return false;
          }
          return true;
        }
        if (wait_for_monitor_stop(stop_token, DRIVER_RESTART_POLL_INTERVAL)) {
          return false;
        }
      }

      BOOST_LOG(error) << "SudoVDA driver did not respond within the restart timeout; streaming will continue with the active display. A reboot may be required.";
      note_restart_failure(std::chrono::steady_clock::now());
    }

    return false;
  }

  bool ensure_driver_is_ready() {
    return ensure_driver_is_ready_impl(RestartCooldownBehavior::skip);
  }

  static bool start_ping_thread_impl(std::function<void()> failCb, std::stop_token stop_token) {
    std::lock_guard<std::recursive_mutex> operation_lock(g_virtual_display_operation_mutex);
    if (stop_token.stop_requested()) {
      return false;
    }
    stop_watchdog_thread();
    if (stop_token.stop_requested()) {
      return false;
    }

    // Save the callback so recovery can restart the ping thread with the same callback.
    store_watchdog_fail_cb(failCb);
    auto failure_cb = std::make_shared<std::function<void()>>(std::move(failCb));

    if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
      return false;
    }

    HANDLE ping_handle = INVALID_HANDLE_VALUE;
    if (!DuplicateHandle(
          GetCurrentProcess(),
          SUDOVDA_DRIVER_HANDLE,
          GetCurrentProcess(),
          &ping_handle,
          0,
          FALSE,
          DUPLICATE_SAME_ACCESS
        )) {
      printf("[SUDOVDA] Watchdog: Failed to duplicate driver handle.\n");
      return false;
    }
    if (stop_token.stop_requested()) {
      CloseHandle(ping_handle);
      return false;
    }

    VIRTUAL_DISPLAY_GET_WATCHDOG_OUT watchdogOut;
    if (GetWatchdogTimeout(ping_handle, watchdogOut)) {
      printf("[SUDOVDA] Watchdog: Timeout %d, Countdown %d\n", watchdogOut.Timeout, watchdogOut.Countdown);
    } else {
      printf("[SUDOVDA] Watchdog fetch failed!\n");
      CloseHandle(ping_handle);
      return false;
    }

    if (!watchdogOut.Timeout) {
      CloseHandle(ping_handle);
      return !stop_token.stop_requested();
    }

    if (stop_token.stop_requested()) {
      CloseHandle(ping_handle);
      return false;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto deadline = now + WATCHDOG_INIT_GRACE;
    g_watchdog_stop_requested.store(false, std::memory_order_release);
    g_watchdog_grace_deadline_ns.store(steady_ticks_from_time(deadline), std::memory_order_release);
    g_watchdog_feed_requested.store(false, std::memory_order_release);

    const auto interval_ms = std::max<long long>(static_cast<long long>(watchdogOut.Timeout) * 1000ll / 3ll, 100ll);
    const auto sleep_duration = std::chrono::milliseconds(interval_ms);

    std::thread ping_thread([sleep_duration, failure_cb = std::move(failure_cb), ping_handle, stop_token] {
      auto close_ping_handle = [ping_handle]() {
        if (ping_handle != INVALID_HANDLE_VALUE) {
          CloseHandle(ping_handle);
        }
      };
      const auto wait_for_watchdog_stop = [stop_token](std::chrono::steady_clock::duration duration) {
        const auto deadline = std::chrono::steady_clock::now() + std::max(duration, std::chrono::steady_clock::duration::zero());
        while (!stop_token.stop_requested() && !g_watchdog_stop_requested.load(std::memory_order_acquire)) {
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
      };
      uint8_t fail_count = 0;
      for (;;) {
        if (stop_token.stop_requested() || g_watchdog_stop_requested.load(std::memory_order_acquire)) {
          close_ping_handle();
          return;
        }

        const auto now_tp = std::chrono::steady_clock::now();
        bool should_feed = g_watchdog_feed_requested.load(std::memory_order_acquire);
        if (!should_feed && within_grace_period(now_tp)) {
          should_feed = true;
        }

        if (!should_feed) {
          if (wait_for_watchdog_stop(sleep_duration)) {
            close_ping_handle();
            return;
          }
          continue;
        }

        if (!PingDriver(ping_handle)) {
          fail_count += 1;
          if (fail_count > 3) {
            close_ping_handle();
            if (!stop_token.stop_requested()) {
              BOOST_LOG(error) << "SudoVDA watchdog ping failed repeatedly; dispatching failure recovery.";
              dispatch_watchdog_fail_cb(failure_cb);
            }
            return;
          }
        } else {
          fail_count = 0;
        }

        if (wait_for_watchdog_stop(sleep_duration)) {
          close_ping_handle();
          return;
        }
      }
    });

    {
      std::lock_guard<std::mutex> lock(g_watchdog_thread_mutex);
      g_watchdog_thread = std::move(ping_thread);
    }

    return true;
  }

  bool startPingThread(std::function<void()> failCb) {
    return start_ping_thread_impl(std::move(failCb), {});
  }

  void setWatchdogFeedingEnabled(bool enable) {
    if (enable) {
      const auto deadline = std::chrono::steady_clock::now() + WATCHDOG_INIT_GRACE;
      g_watchdog_grace_deadline_ns.store(steady_ticks_from_time(deadline), std::memory_order_release);
    }
    g_watchdog_feed_requested.store(enable, std::memory_order_release);
  }

  bool setRenderAdapterByLuid(
    const LUID &adapter_luid,
    const std::wstring &adapter_name,
    const std::uint64_t dedicated_video_memory,
    const std::uint64_t shared_system_memory
  ) {
    std::lock_guard<std::recursive_mutex> operation_lock(g_virtual_display_operation_mutex);
    if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
      return false;
    }
    if (g_current_render_adapter_request &&
        g_current_render_adapter_request->handle_generation == g_driver_handle_generation &&
        luid_equal(g_current_render_adapter_request->luid, adapter_luid)) {
      BOOST_LOG(debug) << "SudoVDA render-adapter request for '"
                       << platf::to_utf8(adapter_name)
                       << "' is already current for this driver handle generation.";
      return true;
    }
    // The SudoVDA IOCTL updates driver preference before all downstream work
    // has completed, so a failure leaves the prior request state ambiguous.
    g_current_render_adapter_request.reset();
    if (!SetRenderAdapter(SUDOVDA_DRIVER_HANDLE, adapter_luid)) {
      BOOST_LOG(warning) << "SudoVDA driver rejected exact render adapter '"
                         << platf::to_utf8(adapter_name) << "'.";
      return false;
    }
    g_current_render_adapter_request = render_adapter_request_t {
      .handle_generation = g_driver_handle_generation,
      .luid = adapter_luid,
    };

    BOOST_LOG(info)
      << "SudoVDA virtual display render-adapter request accepted for '"
      << platf::to_utf8(adapter_name)
      << "' (dedicated=" << dedicated_video_memory / (1024ull * 1024ull)
      << " MiB, shared=" << shared_system_memory / (1024ull * 1024ull)
      << " MiB). This confirms the request, not the adapter later observed by AssignSwapChain.";
    return true;
  }

  bool setRenderAdapterByName(const std::wstring &adapterName) {
    std::lock_guard<std::recursive_mutex> operation_lock(g_virtual_display_operation_mutex);
    if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
      return false;
    }

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (!SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
      return false;
    }

    for (UINT index = 0;; ++index) {
      Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
      const HRESULT enum_result = factory->EnumAdapters1(index, adapter.GetAddressOf());
      if (enum_result == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      if (FAILED(enum_result) || !adapter) {
        BOOST_LOG(warning) << "DXGI adapter enumeration failed while resolving the configured SudoVDA adapter.";
        return false;
      }

      DXGI_ADAPTER_DESC1 desc {};
      if (FAILED(adapter->GetDesc1(&desc))) {
        BOOST_LOG(warning) << "DXGI adapter description query failed while resolving the configured SudoVDA adapter.";
        return false;
      }
      if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
        continue;
      }

      if (std::wstring_view(desc.Description) != adapterName) {
        continue;
      }

      return setRenderAdapterByLuid(
        desc.AdapterLuid,
        desc.Description,
        desc.DedicatedVideoMemory,
        desc.SharedSystemMemory
      );
    }

    return false;
  }

  bool setRenderAdapterWithMostDedicatedMemory() {
    std::lock_guard<std::recursive_mutex> operation_lock(g_virtual_display_operation_mutex);
    if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
      return false;
    }

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (!SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
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
        BOOST_LOG(warning) << "DXGI adapter enumeration failed during SudoVDA auto-selection.";
        return false;
      }

      DXGI_ADAPTER_DESC1 desc {};
      if (FAILED(adapter->GetDesc1(&desc))) {
        BOOST_LOG(warning) << "DXGI adapter description query failed during SudoVDA auto-selection.";
        return false;
      }
      if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
        continue;
      }

      SIZE_T dedicated = desc.DedicatedVideoMemory;
      SIZE_T shared = desc.SharedSystemMemory;
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

    return setRenderAdapterByLuid(
      best_luid,
      best_name,
      best_dedicated,
      best_shared
    );
  }

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

  bool apply_configured_render_adapter_preference(std::string_view context = "display ensure") {
    return VDISPLAY::applyConfiguredRenderAdapterPreference(context);
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

    struct exact_virtual_display_identity_t {
      std::string device_id;
      std::optional<std::wstring> display_name;
      std::optional<std::wstring> monitor_device_path;
    };

    std::optional<exact_virtual_display_identity_t> wait_for_exact_active_device_id(
      const std::string &expected_device_id,
      const uint32_t width,
      const uint32_t height,
      std::stop_token stop_token
    ) {
      if (expected_device_id.empty()) {
        return std::nullopt;
      }

      constexpr auto kIdentityTimeout = std::chrono::seconds(2);
      constexpr auto kPollInterval = std::chrono::milliseconds(50);
      const auto deadline = std::chrono::steady_clock::now() + kIdentityTimeout;
      while (!stop_token.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        const auto devices =
          platf::display_helper::Coordinator::instance().enumerate_devices(
            display_device::DeviceEnumerationDetail::Minimal
          );
        if (devices) {
          const display_device::EnumeratedDevice *exact_match = nullptr;
          for (const auto &device : *devices) {
            if (!is_virtual_display_device(device) ||
                device.m_device_id.empty() ||
                !equals_ci(device.m_device_id, expected_device_id)) {
              continue;
            }
            if (exact_match) {
              BOOST_LOG(error) << "Exact SudoVDA device identity is ambiguous: device_id='"
                               << expected_device_id << "'.";
              return std::nullopt;
            }
            exact_match = &device;
          }

          if (exact_match &&
              exact_match->m_info &&
              !exact_match->m_display_name.empty() &&
              exact_match->m_info->m_resolution.m_width == width &&
              exact_match->m_info->m_resolution.m_height == height) {
            return exact_virtual_display_identity_t {
              .device_id = exact_match->m_device_id,
              .display_name = platf::from_utf8(exact_match->m_display_name),
              .monitor_device_path = exact_match->m_monitor_device_path.empty() ?
                                       std::nullopt :
                                       std::make_optional(platf::from_utf8(exact_match->m_monitor_device_path)),
            };
          }
        }

        if (wait_for_monitor_stop(stop_token, kPollInterval)) {
          return std::nullopt;
        }
      }

      return std::nullopt;
    }

    std::optional<exact_virtual_display_identity_t> wait_for_added_display_identity(
      const VIRTUAL_DISPLAY_ADD_OUT &output,
      const uint32_t width,
      const uint32_t height,
      std::stop_token stop_token
    ) {
      constexpr auto kIdentityTimeout = std::chrono::seconds(2);
      constexpr auto kPollInterval = std::chrono::milliseconds(50);
      const auto deadline = std::chrono::steady_clock::now() + kIdentityTimeout;

      std::optional<DisplayConfigIdentity> display_config_identity;
      std::optional<std::wstring> added_gdi_name;
      while (!stop_token.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        if (auto current_identity = query_display_config_identity(output)) {
          display_config_identity = std::move(current_identity);
        }

        wchar_t added_name[CCHDEVICENAME] {};
        if (GetAddedDisplayName(output, added_name) && added_name[0] != L'\0') {
          added_gdi_name = std::wstring(added_name);
        }

        const std::optional<std::string> monitor_path =
          display_config_identity &&
              display_config_identity->monitor_device_path &&
              !display_config_identity->monitor_device_path->empty() ?
            std::make_optional(platf::to_utf8(*display_config_identity->monitor_device_path)) :
            std::nullopt;
        std::vector<std::string> gdi_names;
        if (display_config_identity &&
            display_config_identity->source_gdi_device_name &&
            !display_config_identity->source_gdi_device_name->empty()) {
          gdi_names.push_back(
            normalize_display_name(
              platf::to_utf8(*display_config_identity->source_gdi_device_name)
            )
          );
        }
        if (added_gdi_name && !added_gdi_name->empty()) {
          const auto normalized = normalize_display_name(platf::to_utf8(*added_gdi_name));
          if (std::find(gdi_names.begin(), gdi_names.end(), normalized) == gdi_names.end()) {
            gdi_names.push_back(normalized);
          }
        }

        if (monitor_path || !gdi_names.empty()) {
          const auto devices =
            platf::display_helper::Coordinator::instance().enumerate_devices(
              display_device::DeviceEnumerationDetail::Minimal
            );
          if (devices) {
            const display_device::EnumeratedDevice *exact_match = nullptr;
            for (const auto &device : *devices) {
              if (!is_virtual_display_device(device) || device.m_device_id.empty()) {
                continue;
              }

              const bool monitor_path_matches =
                monitor_path &&
                !device.m_monitor_device_path.empty() &&
                equals_ci(device.m_monitor_device_path, *monitor_path);
              const auto normalized_display_name =
                device.m_display_name.empty() ?
                  std::string {} :
                  normalize_display_name(device.m_display_name);
              const bool gdi_name_matches =
                !monitor_path &&
                !normalized_display_name.empty() &&
                std::find(gdi_names.begin(), gdi_names.end(), normalized_display_name) != gdi_names.end();
              if (!monitor_path_matches && !gdi_name_matches) {
                continue;
              }

              if (exact_match && !equals_ci(exact_match->m_device_id, device.m_device_id)) {
                BOOST_LOG(error)
                  << "AddVirtualDisplay output-bound identity matched multiple SudoVDA devices "
                  << "(adapter_luid=" << output.AdapterLuid.HighPart << ':'
                  << output.AdapterLuid.LowPart << ", target_id=" << output.TargetId << ").";
                return std::nullopt;
              }
              exact_match = &device;
            }

            if (exact_match &&
                exact_match->m_info &&
                !exact_match->m_display_name.empty() &&
                exact_match->m_info->m_resolution.m_width == width &&
                exact_match->m_info->m_resolution.m_height == height) {
              return exact_virtual_display_identity_t {
                .device_id = exact_match->m_device_id,
                .display_name = platf::from_utf8(exact_match->m_display_name),
                .monitor_device_path = exact_match->m_monitor_device_path.empty() ?
                                         std::nullopt :
                                         std::make_optional(platf::from_utf8(exact_match->m_monitor_device_path)),
              };
            }
          }
        }

        if (wait_for_monitor_stop(stop_token, kPollInterval)) {
          return std::nullopt;
        }
      }

      BOOST_LOG(error)
        << "Timed out resolving an exact active SudoVDA device identity from AddVirtualDisplay output "
        << "(adapter_luid=" << output.AdapterLuid.HighPart << ':'
        << output.AdapterLuid.LowPart << ", target_id=" << output.TargetId << ").";
      return std::nullopt;
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

        if (device_id && !device_id->empty()) {
          if (!device.m_device_id.empty() && equals_ci(device.m_device_id, *device_id)) {
            return true;
          }
          continue;
        }

        if (normalized_name) {
          const auto device_name = normalize_display_name(device.m_display_name);
          const auto friendly_name = normalize_display_name(device.m_friendly_name);
          if ((!device_name.empty() && device_name == *normalized_name) ||
              (!friendly_name.empty() && friendly_name == *normalized_name)) {
            return true;
          }
        }

        if ((!device_id || device_id->empty()) && !normalized_name) {
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

      if (stop_token.stop_requested()) {
        return false;
      }
      if (!is_virtual_display_present(result.display_name, result.device_id)) {
        BOOST_LOG(warning) << "Virtual display '" << name_utf8 << "' device_id='" << device_utf8
                           << "' missing immediately after creation.";
        return false;
      }

      if (wait_for_monitor_stop(stop_token, VIRTUAL_DISPLAY_STABILITY_RECHECK_DELAY)) {
        return false;
      }

      if (stop_token.stop_requested()) {
        return false;
      }
      if (!is_virtual_display_present(result.display_name, result.device_id)) {
        BOOST_LOG(warning) << "Virtual display '" << name_utf8 << "' device_id='" << device_utf8
                           << "' disappeared within " << delay_ms << "ms of confirmation.";
        return false;
      }

      BOOST_LOG(debug) << "Virtual display '" << name_utf8 << "' device_id='" << device_utf8
                       << "' remained present after " << delay_ms << "ms stability recheck.";
      return true;
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
      bool replace_existing,
      std::stop_token stop_token
    ) {
      if (stop_token.stop_requested() || SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
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
                       << " guid=" << requested_uuid.string();

      if (!teardown_conflicting_virtual_displays(requested_uuid, stop_token)) {
        return std::nullopt;
      }
      BOOST_LOG(debug) << "teardown_conflicting_virtual_displays completed for guid=" << requested_uuid.string();
      if (!enforce_teardown_cooldown_if_needed(stop_token)) {
        return std::nullopt;
      }
      if (stop_token.stop_requested()) {
        return std::nullopt;
      }

      // Conflict removal may have reopened a stale SudoVDA handle. The render
      // adapter preference belongs to that handle, so apply it only after all
      // teardown/recovery work and immediately before the create request.
      if (!VDISPLAY::policy::adapter_preference_allows_creation(
            apply_configured_render_adapter_preference("virtual display creation")
          )) {
        return std::nullopt;
      }
      const auto creation_render_request = current_render_adapter_request();
      if (!creation_render_request) {
        BOOST_LOG(error) << "SudoVDA render-adapter request state is unavailable immediately before creation.";
        return std::nullopt;
      }

      const uint32_t requested_fps = apply_refresh_overrides(fps, base_fps_millihz, framegen_refresh_active ? framegen_refresh_multiplier : 1);
      VIRTUAL_DISPLAY_ADD_OUT output {};
      BOOST_LOG(debug) << "Calling AddVirtualDisplay (driver handle present).";
      if (!AddVirtualDisplay(SUDOVDA_DRIVER_HANDLE, width, height, requested_fps, guid, s_client_name, s_client_uid, output)) {
        const DWORD error_code = GetLastError();
        BOOST_LOG(warning) << "AddVirtualDisplay failed: error=" << error_code << " guid=" << requested_uuid.string();

        if (replace_existing && !stop_token.stop_requested()) {
          if (!is_virtual_display_guid_tracked(requested_uuid) &&
              !has_render_adapter_request_provenance(requested_uuid)) {
            BOOST_LOG(error) << "Refusing to remove a colliding SudoVDA virtual display whose in-process ownership cannot be proven.";
            return std::nullopt;
          }
          BOOST_LOG(info) << "Virtual display create collided with existing state for guid="
                          << requested_uuid.string() << "; removing it before retry instead of reusing it.";
          if (!remove_virtual_display_impl(guid, false, stop_token)) {
            BOOST_LOG(error) << "Failed to remove the in-process-owned colliding SudoVDA virtual display.";
          }
          return std::nullopt;
        }
        if (stop_token.stop_requested()) {
          return std::nullopt;
        }

        const bool same_guid_tracked = is_virtual_display_guid_tracked(requested_uuid);
        const bool same_guid_provenance = has_render_adapter_request_provenance(requested_uuid);
        if (!same_guid_provenance) {
          if (!same_guid_tracked) {
            BOOST_LOG(error) << "Refusing to inspect, reuse, or remove a colliding SudoVDA virtual display whose in-process ownership cannot be proven.";
            return std::nullopt;
          }
          BOOST_LOG(warning) << "Removing a tracked SudoVDA virtual display whose render-adapter request provenance is missing.";
          if (!remove_virtual_display_impl(guid, false, stop_token)) {
            BOOST_LOG(error) << "Failed to remove the tracked SudoVDA virtual display with missing provenance.";
          }
          return std::nullopt;
        }

        const bool provenance_is_current =
          render_adapter_request_provenance_matches(
            requested_uuid,
            "colliding SudoVDA virtual display"
          );
        if (!same_guid_tracked || !provenance_is_current) {
          BOOST_LOG(info)
            << "Removing a colliding in-process-owned SudoVDA virtual display before retry "
            << "(guid=" << requested_uuid.string()
            << ", fully_tracked=" << same_guid_tracked
            << ", current_request_provenance=" << provenance_is_current << ").";
          if (!remove_virtual_display_impl(guid, false, stop_token)) {
            BOOST_LOG(error) << "Failed to remove the incomplete or stale-provenance SudoVDA virtual display.";
          }
          return std::nullopt;
        }

        std::optional<owned_display_identity_t> owned_identity;
        {
          std::lock_guard<std::recursive_mutex> operation_lock(g_virtual_display_operation_mutex);
          if (const auto identity = g_owned_display_identities.find(requested_uuid.string());
              identity != g_owned_display_identities.end()) {
            owned_identity = identity->second;
          }
        }
        if (!owned_identity || owned_identity->device_id.empty()) {
          BOOST_LOG(error) << "Tracked SudoVDA virtual display is missing its exact GUID-bound device identity; removing it before retry.";
          if (!remove_virtual_display_impl(guid, false, stop_token)) {
            BOOST_LOG(error) << "Failed to remove the SudoVDA virtual display with missing GUID-bound identity.";
          }
          return std::nullopt;
        }

        BOOST_LOG(debug) << "Waiting for exact owned SudoVDA device identity during reuse: device_id='"
                         << owned_identity->device_id << "'.";
        const auto exact_identity =
          wait_for_exact_active_device_id(
            owned_identity->device_id,
            width,
            height,
            stop_token
          );
        if (!exact_identity) {
          BOOST_LOG(warning) << "Exact GUID-bound SudoVDA device identity was not active during reuse; removing it before retry.";
          if (!remove_virtual_display_impl(guid, false, stop_token)) {
            BOOST_LOG(error) << "Failed to remove the inactive exact-identity SudoVDA virtual display.";
          }
          return std::nullopt;
        }
        if (!render_adapter_request_provenance_matches(
              requested_uuid,
              "exact-identity SudoVDA virtual display reuse"
            )) {
          BOOST_LOG(warning) << "SudoVDA render-adapter request provenance became stale during exact-identity reuse.";
          if (!remove_virtual_display_impl(guid, false, stop_token)) {
            BOOST_LOG(error) << "Failed to remove the stale-provenance SudoVDA virtual display.";
          }
          return std::nullopt;
        }

        if (auto dpi = read_virtual_display_dpi_value()) {
          (void) apply_virtual_display_dpi_value(*dpi);
        }

        if (exact_identity->display_name) {
          wprintf(
            L"[SUDOVDA] Reusing existing virtual display (error=%lu): %ls\n",
            static_cast<unsigned long>(error_code),
            exact_identity->display_name->c_str()
          );
        } else {
          printf("[SUDOVDA] Reusing existing virtual display (error=%lu).\n", static_cast<unsigned long>(error_code));
        }

        BOOST_LOG(info) << "Reused exact GUID-bound SudoVDA virtual display for guid="
                        << requested_uuid.string() << " device_id='"
                        << exact_identity->device_id << "'.";

        VirtualDisplayCreationResult result;
        result.display_name = exact_identity->display_name;
        result.device_id = exact_identity->device_id;
        result.monitor_device_path = exact_identity->monitor_device_path;
        if (s_client_name && std::strlen(s_client_name) > 0) {
          result.client_name = std::string(s_client_name);
        }
        result.reused_existing = true;
        result.confirmed_active = true;
        result.ready_since = std::chrono::steady_clock::now();

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

      const auto rollback_created_display = [&]() {
        if (!remove_virtual_display_impl(guid, false, stop_token)) {
          BOOST_LOG(warning) << "Virtual display creation cleanup failed for guid=" << requested_uuid.string() << '.';
        }
      };
      if (stop_token.stop_requested()) {
        rollback_created_display();
        return std::nullopt;
      }
      if (!render_adapter_request_matches(*creation_render_request) ||
          !record_render_adapter_request_provenance(requested_uuid, *creation_render_request)) {
        BOOST_LOG(error) << "SudoVDA handle generation or render-adapter request changed during creation.";
        rollback_created_display();
        return std::nullopt;
      }

      const auto exact_identity =
        wait_for_added_display_identity(
          output,
          width,
          height,
          stop_token
        );
      if (!exact_identity) {
        if (!stop_token.stop_requested()) {
          printf("[SUDOVDA] Unable to prove the exact active identity of the new virtual display; reverting creation.\n");
        }
        rollback_created_display();
        return std::nullopt;
      }

      if (stop_token.stop_requested()) {
        rollback_created_display();
        return std::nullopt;
      }

      if (exact_identity->display_name) {
        wprintf(L"[SUDOVDA] Virtual display added successfully: %ls\n", exact_identity->display_name->c_str());
      } else {
        wprintf(L"[SUDOVDA] Virtual display added with exact device identity (target=%u).\n", output.TargetId);
      }
      printf("[SUDOVDA] Configuration: W: %d, H: %d, FPS: %d\n", width, height, requested_fps);

      const auto ready_since = std::chrono::steady_clock::now();
      VirtualDisplayCreationResult result;
      result.display_name = exact_identity->display_name;
      result.device_id = exact_identity->device_id;
      if (s_client_name && std::strlen(s_client_name) > 0) {
        result.client_name = std::string(s_client_name);
      }
      result.monitor_device_path = exact_identity->monitor_device_path;
      result.reused_existing = false;
      result.confirmed_active = true;
      result.ready_since = ready_since;
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
    bool replace_existing,
    bool allow_reinstall,
    std::stop_token stop_token
  ) {
    std::lock_guard<std::recursive_mutex> operation_lock(g_virtual_display_operation_mutex);
    if (stop_token.stop_requested()) {
      return std::nullopt;
    }
    (void) hdr_requested;
    constexpr int kMaxInitializationAttempts = 3;
    const auto requested_uuid = guid_to_uuid(guid);

    for (int attempt = 1; attempt <= kMaxInitializationAttempts; ++attempt) {
      if (stop_token.stop_requested()) {
        return std::nullopt;
      }
      if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
        if (open_vdisplay_device_impl(stop_token, allow_reinstall) != DRIVER_STATUS::OK) {
          BOOST_LOG(warning) << "Unable to open SudoVDA driver handle for virtual display creation.";
          return std::nullopt;
        }
      }

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
        replace_existing,
        stop_token
      );
      if (!result) {
        if (stop_token.stop_requested()) {
          return std::nullopt;
        }
        BOOST_LOG(warning) << "Virtual display creation attempt " << attempt << '/' << kMaxInitializationAttempts
                           << " failed.";

        if (attempt == kMaxInitializationAttempts) {
          BOOST_LOG(error) << "Virtual display could not be created after " << kMaxInitializationAttempts << " attempts.";
          return std::nullopt;
        }

        closeVDisplayDevice();

        if (!ensure_driver_is_ready_impl(RestartCooldownBehavior::wait, stop_token, allow_reinstall)) {
          BOOST_LOG(warning) << "Driver recovery failed after virtual display creation failure.";
          return std::nullopt;
        }

        if (open_vdisplay_device_impl(stop_token, allow_reinstall) != DRIVER_STATUS::OK) {
          BOOST_LOG(warning) << "Failed to re-open SudoVDA driver after recovery.";
          return std::nullopt;
        }

        BOOST_LOG(info) << "Retrying SudoVDA virtual display initialization (attempt "
                        << (attempt + 1) << '/' << kMaxInitializationAttempts << ").";
        continue;
      }

      if (confirm_virtual_display_persistence(*result, width, height, stop_token)) {
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
          } else if (result->monitor_device_path) {
            const auto scale_result = VDISPLAY::set_display_scale_percent(
              *result->monitor_device_path,
              scale_percent
            );
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
          } else {
            BOOST_LOG(warning) << "Virtual display scale: monitor device path was unavailable; Windows scale was not applied.";
          }
        }
        if (stop_token.stop_requested()) {
          if (!result->reused_existing) {
            (void) remove_virtual_display_impl(guid, false, stop_token);
          }
          return std::nullopt;
        }
        write_guid_to_state_locked(requested_uuid);
        if (!track_virtual_display_created(requested_uuid, *result)) {
          BOOST_LOG(error) << "SudoVDA virtual display survived creation but its render-adapter request provenance could not be validated.";
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

      if (!ensure_driver_is_ready_impl(RestartCooldownBehavior::wait, stop_token, allow_reinstall)) {
        BOOST_LOG(warning) << "Driver recovery failed after virtual display vanished.";
        return std::nullopt;
      }

      if (open_vdisplay_device_impl(stop_token, allow_reinstall) != DRIVER_STATUS::OK) {
        BOOST_LOG(warning) << "Failed to re-open SudoVDA driver after recovery.";
        return std::nullopt;
      }

      BOOST_LOG(info) << "Retrying SudoVDA virtual display initialization (attempt "
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
    bool replace_existing
  ) {
    if (!release_retained_ensure_display_for_stream(s_client_uid)) {
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
      replace_existing,
      true,
      {}
    );
  }

  bool removeAllVirtualDisplays() {
    abort_all_recovery_monitors();
    std::lock_guard<std::recursive_mutex> operation_lock(g_virtual_display_operation_mutex);
    auto all_guids = active_virtual_display_tracker().all();
    for (const auto &owned : owned_render_adapter_request_provenance_guids()) {
      if (std::find(all_guids.begin(), all_guids.end(), owned) == all_guids.end()) {
        all_guids.push_back(owned);
      }
    }
    if (all_guids.empty()) {
      BOOST_LOG(debug) << "No active or transactionally owned virtual displays to remove.";
      return true;
    }

    bool all_removed = true;
    for (const auto &guid : all_guids) {
      GUID native_guid = uuid_to_guid(guid);
      BOOST_LOG(debug) << "Removing virtual display with GUID " << guid.string();
      if (!VDISPLAY_SUDOVDA::removeVirtualDisplay(native_guid)) {
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

  static bool remove_virtual_display_impl(
    const GUID &guid,
    bool cancel_recovery_monitor,
    std::stop_token stop_token
  ) {
    const auto guid_uuid = guid_to_uuid(guid);
    // A recovery-owned rollback must complete removal even after its monitor
    // has been stopped. If the device handle went stale at that instant,
    // reopening solely to remove the just-created display prevents an orphan.
    // Keep the original no-reinstall policy for cancellation-aware recovery.
    const std::stop_token reopen_stop_token = cancel_recovery_monitor ? stop_token : std::stop_token {};
    // Always cancel profile work for a display being removed. The recovery
    // monitor may already have completed, so its stop token alone is not a
    // sufficient lifetime boundary for this deferred work.
    deferred_hdr_profile_workers().request_stop(guid_uuid.string());
    if (cancel_recovery_monitor && stop_token.stop_requested()) {
      return false;
    }
    if (cancel_recovery_monitor) {
      abort_recovery_monitor(guid_uuid);
    }
    std::lock_guard<std::recursive_mutex> operation_lock(g_virtual_display_operation_mutex);
    auto cached_display_name = resolve_virtual_display_name_from_devices();

    const bool initial_handle_invalid = (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE);
    const bool allow_reinstall = !stop_token.stop_possible();
    bool opened_handle = false;

    auto ensure_handle = [&]() -> bool {
      if (SUDOVDA_DRIVER_HANDLE != INVALID_HANDLE_VALUE) {
        return true;
      }
      if ((cancel_recovery_monitor && stop_token.stop_requested()) ||
          open_vdisplay_device_impl(reopen_stop_token, allow_reinstall) != DRIVER_STATUS::OK) {
        printf("[SUDOVDA] Failed to open driver while removing virtual display.\n");
        return false;
      }
      opened_handle = true;
      return true;
    };

    auto perform_remove = [&]() -> std::pair<bool, DWORD> {
      const bool removed = RemoveVirtualDisplay(SUDOVDA_DRIVER_HANDLE, guid);
      DWORD error_code = removed ? ERROR_SUCCESS : GetLastError();
      if (removed) {
        track_virtual_display_removed(guid_to_uuid(guid));
        note_virtual_display_teardown();
      } else if (error_code == ERROR_FILE_NOT_FOUND || error_code == ERROR_INVALID_PARAMETER) {
        track_virtual_display_removed(guid_to_uuid(guid));
        note_virtual_display_teardown();
      }
      return {removed, error_code};
    };

    if (!ensure_handle()) {
      return false;
    }

    auto [removed, error_code] = perform_remove();
    if (!removed && !initial_handle_invalid && error_code == ERROR_INVALID_HANDLE) {
      printf("[SUDOVDA] Driver handle became invalid while removing virtual display; retrying.\n");
      closeVDisplayDevice();
      if ((!cancel_recovery_monitor || !stop_token.stop_requested()) &&
          open_vdisplay_device_impl(reopen_stop_token, allow_reinstall) == DRIVER_STATUS::OK) {
        opened_handle = true;
        auto retry_result = perform_remove();
        removed = retry_result.first;
        error_code = retry_result.second;
      } else {
        error_code = ERROR_INVALID_HANDLE;
      }
    }

    if (opened_handle && initial_handle_invalid) {
      closeVDisplayDevice();
    }

    if (removed) {
      printf("[SUDOVDA] Virtual display removed successfully.\n");
      if (cached_display_name) {
        constexpr auto teardown_timeout = std::chrono::seconds(2);
        if (!wait_for_virtual_display_teardown(*cached_display_name, teardown_timeout, stop_token)) {
          if (stop_token.stop_requested()) {
            BOOST_LOG(debug) << "Virtual display teardown wait cancelled for '" << platf::to_utf8(*cached_display_name) << "'.";
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

    printf("[SUDOVDA] Failed to remove virtual display (error=%lu).\n", static_cast<unsigned long>(error_code));
    return false;
  }

  bool removeVirtualDisplay(const GUID &guid) {
    return remove_virtual_display_impl(guid, true, {});
  }

  static bool is_sudovda_driver_installed_passive() {
    // Status/enumeration paths must not repair a missing device node. Users can
    // remove SudoVDA when they do not use virtual displays; reinstall only from
    // explicit driver initialization/recovery.
    if (driver_handle_responsive(SUDOVDA_DRIVER_HANDLE)) {
      return true;
    }

    if (probe_driver_responsive_once()) {
      return true;
    }

    return find_sudovda_device_instance_id().has_value();
  }

  bool isSudaVDADriverInstalled() {
    std::lock_guard<std::recursive_mutex> operation_lock(g_virtual_display_operation_mutex);
    return VDISPLAY::policy::passive_install_status(is_sudovda_driver_installed_passive());
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
    const bool allow_any_fallback
  ) {
    if (stable_id.empty()) {
      return resolveActiveVirtualDisplayDeviceId(
        preferred_output_identifier,
        client_name,
        allow_any_fallback
      );
    }

    const auto stable_uuid = VDISPLAY::virtualDisplayUuidFromStableId(stable_id);
    std::optional<owned_display_identity_t> owned_identity;
    {
      std::lock_guard<std::recursive_mutex> operation_lock(g_virtual_display_operation_mutex);
      if (const auto identity = g_owned_display_identities.find(stable_uuid.string());
          identity != g_owned_display_identities.end()) {
        owned_identity = identity->second;
      }
    }
    if (!owned_identity) {
      BOOST_LOG(debug) << "No proven SudoVDA device identity is recorded for stable_id='"
                       << stable_id << "'.";
      return std::nullopt;
    }

    const auto devices =
      platf::display_helper::Coordinator::instance().enumerate_devices(
        display_device::DeviceEnumerationDetail::Minimal
      );
    if (!devices) {
      return std::nullopt;
    }

    for (const auto &device : *devices) {
      if (!is_virtual_display_device(device) ||
          device.m_device_id.empty() ||
          !device.m_info ||
          device.m_display_name.empty()) {
        continue;
      }

      if (equals_ci(device.m_device_id, owned_identity->device_id)) {
        BOOST_LOG(debug) << "Resolved active SudoVDA virtual display by proven stable GUID identity: device_id='"
                         << device.m_device_id << "'.";
        return device.m_device_id;
      }
    }

    BOOST_LOG(debug) << "Proven SudoVDA identity for stable_id='" << stable_id
                     << "' is not currently active.";
    return std::nullopt;
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
    return VDISPLAY::policy::is_virtual_display_selection(output_identifier, true);
  }

  uint64_t client_uuid_to_vdd_display_id(const GUID &client_guid) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(&client_guid);
    std::uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < sizeof(GUID); ++i) {
      hash ^= static_cast<std::uint64_t>(bytes[i]);
      hash *= 1099511628211ull;
    }
    return hash == 0 ? 1 : hash;
  }

  GUID sharedVirtualDisplayGuid() {
    return VDISPLAY::sharedVirtualDisplayGuid();
  }

  bool is_vdd_virtual_display_identity(
    const std::string &device_path,
    const std::string &friendly_name,
    const std::string &edid_manufacturer_id,
    const std::string &edid_product_code
  ) {
    auto contains_ci_local = [](const std::string &haystack, const std::string &needle) {
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
    };

    if (contains_ci_local(device_path, "MttVDD")) {
      return true;
    }
    if (equals_ci(friendly_name, "Virtual Display Driver")) {
      return true;
    }
    return equals_ci(edid_manufacturer_id, "MTT") &&
           (equals_ci(edid_product_code, "1337") || equals_ci(edid_product_code, "0x1337") ||
            equals_ci(edid_product_code, "1338") || equals_ci(edid_product_code, "0x1338"));
  }

  std::vector<SudaVDADisplayInfo> enumerateSudaVDADisplays() {
    std::vector<SudaVDADisplayInfo> result;

    if (!isSudaVDADriverInstalled()) {
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

      SudaVDADisplayInfo info;
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
}  // namespace VDISPLAY_SUDOVDA

bool VDISPLAY_SUDOVDA::has_active_physical_display() {
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

bool VDISPLAY_SUDOVDA::should_auto_enable_virtual_display() {
  if (!isSudaVDADriverInstalled()) {
    BOOST_LOG(warning) << "Suda VDA driver not installed, not enabling virtual display.";
    return false;
  }

  if (has_active_physical_display()) {
    BOOST_LOG(debug) << "Active physical display detected, not enabling virtual display.";
    return false;
  }

  return true;
}

uuid_util::uuid_t VDISPLAY_SUDOVDA::persistentVirtualDisplayUuid() {
  return VDISPLAY::persistentVirtualDisplayUuid();
}

namespace {
  void wait_for_sudovda_ensure_target(
    VDISPLAY::ensure_display_result &result,
    std::chrono::steady_clock::duration timeout
  ) {
    result.readiness = VDISPLAY::ensure_display_readiness_e::request_retained;
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (true) {
      const auto device_id =
        VDISPLAY_SUDOVDA::resolveActiveVirtualDisplayDeviceIdForStableId(
          "sunshine-ensure",
          {},
          "Sunshine Temporary",
          false
        );
      result.device_id = device_id.value_or(std::string {});
      result.display_name.clear();

      if (device_id) {
        const auto display_name = VDISPLAY::resolveUsableDisplayName(*device_id);
        if (display_name && !display_name->empty()) {
          result.display_name = *display_name;
          result.readiness = VDISPLAY::ensure_display_readiness_e::target_enumerated;
          const auto dxgi_names = platf::display_names(platf::mem_type_e::dxgi);
          if (std::any_of(dxgi_names.begin(), dxgi_names.end(), [&](const std::string &name) {
                return !name.empty() && boost::iequals(name, *display_name);
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

VDISPLAY_SUDOVDA::ensure_display_result VDISPLAY_SUDOVDA::ensure_display(
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
        << "Owned SudoVDA encoder-probe display request no longer matches the preferred render adapter.";
      return result;
    }
    if (!isSudaVDADriverInstalled()) {
      BOOST_LOG(warning) << "SudoVDA driver not available for owned encoder probing.";
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
        "retained SudoVDA encoder-probe display"
      )) {
    wait_for_sudovda_ensure_target(result, std::chrono::seconds(3));
    BOOST_LOG(info) << "Reusing temporary virtual display for the active encoder probe (readiness="
                    << static_cast<int>(result.readiness)
                    << ", display_name='"
                    << (result.display_name.empty() ? std::string("<pending>") : result.display_name)
                    << "').";
    return result;
  }

  if (retained_ensure_display) {
    VDISPLAY_SUDOVDA::cleanup_ensure_display(result);
    result.tracks_temporary_for_probe = false;
    result.temporary_generation = 0;
    std::lock_guard<std::mutex> lock(g_ensure_display_state_mutex);
    if (g_ensure_display_lifetime.retained()) {
      BOOST_LOG(error) << "Failed to remove stale owned SudoVDA encoder-probe display.";
      return result;
    }
    BOOST_LOG(debug) << "Ensure display retention state was stale; creating a fresh temporary display.";
  }

  auto virtual_displays = enumerateSudaVDADisplays();
  bool has_active_virtual = std::any_of(
    virtual_displays.begin(),
    virtual_displays.end(),
    [](const SudaVDADisplayInfo &info) {
      return info.is_active;
    }
  );

  if (has_active_virtual) {
    BOOST_LOG(warning) << "Active SudoVDA virtual display has no exact owned render-adapter request provenance for encoder probing; attempting to create an owned temporary display.";
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
    BOOST_LOG(error) << "Could not publish SudoVDA encoder-probe display ownership.";
    (void) removeVirtualDisplay(result.temporary_guid);
    return result;
  }

  // Require the exact retained target to have a non-empty GDI name and appear
  // in DXGI. An unrelated output must not satisfy readiness on its behalf.
  wait_for_sudovda_ensure_target(result, std::chrono::seconds(3));
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

void VDISPLAY_SUDOVDA::cleanup_ensure_display(const ensure_display_result &result) {
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

bool VDISPLAY_SUDOVDA::has_retained_ensure_display() {
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
    has_render_adapter_request_provenance(guid_to_uuid(retained_guid))
  );
}

void VDISPLAY_SUDOVDA::cleanup_retained_ensure_display() {
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
    BOOST_LOG(warning) << "Failed to remove retained SudoVDA encoder-probe virtual display.";
    return;
  }
}
