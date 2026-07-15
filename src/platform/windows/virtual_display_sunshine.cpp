#include "virtual_display.h"

#include <virtual_display/driver/control_client.h>
#include <virtual_display/driver/windows_control_client.h>

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
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <cctype>
#include <cfgmgr32.h>
#include <chrono>
#include <cmath>
#include <combaseapi.h>
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
#include <random>
#include <nlohmann/json.hpp>
#include <optional>
#include <physicalmonitorenumerationapi.h>
#include <setupapi.h>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <unordered_map>
#include <vector>
#include <winsock2.h>
#include <windows.h>
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
  using VDISPLAY::ensure_display_result;
  inline constexpr const char *VIRTUAL_DISPLAY_SELECTION = VDISPLAY::VIRTUAL_DISPLAY_SELECTION;

  void closeVDisplayDevice();
  DRIVER_STATUS openVDisplayDevice();
  bool ensure_driver_is_ready();
  bool startPingThread(std::function<void()> failCb);
  void setWatchdogFeedingEnabled(bool enable);
  bool setRenderAdapterByName(const std::wstring &adapterName);
  bool setRenderAdapterWithMostDedicatedMemory();
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
  ensure_display_result ensure_display();
  void cleanup_ensure_display(const ensure_display_result &result, bool probe_succeeded, bool allow_temporary_teardown = true);

  enum class RestartCooldownBehavior {
    skip,
    wait,
  };

  static bool ensure_driver_is_ready_impl(RestartCooldownBehavior cooldown_behavior);

  namespace {
    constexpr auto WATCHDOG_INIT_GRACE = std::chrono::seconds(30);
    constexpr auto DRIVER_RESTART_TIMEOUT = std::chrono::seconds(5);
    constexpr auto DRIVER_RESTART_POLL_INTERVAL = std::chrono::milliseconds(500);
    constexpr auto DRIVER_RESTART_FAILURE_COOLDOWN = std::chrono::seconds(3);
    constexpr int DRIVER_RESTART_MAX_ATTEMPTS = 3;
    constexpr auto DEVICE_RESTART_SETTLE_DELAY = std::chrono::milliseconds(200);
    constexpr auto VIRTUAL_DISPLAY_TEARDOWN_COOLDOWN = std::chrono::milliseconds(250);
    constexpr int ENSURE_DISPLAY_MAX_RETRY_FAILURES = 8;
    constexpr std::wstring_view SUNSHINE_DRIVER_HARDWARE_ID = L"root\\sunshinevirtualdisplay";
    constexpr std::wstring_view SUNSHINE_DRIVER_FRIENDLY_NAME_W = L"Sunshine Virtual Display Driver";
    constexpr std::uint32_t DRIVER_LEASE_TIMEOUT_MS = 30000;
    constexpr std::uint16_t REQUIRED_DRIVER_PROTOCOL_MAJOR = sunshine_driver::kProtocolVersionMajor;
    constexpr std::uint16_t REQUIRED_DRIVER_PROTOCOL_MINOR = sunshine_driver::kProtocolVersionMinor;

    std::atomic<bool> g_watchdog_feed_requested {false};
    std::atomic<bool> g_watchdog_stop_requested {false};
    std::atomic<std::int64_t> g_watchdog_grace_deadline_ns {0};
    std::mutex g_watchdog_thread_mutex;
    std::thread g_watchdog_thread;
    std::function<void()> g_watchdog_fail_cb;
    std::atomic<std::int64_t> g_last_teardown_ns {0};
    std::atomic<std::int64_t> g_last_restart_failure_ns {0};
    std::mutex g_ensure_display_state_mutex;
    bool g_ensure_display_retained = false;
    GUID g_ensure_display_guid {};
    int g_ensure_display_failure_count = 0;

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

    void enforce_teardown_cooldown_if_needed() {
      const auto last_teardown = g_last_teardown_ns.load(std::memory_order_acquire);
      if (last_teardown <= 0) {
        return;
      }

      const auto last_time = time_from_steady_ticks(last_teardown);
      const auto deadline = last_time + VIRTUAL_DISPLAY_TEARDOWN_COOLDOWN;
      const auto now = std::chrono::steady_clock::now();
      if (deadline > now) {
        const auto sleep_for = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        BOOST_LOG(debug) << "Delaying virtual display creation for " << sleep_for.count()
                         << " ms to let teardown settle.";
        std::this_thread::sleep_for(sleep_for);
      }
    }

    bool within_grace_period(std::chrono::steady_clock::time_point now) {
      auto deadline_ticks = g_watchdog_grace_deadline_ns.load(std::memory_order_acquire);
      if (deadline_ticks <= 0) {
        return false;
      }
      return now < time_from_steady_ticks(deadline_ticks);
    }

    void stop_watchdog_thread(bool wait_for_exit) {
      g_watchdog_stop_requested.store(true, std::memory_order_release);

      std::thread watchdog_thread;
      {
        std::lock_guard<std::mutex> lock(g_watchdog_thread_mutex);
        if (!g_watchdog_thread.joinable()) {
          return;
        }
        watchdog_thread = std::move(g_watchdog_thread);
      }

      if (!watchdog_thread.joinable()) {
        return;
      }

      if (watchdog_thread.get_id() == std::this_thread::get_id()) {
        // Failure callbacks can tear down the driver from within the watchdog itself.
        watchdog_thread.detach();
        return;
      }

      if (wait_for_exit) {
        watchdog_thread.join();
      } else {
        watchdog_thread.detach();
      }
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

    bool ensure_watchdog_thread_active_for_lease() {
      if (watchdog_thread_running()) {
        return true;
      }

      auto fail_cb = copy_watchdog_fail_cb();
      if (!fail_cb) {
        BOOST_LOG(warning) << "Sunshine virtual display lease-feed thread is not running and no failure callback is registered; using driver transport reset fallback.";
        fail_cb = default_watchdog_fail_cb();
      }

      if (!startPingThread(std::move(fail_cb))) {
        BOOST_LOG(warning) << "Sunshine virtual display lease-feed thread could not be started for an active temporary display.";
        return false;
      }

      return true;
    }

    void dispatch_watchdog_fail_cb(std::shared_ptr<const std::function<void()>> fail_cb) {
      if (!fail_cb || !*fail_cb) {
        return;
      }

      try {
        std::thread([fail_cb = std::move(fail_cb)]() {
          try {
            if (*fail_cb) {
              (*fail_cb)();
            }
          } catch (const std::exception &err) {
            BOOST_LOG(error) << "Sunshine virtual display lease-feed failure callback threw: " << err.what();
          } catch (...) {
            BOOST_LOG(error) << "Sunshine virtual display lease-feed failure callback threw an unknown exception.";
          }
        }).detach();
      } catch (const std::system_error &err) {
        BOOST_LOG(error) << "Sunshine virtual display lease-feed: failed to dispatch failure callback thread: " << err.what();
        try {
          (*fail_cb)();
        } catch (const std::exception &cb_err) {
          BOOST_LOG(error) << "Sunshine virtual display lease-feed failure callback threw: " << cb_err.what();
        } catch (...) {
          BOOST_LOG(error) << "Sunshine virtual display lease-feed failure callback threw an unknown exception.";
        }
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

    bool set_permanent_display_count(sunshine_driver::ControlClient &client, std::uint32_t display_count) {
      sunshine_driver::PermanentDisplayCountRequest request {};
      request.display_count = display_count;

      const auto result = client.set_permanent_display_count(request);
      if (!result.ok()) {
        const auto after_failure = client.query_permanent_display_count();
        if (after_failure.ok() && after_failure.value.current_display_count == display_count) {
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
    bool try_reenable_disabled_device(const std::wstring &instance_id) {
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
      std::this_thread::sleep_for(DEVICE_RESTART_SETTLE_DELAY * 2);

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

    std::uint64_t generate_driver_lease_id() {
      static std::mutex mutex;
      static std::mt19937_64 rng([] {
        std::random_device rd;
        const auto now = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        std::seed_seq seed {
          rd(),
          rd(),
          static_cast<unsigned int>(now),
          static_cast<unsigned int>(now >> 32),
          static_cast<unsigned int>(GetCurrentProcessId())
        };
        return std::mt19937_64(seed);
      }());

      std::lock_guard<std::mutex> lock(mutex);
      std::uint64_t lease_id = 0;
      while (lease_id < sunshine_driver::kMinOpaqueLeaseId) {
        lease_id = rng() | sunshine_driver::kMinOpaqueLeaseId;
      }
      return lease_id;
    }

    bool is_missing_lease_error(DWORD error_code) {
      return error_code == ERROR_SUCCESS ||
             error_code == ERROR_FILE_NOT_FOUND ||
             error_code == ERROR_NOT_FOUND ||
             error_code == ERROR_INVALID_PARAMETER;
    }

    void track_virtual_display_created(const uuid_util::uuid_t &guid) {
      active_virtual_display_tracker().add(guid);
    }

    void track_virtual_display_removed(const uuid_util::uuid_t &guid) {
      driver_lease_tracker().remove(guid);
      active_virtual_display_tracker().remove(guid);
    }

    bool is_virtual_display_guid_tracked(const uuid_util::uuid_t &guid) {
      return active_virtual_display_tracker().contains(guid);
    }

    std::vector<uuid_util::uuid_t> collect_conflicting_virtual_displays(const uuid_util::uuid_t &guid) {
      return active_virtual_display_tracker().other_than(guid);
    }

    void teardown_conflicting_virtual_displays(const uuid_util::uuid_t &guid) {
      auto conflicts = collect_conflicting_virtual_displays(guid);
      for (const auto &entry : conflicts) {
        GUID native_guid = uuid_to_guid(entry);
        (void) removeVirtualDisplay(native_guid);
      }
    }

    std::optional<std::wstring> resolve_virtual_display_name_from_devices_for_client(const char *client_name);

    void release_retained_ensure_display_for_stream(const GUID &guid, const char *client_uid) {
      if (is_ensure_display_client(client_uid)) {
        return;
      }

      GUID guid_to_remove = uuid_to_guid(persistentVirtualDisplayUuid());
      bool should_remove = false;
      {
        std::lock_guard<std::mutex> lock(g_ensure_display_state_mutex);
        if (g_ensure_display_retained) {
          guid_to_remove = g_ensure_display_guid;
          g_ensure_display_retained = false;
          g_ensure_display_failure_count = 0;
          std::memset(&g_ensure_display_guid, 0, sizeof(g_ensure_display_guid));
          should_remove = true;
        }
      }

      if (!should_remove && resolve_virtual_display_name_from_devices_for_client("Sunshine Temporary")) {
        should_remove = true;
      }

      if (!should_remove) {
        return;
      }

      BOOST_LOG(info) << "Removing encoder-probe virtual display before creating stream display guid="
                      << guid_to_uuid(guid).string() << '.';
      if (!removeVirtualDisplay(guid_to_remove)) {
        BOOST_LOG(warning) << "Failed to remove retained encoder-probe virtual display before stream creation.";
      }
    }

    bool adopt_existing_driver_lease(
      sunshine_driver::ControlClient &client,
      const uuid_util::uuid_t &guid,
      std::uint64_t display_id,
      const std::optional<std::wstring> &display_name,
      const std::optional<std::string> &device_id,
      const std::optional<std::wstring> &monitor_device_path
    ) {
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
                             << " but the driver did not report a lease id.";
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
                             << " with missing lease_id=" << entry.lease_id << '.';
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
                        << " display_id=" << entry.display_id << " lease_id=" << entry.lease_id << '.';
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
      const std::optional<std::string> &client_name = std::nullopt
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
      bool wait_for_completion = false
    ) {
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

      auto apply_profile_work = [profile_path,
                                 client_name,
                                 monitor_path = monitor_device_path,
                                 display_name,
                                 device_id,
                                 should_clear_mismatched]() {
        std::optional<std::wstring> device_name_w = monitor_path;
        if (!device_name_w || device_name_w->empty()) {
          // Resolve monitor path - allow up to 5 seconds for display to be enumerable
          if (should_clear_mismatched) {
            // Virtual displays: avoid relying on the client name (it may be stale/incorrect) and instead target the
            // active Sunshine virtual display when present. Prefer the explicit display identifiers first.
            device_name_w = resolve_monitor_device_path(display_name, device_id, 50, std::chrono::milliseconds(100), std::nullopt);

            if (!device_name_w || device_name_w->empty()) {
              const auto active_vd_name = resolve_virtual_display_name_from_devices();
              const auto active_vd_device_id = resolveAnyVirtualDisplayDeviceId();
              if (active_vd_name || active_vd_device_id) {
                BOOST_LOG(debug) << "HDR profile: virtual display monitor path unresolved; falling back to active virtual display."
                                 << " active_name='" << (active_vd_name ? platf::to_utf8(*active_vd_name) : std::string("(none)"))
                                 << "' active_device_id='" << (active_vd_device_id ? *active_vd_device_id : std::string("(none)")) << "'.";
                device_name_w = resolve_monitor_device_path(active_vd_name, active_vd_device_id, 50, std::chrono::milliseconds(100), std::nullopt);
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
            device_name_w = resolve_monitor_device_path(physical_display_name, physical_device_id, 50, std::chrono::milliseconds(100), std::nullopt);
          }
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
          const auto [local_success, local_access_denied] = apply_profile_for_scope(color_profile_scope_e::current_user);
          success = local_success;

          if (!success && should_clear_mismatched && running_as_system && local_access_denied) {
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

      if (wait_for_completion) {
        apply_profile_work();
      } else {
        std::thread(std::move(apply_profile_work)).detach();
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
        false,
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

    bool request_hdr10_advanced_color(const DisplayConfigTarget &output) {
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

      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      do {
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
          const bool ten_bit_or_better = info->bits_per_color_channel >= 10;
          if (info->supported && info->hdr_supported && info->hdr_enabled && !info->limited_by_policy && ten_bit_or_better) {
            return true;
          }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      } while (std::chrono::steady_clock::now() < deadline);

      BOOST_LOG(warning) << "Sunshine virtual display HDR: Windows did not report HDR support/enabled at 10-bit for target "
                         << output.TargetId << " after activation request.";
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
      std::chrono::steady_clock::duration timeout = std::chrono::milliseconds(250)
    ) {
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      do {
        if (auto identity = query_display_config_identity(output)) {
          if (display_config_identity_has_display_name(*identity)) {
            return identity;
          }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      } while (std::chrono::steady_clock::now() < deadline);

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
      const std::optional<std::string> &client_name
    ) {
      // Try without impersonation first (faster if already in user context)
      for (int i = 0; i < attempts; ++i) {
        if (auto path = resolve_monitor_device_path_once(display_name, device_id, client_name)) {
          return path;
        }
        if (i + 1 < attempts) {
          std::this_thread::sleep_for(delay);
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
          if (auto path = resolve_monitor_device_path_once(display_name, device_id, client_name)) {
            result = path;
            return;
          }
          if (i + 1 < attempts) {
            std::this_thread::sleep_for(delay);
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

    uuid_util::uuid_t ensure_persistent_guid() {
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
    constexpr auto RECOVERY_INITIAL_SETTLE_GRACE = std::chrono::seconds(6);
    constexpr auto RECOVERY_POST_SUCCESS_GRACE = std::chrono::seconds(3);
    constexpr auto RECOVERY_MAX_ATTEMPTS_BACKOFF = std::chrono::seconds(5);
    constexpr auto RECOVERY_MAX_BACKOFF = std::chrono::seconds(60);
    constexpr auto DRIVER_RECOVERY_WARMUP_DELAY = std::chrono::milliseconds(500);

    std::mutex g_virtual_display_recovery_abort_mutex;
    std::map<uuid_util::uuid_t, std::weak_ptr<std::atomic_bool>> g_virtual_display_recovery_abort;

    std::shared_ptr<std::atomic_bool> reset_recovery_monitor_abort_flag(const uuid_util::uuid_t &guid_uuid) {
      std::lock_guard<std::mutex> lock(g_virtual_display_recovery_abort_mutex);
      auto &entry = g_virtual_display_recovery_abort[guid_uuid];
      if (auto existing = entry.lock()) {
        existing->store(true, std::memory_order_release);
      }
      auto flag = std::make_shared<std::atomic_bool>(false);
      entry = flag;
      return flag;
    }

    void abort_recovery_monitor(const uuid_util::uuid_t &guid_uuid) {
      std::lock_guard<std::mutex> lock(g_virtual_display_recovery_abort_mutex);
      auto it = g_virtual_display_recovery_abort.find(guid_uuid);
      if (it == g_virtual_display_recovery_abort.end()) {
        return;
      }
      if (auto flag = it->second.lock()) {
        flag->store(true, std::memory_order_release);
      }
      g_virtual_display_recovery_abort.erase(it);
    }

    void abort_all_recovery_monitors() {
      std::lock_guard<std::mutex> lock(g_virtual_display_recovery_abort_mutex);
      for (auto &[_, weak_flag] : g_virtual_display_recovery_abort) {
        if (auto flag = weak_flag.lock()) {
          flag->store(true, std::memory_order_release);
        }
      }
      g_virtual_display_recovery_abort.clear();
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

    bool monitor_should_abort(const RecoveryMonitorState &state) {
      return state.params.should_abort && state.params.should_abort();
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
      if (!devices) {
        // Enumeration failed, so the target's presence cannot be determined yet.
        return MonitorTargetPresence::unknown;
      }
      if (devices->empty()) {
        // A successful empty enumeration is definitive on a headless system. Let the
        // normal missing-target grace period recreate the virtual display.
        return MonitorTargetPresence::missing;
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

          if (adopted_display_name != state.current_display_name
              || adopted_device_id != state.current_device_id
              || adopted_monitor_device_path != state.current_monitor_device_path) {
            const auto before = state.describe_target();
            state.update_identifiers(adopted_display_name, adopted_device_id, adopted_monitor_device_path);
            BOOST_LOG(debug) << "Virtual display recovery monitor adopted updated identifiers via client_name '"
                             << state.params.client_name << "': " << before << " -> " << state.describe_target();
          }
        }

        const bool is_active = device.m_info.has_value() || !device.m_display_name.empty();
        if (is_active) {
          return MonitorTargetPresence::present_active;
        }
        matched_inactive = true;
      }

      return matched_inactive ? MonitorTargetPresence::present_inactive : MonitorTargetPresence::missing;
    }

    bool attempt_virtual_display_recovery(RecoveryMonitorState &state) {
      if (monitor_should_abort(state)) {
        return false;
      }
      if (!ensure_driver_is_ready()) {
        BOOST_LOG(warning) << "Virtual display recovery: driver not ready for " << state.describe_target();
        return false;
      }

      proc::vDisplayDriverStatus = openVDisplayDevice();
      if (proc::vDisplayDriverStatus != DRIVER_STATUS::OK) {
        BOOST_LOG(warning) << "Virtual display recovery: failed to reopen driver (status="
                           << static_cast<int>(proc::vDisplayDriverStatus) << ") for "
                           << state.describe_target();
        return false;
      }

      // Restart the watchdog ping thread with the new driver handle.
      // The old ping thread is still feeding a stale duplicated handle;
      // startPingThread stops it and duplicates the freshly opened handle.
      if (auto watchdog_fail_cb = copy_watchdog_fail_cb(); watchdog_fail_cb) {
        if (!startPingThread(std::move(watchdog_fail_cb))) {
          BOOST_LOG(warning) << "Virtual display recovery: failed to restart watchdog ping thread for "
                             << state.describe_target();
        }
      }

      setWatchdogFeedingEnabled(true);
      auto recreation = createVirtualDisplay(
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
        state.params.hdr_requested
      );
      if (!recreation) {
        BOOST_LOG(warning) << "Virtual display recovery: createVirtualDisplay failed for " << state.describe_target();
        return false;
      }

      state.update_identifiers(recreation->display_name, recreation->device_id, recreation->monitor_device_path);
      if (monitor_should_abort(state)) {
        BOOST_LOG(debug) << "Virtual display recovery aborted after recreation for " << state.describe_target();
        return false;
      }
      if (state.params.on_recovery_success) {
        state.params.on_recovery_success(*recreation);
      }
      return true;
    }

    void run_virtual_display_recovery_monitor(RecoveryMonitorState state) {
      unsigned int attempts = 0;
      unsigned int backoff_cycles = 0;
      constexpr unsigned int MAX_RECOVERY_BACKOFF_CYCLES = 5;
      bool observed_active = state.confirmed_active_at_schedule;
      std::optional<std::chrono::steady_clock::time_point> active_since =
        state.confirmed_active_at_schedule ? std::make_optional(std::chrono::steady_clock::now()) : std::nullopt;
      std::optional<std::chrono::steady_clock::time_point> inactive_since;
      std::optional<std::chrono::steady_clock::time_point> missing_since;
      auto recovery_cooldown_until = std::chrono::steady_clock::now() + RECOVERY_INITIAL_SETTLE_GRACE;

      while (true) {
        if (monitor_should_abort(state)) {
          BOOST_LOG(debug) << "Virtual display recovery monitor aborted for " << state.describe_target();
          return;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto presence = monitor_target_presence(state);

        if (presence == MonitorTargetPresence::unknown) {
          std::this_thread::sleep_for(RECOVERY_CHECK_INTERVAL);
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
          std::this_thread::sleep_for(RECOVERY_CHECK_INTERVAL);
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
          std::this_thread::sleep_for(RECOVERY_CHECK_INTERVAL);
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
          std::this_thread::sleep_for(RECOVERY_CHECK_INTERVAL);
          continue;
        }

        const auto issue_for = now - **issue_since;
        if (issue_for < required_grace) {
          std::this_thread::sleep_for(RECOVERY_CHECK_INTERVAL);
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
          std::this_thread::sleep_for(backoff);
          continue;
        }

        attempts += 1;
        const auto issue_ms = std::chrono::duration_cast<std::chrono::milliseconds>(issue_for).count();
        BOOST_LOG(warning) << "Virtual display recovery monitor detected disappearance for "
                           << state.describe_target() << " (attempt "
                           << attempts << '/' << state.params.max_attempts
                           << ", " << issue_label << "_for=" << issue_ms << "ms).";

        if (monitor_should_abort(state)) {
          BOOST_LOG(debug) << "Virtual display recovery monitor aborted for " << state.describe_target();
          return;
        }
        const bool recovered = attempt_virtual_display_recovery(state);
        inactive_since.reset();
        missing_since.reset();
        active_since.reset();

        if (recovered) {
          observed_active = false;
          recovery_cooldown_until = std::chrono::steady_clock::now() + RECOVERY_POST_SUCCESS_GRACE;
        } else {
          recovery_cooldown_until = std::chrono::steady_clock::now() + RECOVERY_RETRY_DELAY;
        }

        std::this_thread::sleep_for(RECOVERY_RETRY_DELAY);
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

    const auto guid_uuid = guid_to_uuid(params.guid);
    const bool has_device_id = params.device_id && !params.device_id->empty();
    const bool has_display_name = params.display_name && !params.display_name->empty();
    const bool has_client_name = !params.client_name.empty();
    if (!has_device_id && !has_display_name && !has_client_name) {
      BOOST_LOG(debug) << "Virtual display recovery monitor skipped: no identifiers available.";
      return;
    }

    RecoveryMonitorState initial_state(params);
    if (monitor_should_abort(initial_state)) {
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

    const auto abort_flag = reset_recovery_monitor_abort_flag(guid_uuid);
    VirtualDisplayRecoveryParams wrapped = params;
    const auto external_abort = params.should_abort;
    wrapped.should_abort = [abort_flag, external_abort]() {
      if (abort_flag->load(std::memory_order_acquire)) {
        return true;
      }
      return external_abort ? external_abort() : false;
    };

    RecoveryMonitorState state(wrapped);
    state.confirmed_active_at_schedule = true;
    BOOST_LOG(debug) << "Virtual display recovery monitor scheduled for " << state.describe_target()
                     << " (max_attempts=" << params.max_attempts << ").";
    std::thread monitor_thread([state = std::move(state)]() mutable {
      run_virtual_display_recovery_monitor(std::move(state));
    });
    monitor_thread.detach();
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
    return uuid_to_guid(ensure_persistent_guid());
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

  std::unique_ptr<sunshine_driver::WindowsControlTransport> VIRTUAL_DISPLAY_DRIVER_TRANSPORT;

  void closeVDisplayDevice() {
    g_watchdog_stop_requested.store(true, std::memory_order_release);
    stop_watchdog_thread(true);
    setWatchdogFeedingEnabled(false);
    g_watchdog_grace_deadline_ns.store(0, std::memory_order_release);
    VIRTUAL_DISPLAY_DRIVER_TRANSPORT.reset();
  }

  bool ensure_control_transport_responsive(std::string_view operation) {
    if (driver_transport_responsive(VIRTUAL_DISPLAY_DRIVER_TRANSPORT.get())) {
      return true;
    }

    if (VIRTUAL_DISPLAY_DRIVER_TRANSPORT) {
      BOOST_LOG(debug) << operation << ": cached Sunshine virtual display driver transport is not responsive; reopening.";
      closeVDisplayDevice();
    }

    const auto status = openVDisplayDevice();
    if (status != DRIVER_STATUS::OK) {
      BOOST_LOG(warning) << operation << ": failed to open Sunshine virtual display driver transport (status="
                         << static_cast<int>(status) << ").";
      return false;
    }

    if (!driver_transport_responsive(VIRTUAL_DISPLAY_DRIVER_TRANSPORT.get())) {
      BOOST_LOG(warning) << operation << ": opened Sunshine virtual display driver transport is not responsive.";
      closeVDisplayDevice();
      return false;
    }

    return true;
  }

  void ensureVirtualDisplayRegistryDefaults() {
    // The Sunshine driver is runtime-only in this pass and does not require registry defaults.
  }

  DRIVER_STATUS openVDisplayDevice() {
    uint32_t retryInterval = 20;
    bool attempted_recovery = false;
    while (true) {
      auto opened = sunshine_driver::open_first_control_device();
      if (!opened.ok()) {
        if (retryInterval > 320) {
          if (!attempted_recovery) {
            attempted_recovery = true;
            if (ensure_driver_is_ready_impl(RestartCooldownBehavior::wait)) {
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
        Sleep(retryInterval);
        continue;
      }

      VIRTUAL_DISPLAY_DRIVER_TRANSPORT = std::move(opened.transport);
      break;
    }

    sunshine_driver::ControlClient client {*VIRTUAL_DISPLAY_DRIVER_TRANSPORT};
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
      const bool incompatible_protocol = version.status == sunshine_driver::ControlStatus::ProtocolIncompatible;
      const auto failed_status = incompatible_protocol ? DRIVER_STATUS::VERSION_INCOMPATIBLE : DRIVER_STATUS::FAILED;
      return failed_status;
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

    if (!driver_lease_tracker().all().empty()) {
      (void) ensure_watchdog_thread_active_for_lease();
    }

    return DRIVER_STATUS::OK;
  }

  static bool ensure_driver_is_ready_impl(RestartCooldownBehavior cooldown_behavior) {
    if (driver_transport_responsive(VIRTUAL_DISPLAY_DRIVER_TRANSPORT.get())) {
      return true;
    }

    if (VIRTUAL_DISPLAY_DRIVER_TRANSPORT) {
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
        if (try_reenable_disabled_device(*instance_id)) {
          if (probe_driver_responsive_once()) {
            BOOST_LOG(info) << "Sunshine virtual display driver responded after re-enabling disabled device.";
            std::this_thread::sleep_for(DRIVER_RECOVERY_WARMUP_DELAY);
            return true;
          }
        }
      }
    }

    for (int attempt = 1; attempt <= DRIVER_RESTART_MAX_ATTEMPTS; ++attempt) {
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
        std::this_thread::sleep_for(cooldown_remaining);
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

      if (!restart_virtual_display_device(*instance_id)) {
        BOOST_LOG(error) << "Sunshine virtual display adapter restart failed; streaming will continue with the active display. A reboot may be required.";
        note_restart_failure(std::chrono::steady_clock::now());
        continue;
      }

      const auto deadline = std::chrono::steady_clock::now() + DRIVER_RESTART_TIMEOUT;
      while (std::chrono::steady_clock::now() < deadline) {
        if (probe_driver_responsive_once()) {
          BOOST_LOG(info) << "Sunshine virtual display driver responded after restart.";
          std::this_thread::sleep_for(DRIVER_RECOVERY_WARMUP_DELAY);
          return true;
        }
        std::this_thread::sleep_for(DRIVER_RESTART_POLL_INTERVAL);
      }

      BOOST_LOG(error) << "Sunshine virtual display driver did not respond within the restart timeout; streaming will continue with the active display. A reboot may be required.";
      note_restart_failure(std::chrono::steady_clock::now());
    }

    return false;
  }

  bool ensure_driver_is_ready() {
    return ensure_driver_is_ready_impl(RestartCooldownBehavior::skip);
  }

  bool startPingThread(std::function<void()> failCb) {
    stop_watchdog_thread(true);

    // Save the callback so recovery can restart the lease feed thread with the same callback.
    store_watchdog_fail_cb(failCb);
    auto failure_cb = std::make_shared<std::function<void()>>(std::move(failCb));

    if (!ensure_control_transport_responsive("Sunshine virtual display lease feed")) {
      return false;
    }

    auto opened = sunshine_driver::open_first_control_device();
    if (!opened.ok()) {
      printf("[SunshineVirtualDisplay] Lease feed: failed to open control device (status=%s, error=%lu).\n",
             sunshine_driver::to_string(opened.status),
             static_cast<unsigned long>(opened.native_error));
      return false;
    }

    sunshine_driver::ControlClient ping_client {*opened.transport};
    if (!check_driver_protocol_compatible(ping_client)) {
      return false;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto deadline = now + WATCHDOG_INIT_GRACE;
    const bool feed_was_requested = g_watchdog_feed_requested.load(std::memory_order_acquire);
    g_watchdog_stop_requested.store(false, std::memory_order_release);
    g_watchdog_grace_deadline_ns.store(steady_ticks_from_time(deadline), std::memory_order_release);
    g_watchdog_feed_requested.store(feed_was_requested, std::memory_order_release);

    const auto sleep_duration = std::chrono::milliseconds(DRIVER_LEASE_TIMEOUT_MS / 3);

    std::thread ping_thread([sleep_duration, failure_cb = std::move(failure_cb), ping_transport = std::move(opened.transport)]() mutable {
      sunshine_driver::ControlClient client {*ping_transport};
      uint8_t fail_count = 0;
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
          std::this_thread::sleep_for(sleep_duration);
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

        std::this_thread::sleep_for(sleep_duration);
      }
    });

    {
      std::lock_guard<std::mutex> lock(g_watchdog_thread_mutex);
      g_watchdog_thread = std::move(ping_thread);
    }

    return true;
  }

  void setWatchdogFeedingEnabled(bool enable) {
    if (enable) {
      const auto deadline = std::chrono::steady_clock::now() + WATCHDOG_INIT_GRACE;
      g_watchdog_grace_deadline_ns.store(steady_ticks_from_time(deadline), std::memory_order_release);
    }
    g_watchdog_feed_requested.store(enable, std::memory_order_release);
    if (!enable) {
      return;
    }

    const bool transport_ready = ensure_control_transport_responsive("Sunshine virtual display watchdog feed");
    g_watchdog_feed_requested.store(true, std::memory_order_release);
    if (!transport_ready) {
      BOOST_LOG(warning) << "Sunshine virtual display watchdog feed requested but driver transport is unavailable.";
      return;
    }

    (void) ensure_watchdog_thread_active_for_lease();
    g_watchdog_feed_requested.store(true, std::memory_order_release);
  }

  bool set_render_adapter_luid(const LUID &adapter_luid, const std::wstring &adapter_name, SIZE_T dedicated_memory, SIZE_T shared_memory) {
    if (!VIRTUAL_DISPLAY_DRIVER_TRANSPORT || !VIRTUAL_DISPLAY_DRIVER_TRANSPORT->valid()) {
      return false;
    }

    sunshine_driver::ControlClient client {*VIRTUAL_DISPLAY_DRIVER_TRANSPORT};
    sunshine_driver::SetRenderAdapterRequest request {};
    request.adapter_luid = sunshine_driver::from_windows_luid(adapter_luid);
    const auto result = client.set_render_adapter(request);
    if (!result.ok()) {
      BOOST_LOG(warning) << "Failed to set Sunshine virtual display render adapter to '"
                         << platf::to_utf8(adapter_name)
                         << "' (status=" << sunshine_driver::to_string(result.status)
                         << ", native_error=" << result.native_error << ").";
      return false;
    }

    const unsigned long long dedicated_mib = static_cast<unsigned long long>(dedicated_memory / (1024ull * 1024ull));
    const unsigned long long shared_mib = static_cast<unsigned long long>(shared_memory / (1024ull * 1024ull));
    BOOST_LOG(info) << "Sunshine virtual display render adapter set to '"
                    << platf::to_utf8(adapter_name)
                    << "' (dedicated=" << dedicated_mib
                    << " MiB, shared=" << shared_mib << " MiB).";
    return true;
  }

  bool setRenderAdapterByName(const std::wstring &adapterName) {
    if (!VIRTUAL_DISPLAY_DRIVER_TRANSPORT || !VIRTUAL_DISPLAY_DRIVER_TRANSPORT->valid()) {
      return false;
    }

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
      return false;
    }

    for (UINT index = 0;; ++index) {
      Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
      if (factory->EnumAdapters1(index, adapter.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) {
        break;
      }

      DXGI_ADAPTER_DESC1 desc {};
      if (FAILED(adapter->GetDesc1(&desc))) {
        continue;
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
    if (!VIRTUAL_DISPLAY_DRIVER_TRANSPORT || !VIRTUAL_DISPLAY_DRIVER_TRANSPORT->valid()) {
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
      if (factory->EnumAdapters1(index, adapter.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) {
        break;
      }

      DXGI_ADAPTER_DESC1 desc {};
      if (FAILED(adapter->GetDesc1(&desc))) {
        continue;
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

  void apply_configured_render_adapter_preference() {
    if (!config::video.adapter_name.empty()) {
      if (!setRenderAdapterByName(platf::from_utf8(config::video.adapter_name))) {
        BOOST_LOG(warning) << "Sunshine virtual display could not use configured render adapter '"
                           << config::video.adapter_name << "' for display ensure.";
      }
      return;
    }

    (void) setRenderAdapterWithMostDedicatedMemory();
  }

  bool wait_for_virtual_display_ready(
    const std::optional<std::wstring> &display_name,
    std::optional<std::string> &device_id,
    uint32_t width,
    uint32_t height,
    const DisplayConfigIdentity *display_config_identity = nullptr
  ) {
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
    std::optional<std::chrono::steady_clock::time_point> enumerated_at;
    const auto enumeration_timeout = std::chrono::seconds(2);
    const auto activation_grace = std::chrono::milliseconds(500);
    const auto poll_interval = std::chrono::milliseconds(50);
    const bool has_dynamic_hints =
      (device_id && !device_id->empty()) || normalized_name || monitor_path_hint || gdi_name_hint || friendly_name_hint;

    while (true) {
      const auto now = std::chrono::steady_clock::now();
      if (!enumerated_at && now - start >= enumeration_timeout) {
        BOOST_LOG(warning) << "Timed out waiting for Windows to enumerate virtual display.";
        return false;
      }
      if (enumerated_at && now - *enumerated_at >= activation_grace) {
        BOOST_LOG(debug) << "Virtual display was enumerated before final activation/mode details settled; continuing so the display helper can apply the session mode.";
        return true;
      }

      auto attempt_candidate = [&](const display_device::EnumeratedDevice &candidate) -> bool {
        if (!candidate.m_device_id.empty()) {
          if (!device_id || !equals_ci(candidate.m_device_id, *device_id)) {
            device_id = candidate.m_device_id;
          }
        }

        if (!enumerated_at) {
          enumerated_at = now;
        }

        if (candidate.m_info) {
          if (candidate.m_info->m_resolution.m_width == width &&
              candidate.m_info->m_resolution.m_height == height) {
            return true;
          }

          BOOST_LOG(debug) << "Virtual display candidate "
                           << (candidate.m_display_name.empty() ? candidate.m_device_id : candidate.m_display_name)
                           << " is active at " << candidate.m_info->m_resolution.m_width << 'x'
                           << candidate.m_info->m_resolution.m_height << "; waiting for "
                           << width << 'x' << height << '.';
          if (enumerated_at && now - *enumerated_at >= activation_grace) {
            BOOST_LOG(debug) << "Virtual display is active before the requested mode settled; continuing so the display helper can apply the session mode.";
            return true;
          }
          return false;
        }

        if (enumerated_at && now - *enumerated_at >= activation_grace) {
          BOOST_LOG(debug) << "Virtual display is enumerated but not active yet; continuing so the display helper can apply the session mode.";
          return true;
        }

        return false;
      };

      auto devices = platf::display_helper::Coordinator::instance().enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
      if (devices) {
        std::optional<display_device::EnumeratedDevice> unique_resolution_candidate;
        bool resolution_conflict = false;

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

          bool matches = false;
          if (device_id && !device_id->empty() && !candidate.m_device_id.empty()) {
            matches = equals_ci(candidate.m_device_id, *device_id);
          }

          const auto candidate_display_name = !candidate.m_display_name.empty() ? std::make_optional(normalize_display_name(candidate.m_display_name)) : std::nullopt;
          const auto candidate_friendly_name = !candidate.m_friendly_name.empty() ? std::make_optional(normalize_display_name(candidate.m_friendly_name)) : std::nullopt;

          if (!matches && monitor_path_hint && !candidate.m_device_id.empty()) {
            matches = equals_ci(candidate.m_device_id, *monitor_path_hint);
          }

          if (!matches && gdi_name_hint) {
            if (candidate_display_name && *candidate_display_name == *gdi_name_hint) {
              matches = true;
            }
          }

          if (!matches && friendly_name_hint) {
            if (candidate_friendly_name && *candidate_friendly_name == *friendly_name_hint) {
              matches = true;
            }
          }

          if (!matches && normalized_name) {
            if (!candidate.m_display_name.empty() &&
                candidate_display_name && *candidate_display_name == *normalized_name) {
              matches = true;
            } else if (!candidate.m_friendly_name.empty() &&
                       candidate_friendly_name && *candidate_friendly_name == *normalized_name) {
              matches = true;
            }
          }

          if (!matches && !has_dynamic_hints) {
            matches = true;
          }

          if (!matches) {
            continue;
          }

          if (attempt_candidate(candidate)) {
            return true;
          }
        }

        if (!resolution_conflict && unique_resolution_candidate) {
          if (attempt_candidate(*unique_resolution_candidate)) {
            return true;
          }
        }
      }

      std::this_thread::sleep_for(poll_interval);
    }
  }

  bool wait_for_virtual_display_teardown(
    const std::wstring &display_name,
    std::chrono::steady_clock::duration timeout
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

      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return false;
  }

  namespace {

    constexpr auto VIRTUAL_DISPLAY_STABILITY_RECHECK_DELAY = std::chrono::milliseconds(125);

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
      uint32_t height
    ) {
      (void) width;
      (void) height;

      const auto name_utf8 = result.display_name ? platf::to_utf8(*result.display_name) : std::string("(pending)");
      const auto device_utf8 = result.device_id ? *result.device_id : std::string("(unknown)");
      const auto delay_ms = std::chrono::duration_cast<std::chrono::milliseconds>(VIRTUAL_DISPLAY_STABILITY_RECHECK_DELAY).count();

      if (!is_virtual_display_present(result.display_name, result.device_id)) {
        BOOST_LOG(warning) << "Virtual display '" << name_utf8 << "' device_id='" << device_utf8
                           << "' missing immediately after creation.";
        return false;
      }

      std::this_thread::sleep_for(VIRTUAL_DISPLAY_STABILITY_RECHECK_DELAY);

      if (!is_virtual_display_present(result.display_name, result.device_id)) {
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
      bool replace_existing
    ) {
      if (!VIRTUAL_DISPLAY_DRIVER_TRANSPORT || !VIRTUAL_DISPLAY_DRIVER_TRANSPORT->valid()) {
        return std::nullopt;
      }

      uuid_util::uuid_t requested_uuid {};
      std::memcpy(requested_uuid.b8, &guid, sizeof(requested_uuid.b8));

      // Log entry and inputs for deeper diagnostics
      BOOST_LOG(debug) << "createVirtualDisplay called: client_uid='" << (s_client_uid ? s_client_uid : "(null)")
                       << "' client_name='" << (s_client_name ? s_client_name : "(null)")
                       << "' hdr_profile='" << (s_hdr_profile ? s_hdr_profile : "(null)")
                       << "' width=" << width << " height=" << height << " fps=" << fps
                       << " hdr_requested=" << hdr_requested
                       << " guid=" << requested_uuid.string();

      release_retained_ensure_display_for_stream(guid, s_client_uid);
      teardown_conflicting_virtual_displays(requested_uuid);
      BOOST_LOG(debug) << "teardown_conflicting_virtual_displays completed for guid=" << requested_uuid.string();
      enforce_teardown_cooldown_if_needed();

      sunshine_driver::ControlClient client {*VIRTUAL_DISPLAY_DRIVER_TRANSPORT};
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
      const auto lease_id = generate_driver_lease_id();
      const auto dpi_settings_prefix = virtual_display_dpi_settings_prefix(display_id);
      const auto configured_scale = config::video.dd.virtual_display_scale_percent;
      const auto dpi_snapshot = configured_scale == 0 ?
        read_virtual_display_dpi_value(dpi_settings_prefix) :
        std::nullopt;

      sunshine_driver::CreateTemporaryDisplayRequest create_request {};
      create_request.lease_id = lease_id;
      create_request.display_id = display_id;
      create_request.width = width;
      create_request.height = height;
      if (configured_scale > 0) {
        const auto dpi = 96.0 * static_cast<double>(configured_scale) / 100.0;
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
                       << display_id << ", lease_id=" << lease_id
                       << ", HDR peak=" << create_request.hdr_max_luminance_nits << " nits"
                       << ", scale=" << configured_scale << "%"
                       << ", physical=" << create_request.physical_width_mm << 'x'
                       << create_request.physical_height_mm << " mm).";
      const auto create_result = client.create_temporary_display(create_request);
      if (!create_result.ok()) {
        const DWORD error_code = create_result.native_error;
        BOOST_LOG(warning) << "Sunshine temporary display create failed: status="
                           << sunshine_driver::to_string(create_result.status)
                           << " error=" << error_code
                           << " guid=" << requested_uuid.string() << " display_id=" << display_id;

        if (replace_existing) {
          BOOST_LOG(info) << "Sunshine temporary display create collided with existing state for guid="
                          << requested_uuid.string() << "; removing it before retry instead of reusing it.";
          (void) removeVirtualDisplay(guid);
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

        if (dpi_snapshot) {
          (void) apply_virtual_display_dpi_value(*dpi_snapshot);
        }

        if (reuse_name || device_id) {
          BOOST_LOG(debug) << "Waiting for virtual display ready (reuse). display_name='"
                           << (reuse_name ? platf::to_utf8(*reuse_name) : std::string("(none)"))
                           << "' device_id='" << (device_id ? *device_id : std::string("(none)")) << "'";
          std::optional<std::wstring> display_name = reuse_name;
          if (wait_for_virtual_display_ready(display_name, device_id, width, height)) {
            if (display_name) {
              wprintf(
                L"[SunshineVirtualDisplay] Reusing existing virtual display (error=%lu): %ls\n",
                static_cast<unsigned long>(error_code),
                display_name->c_str()
              );
            } else {
              printf("[SunshineVirtualDisplay] Reusing existing virtual display (error=%lu).\n", static_cast<unsigned long>(error_code));
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

            result.monitor_device_path = resolve_monitor_device_path(display_name, result.device_id);
            result.reused_existing = true;
            result.ready_since = ready_since;
            if (!adopt_existing_driver_lease(client, requested_uuid, display_id, result.display_name, result.device_id, result.monitor_device_path)) {
              BOOST_LOG(warning) << "Refusing to reuse existing Sunshine virtual display for guid="
                                 << requested_uuid.string() << " because its driver lease could not be adopted.";
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
              true
            );
            return result;
          }
        }

        printf("[SunshineVirtualDisplay] Failed to add virtual display (status=%s, error=%lu).\n",
               sunshine_driver::to_string(create_result.status),
               static_cast<unsigned long>(error_code));
        return std::nullopt;
      }

      const DisplayConfigTarget output {
        sunshine_driver::to_windows_luid(create_result.value.os_adapter_luid),
        create_result.value.target_id
      };

      driver_lease_tracker().put(
        requested_uuid,
        DriverLeaseInfo {
          create_result.value.display_id != 0 ? create_result.value.display_id : display_id,
          create_result.value.lease_id != 0 ? create_result.value.lease_id : lease_id,
          std::nullopt,
          std::nullopt,
          std::nullopt
        }
      );
      (void) ensure_watchdog_thread_active_for_lease();

      auto display_config_identity = wait_for_display_config_identity(output);

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

      const auto has_target_identity = display_config_identity && display_config_identity_has_display_name(*display_config_identity);
      const auto display_config_ptr = has_target_identity ? &*display_config_identity : nullptr;
      if (!resolved_display_name && !device_id && !has_target_identity) {
        BOOST_LOG(debug) << "Sunshine temporary display created before Windows exposed a target-specific display identity; waiting for virtual display enumeration.";
      }

      if (!wait_for_virtual_display_ready(resolved_display_name, device_id, width, height, display_config_ptr)) {
        if (allow_pending_enumeration) {
          BOOST_LOG(warning) << "Sunshine temporary display was accepted by the driver, but Windows display enumeration is unavailable; retaining it for encoder probing.";

          const auto ready_since = std::chrono::steady_clock::now();
          VirtualDisplayCreationResult result;
          result.display_name = resolved_display_name;
          if (device_id && !device_id->empty()) {
            result.device_id = *device_id;
          }
          if (s_client_name && std::strlen(s_client_name) > 0) {
            result.client_name = std::string(s_client_name);
          }
          result.reused_existing = false;
          result.ready_since = ready_since;
          driver_lease_tracker().update_identity(requested_uuid, result.display_name, result.device_id, result.monitor_device_path);
          return result;
        }

        printf("[SunshineVirtualDisplay] Timed out waiting for Windows to enumerate the new virtual display; reverting creation.\n");
        (void) removeVirtualDisplay(guid);
        return std::nullopt;
      }

      if (hdr_requested && !request_hdr10_advanced_color(output)) {
        BOOST_LOG(warning) << "Sunshine virtual display HDR: requested HDR display did not become HDR-capable; continuing with SDR capture.";
      }

      if (dpi_snapshot) {
        (void) apply_virtual_display_dpi_value(*dpi_snapshot);
      }

      // Prefer a real GDI display name (\\.\DISPLAYx) over GUID placeholders once enumeration is complete.
      if (resolved_display_name && !resolved_display_name->empty() && !is_gdi_display_name(*resolved_display_name)) {
        std::optional<std::wstring> gdi_name;
        if (auto identity = wait_for_display_config_identity(output, std::chrono::milliseconds(250))) {
          if (identity->source_gdi_device_name && !identity->source_gdi_device_name->empty()) {
            gdi_name = identity->source_gdi_device_name;
            display_config_identity = identity;
          }
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
      printf("[SunshineVirtualDisplay] Configuration: W: %d, H: %d, FPS: %d, DisplayId: %llu, LeaseId: %llu\n",
             width,
             height,
             requested_fps,
             static_cast<unsigned long long>(create_result.value.display_id != 0 ? create_result.value.display_id : display_id),
             static_cast<unsigned long long>(create_result.value.lease_id != 0 ? create_result.value.lease_id : lease_id));

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
      result.reused_existing = false;
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
        true
      );
      driver_lease_tracker().update_identity(requested_uuid, result.display_name, result.device_id, result.monitor_device_path);
      return result;
    }

  }  // namespace

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
    constexpr int kMaxInitializationAttempts = 3;
    const auto requested_uuid = guid_to_uuid(guid);

    for (int attempt = 1; attempt <= kMaxInitializationAttempts; ++attempt) {
      if (!ensure_control_transport_responsive("Sunshine virtual display creation")) {
        BOOST_LOG(warning) << "Unable to open Sunshine virtual display driver transport for virtual display creation.";
        return std::nullopt;
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
        hdr_requested,
        allow_pending_enumeration,
        replace_existing
      );
      if (!result) {
        BOOST_LOG(warning) << "Virtual display creation attempt " << attempt << '/' << kMaxInitializationAttempts
                           << " failed.";

        if (attempt == kMaxInitializationAttempts) {
          BOOST_LOG(error) << "Virtual display could not be created after " << kMaxInitializationAttempts << " attempts.";
          return std::nullopt;
        }

        closeVDisplayDevice();

        if (!ensure_driver_is_ready_impl(RestartCooldownBehavior::wait)) {
          BOOST_LOG(warning) << "Driver recovery failed after virtual display creation failure.";
          return std::nullopt;
        }

        if (openVDisplayDevice() != DRIVER_STATUS::OK) {
          BOOST_LOG(warning) << "Failed to re-open Sunshine virtual display driver after recovery.";
          return std::nullopt;
        }

        BOOST_LOG(info) << "Retrying Sunshine virtual display initialization (attempt "
                        << (attempt + 1) << '/' << kMaxInitializationAttempts << ").";
        continue;
      }

      if (allow_pending_enumeration || confirm_virtual_display_persistence(*result, width, height)) {
        if (config::video.dd.virtual_display_scale_percent > 0) {
          if (!result->monitor_device_path) {
            result->monitor_device_path = resolve_monitor_device_path(result->display_name, result->device_id);
          }
          if (result->monitor_device_path) {
            const auto scale_result = VDISPLAY::set_display_scale_percent(
              *result->monitor_device_path,
              static_cast<std::uint32_t>(config::video.dd.virtual_display_scale_percent)
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
          } else if (!allow_pending_enumeration) {
            BOOST_LOG(warning) << "Virtual display scale: monitor device path was unavailable; Windows scale was not applied.";
          }
        }
        track_virtual_display_created(requested_uuid);
        return result;
      }

      const auto name_utf8 = result->display_name ? platf::to_utf8(*result->display_name) : std::string("(pending)");
      BOOST_LOG(warning) << "Virtual display '" << name_utf8 << "' vanished after creation attempt "
                         << attempt << '/' << kMaxInitializationAttempts << "; recovering driver.";

      if (attempt == kMaxInitializationAttempts) {
        break;
      }

      closeVDisplayDevice();

      if (!ensure_driver_is_ready_impl(RestartCooldownBehavior::wait)) {
        BOOST_LOG(warning) << "Driver recovery failed after virtual display vanished.";
        return std::nullopt;
      }

      if (openVDisplayDevice() != DRIVER_STATUS::OK) {
        BOOST_LOG(warning) << "Failed to re-open Sunshine virtual display driver after recovery.";
        return std::nullopt;
      }

      BOOST_LOG(info) << "Retrying Sunshine virtual display initialization (attempt "
                      << (attempt + 1) << '/' << kMaxInitializationAttempts << ").";
    }

    BOOST_LOG(error) << "Virtual display could not be stabilized after " << kMaxInitializationAttempts << " attempts.";
    return std::nullopt;
  }

  bool removeAllVirtualDisplays() {
    abort_all_recovery_monitors();
    auto all_guids = active_virtual_display_tracker().all();
    if (all_guids.empty()) {
      BOOST_LOG(debug) << "No active virtual displays to remove.";
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
    abort_recovery_monitor(guid_to_uuid(guid));
    const auto guid_uuid = guid_to_uuid(guid);
    const auto lease_info = driver_lease_tracker().get(guid_uuid);
    auto cached_display_name = lease_info && lease_info->display_name ? lease_info->display_name : resolve_virtual_display_name_from_devices();

    const bool initial_transport_invalid = !driver_transport_responsive(VIRTUAL_DISPLAY_DRIVER_TRANSPORT.get());
    bool opened_handle = false;

    auto ensure_handle = [&]() -> bool {
      if (driver_transport_responsive(VIRTUAL_DISPLAY_DRIVER_TRANSPORT.get())) {
        return true;
      }

      if (VIRTUAL_DISPLAY_DRIVER_TRANSPORT) {
        printf("[SunshineVirtualDisplay] Cached driver transport is not responsive while removing virtual display; reopening.\n");
        closeVDisplayDevice();
      }

      if (openVDisplayDevice() != DRIVER_STATUS::OK) {
        printf("[SunshineVirtualDisplay] Failed to open driver while removing virtual display.\n");
        return false;
      }
      opened_handle = true;
      if (!driver_transport_responsive(VIRTUAL_DISPLAY_DRIVER_TRANSPORT.get())) {
        closeVDisplayDevice();
        return false;
      }
      return true;
    };

    auto perform_remove = [&]() -> std::pair<bool, DWORD> {
      DWORD error_code = ERROR_SUCCESS;

      sunshine_driver::ControlClient client {*VIRTUAL_DISPLAY_DRIVER_TRANSPORT};
      if (lease_info) {
        sunshine_driver::LeaseRequest release_request {};
        release_request.lease_id = lease_info->lease_id;
        const auto released = client.release_lease(release_request);
        if (released.ok()) {
          track_virtual_display_removed(guid_uuid);
          note_virtual_display_teardown();
          return std::pair<bool, DWORD> {true, ERROR_SUCCESS};
        }

        error_code = released.native_error;
        if (released.status == sunshine_driver::ControlStatus::TransportFailed && is_missing_lease_error(error_code)) {
          track_virtual_display_removed(guid_uuid);
          note_virtual_display_teardown();
          return std::pair<bool, DWORD> {true, error_code};
        }
      }

      sunshine_driver::LeaseDisplayRequest remove_request {};
      remove_request.lease_id = lease_info ? lease_info->lease_id : 0;
      remove_request.display_id = lease_info ? lease_info->display_id : client_uuid_to_virtual_display_id(guid);
      const auto removed = client.remove_temporary_display(remove_request);
      if (removed.ok()) {
        track_virtual_display_removed(guid_uuid);
        note_virtual_display_teardown();
        return std::pair<bool, DWORD> {true, ERROR_SUCCESS};
      }

      error_code = removed.native_error;
      if (removed.status == sunshine_driver::ControlStatus::TransportFailed && is_missing_lease_error(error_code)) {
        track_virtual_display_removed(guid_uuid);
        note_virtual_display_teardown();
        return std::pair<bool, DWORD> {true, error_code};
      }

      return {false, error_code};
    };

    if (!ensure_handle()) {
      return false;
    }

    auto [removed, error_code] = perform_remove();
    if (!removed && error_code == ERROR_INVALID_HANDLE) {
      printf("[SunshineVirtualDisplay] Driver transport became invalid while removing virtual display; retrying.\n");
      closeVDisplayDevice();
      if (openVDisplayDevice() == DRIVER_STATUS::OK) {
        opened_handle = true;
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
        if (!wait_for_virtual_display_teardown(*cached_display_name, teardown_timeout)) {
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
    if (driver_transport_responsive(VIRTUAL_DISPLAY_DRIVER_TRANSPORT.get())) {
      return true;
    }

    if (probe_driver_responsive_once()) {
      return true;
    }

    return find_virtual_display_device_instance_id().has_value();
  }

  bool isVirtualDisplayDriverInstalled() {
    return is_sunshine_driver_installed_passive();
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
      if (device.m_info) {
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
      if (!active_any_match && device.m_info) {
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
        if (device.m_info) {
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
        if (device.m_info) {
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

      if (device.m_info) {
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
        if (device.m_info) {
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
    return equals_ci(output_identifier, VIRTUAL_DISPLAY_SELECTION);
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
      info.is_active = device.m_info.has_value() || !device.m_display_name.empty();
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
    BOOST_LOG(debug) << "No display devices detected, therefore returning false.";
    return false;
  }

  for (const auto &device : *devices) {
    bool is_virtual = is_virtual_display_device(device);
    if (!is_virtual) {
      bool is_active = !device.m_display_name.empty();
      BOOST_LOG(debug) << "Physical device: " << device.m_display_name << ", is_active: " << is_active;
      if (is_active) {
        return true;
      }
    }
  }

  BOOST_LOG(debug) << "No active physical display found, returning false";
  return false;
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
  return virtualDisplayUuidFromStableId("sunshine-ensure");
}

VDISPLAY_SUNSHINE::ensure_display_result VDISPLAY_SUNSHINE::ensure_display() {
  ensure_display_result result {false, false, false, {}};

  if (has_active_physical_display()) {
    result.success = true;
    return result;
  }

  if (!should_auto_enable_virtual_display()) {
    BOOST_LOG(debug) << "No active physical displays and virtual display auto-enable is disabled.";
    return result;
  }

  if (proc::vDisplayDriverStatus != DRIVER_STATUS::OK) {
    proc::initVDisplayDriver();
    if (proc::vDisplayDriverStatus != DRIVER_STATUS::OK) {
      BOOST_LOG(warning) << "Virtual display driver unavailable for display ensure (status=" << static_cast<int>(proc::vDisplayDriverStatus) << "). Continuing with best-effort ensure.";
    }
  }

  auto uuid = persistentVirtualDisplayUuid();
  std::memcpy(&result.temporary_guid, uuid.b8, sizeof(result.temporary_guid));

  {
    std::lock_guard<std::mutex> lock(g_ensure_display_state_mutex);
    if (g_ensure_display_retained && guid_equal(g_ensure_display_guid, result.temporary_guid)) {
      if (is_virtual_display_guid_tracked(result.temporary_guid)) {
        result.success = true;
        result.tracks_temporary_for_probe = true;
        BOOST_LOG(info) << "Reusing retained temporary virtual display for encoder probing (failure_count="
                        << g_ensure_display_failure_count << ").";
        return result;
      }

      g_ensure_display_retained = false;
      g_ensure_display_failure_count = 0;
      std::memset(&g_ensure_display_guid, 0, sizeof(g_ensure_display_guid));
      BOOST_LOG(debug) << "Ensure display retention state was stale; creating a fresh temporary display.";
    }
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
    BOOST_LOG(debug) << "Active virtual display already exists.";
    result.success = true;
    return result;
  }

  apply_configured_render_adapter_preference();

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
  result.success = true;
  {
    std::lock_guard<std::mutex> lock(g_ensure_display_state_mutex);
    g_ensure_display_retained = true;
    g_ensure_display_guid = result.temporary_guid;
    g_ensure_display_failure_count = 0;
  }

  // Wait for DXGI to enumerate the new virtual display.
  // CCD (used by wait_for_virtual_display_ready) and DXGI are different enumeration
  // paths; DXGI may lag behind CCD by hundreds of milliseconds.
  {
    const auto dxgi_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    bool dxgi_ready = false;
    while (std::chrono::steady_clock::now() < dxgi_deadline) {
      auto names = platf::display_names(platf::mem_type_e::dxgi);
      if (!names.empty()) {
        dxgi_ready = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!dxgi_ready) {
      BOOST_LOG(warning) << "Temporary virtual display created but DXGI has not enumerated it yet; probe may fail.";
    }
  }

  BOOST_LOG(info) << "Temporary virtual display ready.";
  return result;
}

void VDISPLAY_SUNSHINE::cleanup_ensure_display(const ensure_display_result &result, bool probe_succeeded, bool allow_temporary_teardown) {
  if (!result.tracks_temporary_for_probe) {
    return;
  }

  GUID guid_to_remove {};
  bool should_remove = false;
  int failure_count = 0;
  {
    std::lock_guard<std::mutex> lock(g_ensure_display_state_mutex);

    if (!g_ensure_display_retained || !guid_equal(g_ensure_display_guid, result.temporary_guid)) {
      return;
    }

    if (probe_succeeded) {
      g_ensure_display_failure_count = 0;
      if (allow_temporary_teardown) {
        guid_to_remove = g_ensure_display_guid;
        g_ensure_display_retained = false;
        std::memset(&g_ensure_display_guid, 0, sizeof(g_ensure_display_guid));
        should_remove = true;
      }
    } else {
      ++g_ensure_display_failure_count;
      failure_count = g_ensure_display_failure_count;
      if (allow_temporary_teardown && g_ensure_display_failure_count >= ENSURE_DISPLAY_MAX_RETRY_FAILURES) {
        guid_to_remove = g_ensure_display_guid;
        g_ensure_display_retained = false;
        g_ensure_display_failure_count = 0;
        std::memset(&g_ensure_display_guid, 0, sizeof(g_ensure_display_guid));
        should_remove = true;
      }
    }
  }

  if (!probe_succeeded) {
    if (should_remove) {
      BOOST_LOG(warning) << "Encoder probe failed " << ENSURE_DISPLAY_MAX_RETRY_FAILURES
                         << " times with retained temporary display; resetting it.";
    } else {
      BOOST_LOG(info) << "Keeping temporary virtual display for probe retry (failure "
                      << failure_count << '/' << ENSURE_DISPLAY_MAX_RETRY_FAILURES << ").";
    }
  }

  if (!should_remove) {
    if (probe_succeeded && !allow_temporary_teardown) {
      BOOST_LOG(debug) << "Temporary virtual display retained because teardown is currently disallowed.";
    }
    return;
  }

  if (!removeVirtualDisplay(guid_to_remove)) {
    BOOST_LOG(warning) << "Failed to remove temporary virtual display.";
  } else {
    BOOST_LOG(info) << "Removed temporary virtual display.";
  }
}

bool VDISPLAY_SUNSHINE::has_retained_ensure_display() {
  std::lock_guard<std::mutex> lock(g_ensure_display_state_mutex);
  if (!g_ensure_display_retained) {
    return false;
  }
  return is_virtual_display_guid_tracked(g_ensure_display_guid);
}
