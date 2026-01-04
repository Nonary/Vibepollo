#include "virtual_display.h"

#include "src/config.h"
#include "src/logging.h"
#include "src/process.h"
#include "src/state_storage.h"
#include "src/platform/common.h"
#include "src/platform/windows/misc.h"
#include "src/platform/windows/display_helper_coordinator.h"
#include "src/uuid.h"

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <algorithm>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <cmath>
#include <combaseapi.h>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <cfgmgr32.h>
#include <dxgi.h>
#include <dxgi1_6.h>
#include <devguid.h>
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
#include <string_view>
#include <setupapi.h>
#include <system_error>
#include <thread>
#include <utility>
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

using namespace SUDOVDA;

namespace VDISPLAY {
  namespace {
    constexpr auto WATCHDOG_INIT_GRACE = std::chrono::seconds(30);
    constexpr auto DRIVER_RESTART_TIMEOUT = std::chrono::seconds(5);
    constexpr auto DRIVER_RESTART_POLL_INTERVAL = std::chrono::milliseconds(500);
    constexpr auto DRIVER_RESTART_FAILURE_COOLDOWN = std::chrono::seconds(10);
    constexpr auto DEVICE_RESTART_SETTLE_DELAY = std::chrono::milliseconds(200);
    constexpr auto VIRTUAL_DISPLAY_TEARDOWN_COOLDOWN = std::chrono::milliseconds(250);
    constexpr std::wstring_view SUDOVDA_HARDWARE_ID = L"root\\sudomaker\\sudovda";
    constexpr std::wstring_view SUDOVDA_FRIENDLY_NAME_W = L"SudoMaker Virtual Display Adapter";

    std::atomic<bool> g_watchdog_feed_requested {false};
    std::atomic<std::int64_t> g_watchdog_grace_deadline_ns {0};
    std::atomic<std::int64_t> g_last_teardown_ns {0};
    std::atomic<std::int64_t> g_last_restart_failure_ns {0};

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

    uint32_t apply_refresh_overrides(uint32_t fps_millihz, uint32_t base_fps_millihz = 0u, bool framegen_refresh_active = false) {
      constexpr uint64_t scale = 1000ull;
      // Either option (virtual_double_refresh or framegen) requests a minimum of 2x base fps
      const bool needs_double_minimum = config::video.double_refreshrate || framegen_refresh_active;
      if (needs_double_minimum && base_fps_millihz > 0) {
        const uint64_t minimum_millihz = static_cast<uint64_t>(base_fps_millihz) * 2ull;
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
      explicit DevInfoHandle(HDEVINFO value) : handle(value) {}

      DevInfoHandle(const DevInfoHandle &) = delete;
      DevInfoHandle &operator=(const DevInfoHandle &) = delete;

      DevInfoHandle(DevInfoHandle &&other) noexcept : handle(other.handle) {
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

      if (!apply_device_state_change(info.get(), device_info, DICS_ENABLE)) {
        return false;
      }

      return true;
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

    void track_virtual_display_created(const uuid_util::uuid_t &guid) {
      active_virtual_display_tracker().add(guid);
    }

    void track_virtual_display_removed(const uuid_util::uuid_t &guid) {
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
      scoped_reg_key_t(HKEY k, bool should_close): key(k), close(should_close) {}

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
      // Format: \\?\DISPLAY#SMKD1CE#1&28a6823a&2&UID265#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}
      size_t first_hash = device_path.find(L'#');
      if (first_hash == std::wstring::npos) return std::nullopt;
      size_t second_hash = device_path.find(L'#', first_hash + 1);
      if (second_hash == std::wstring::npos) return std::nullopt;
      size_t third_hash = device_path.find(L'#', second_hash + 1);
      if (third_hash == std::wstring::npos) return std::nullopt;

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
      LSTATUS status = RegQueryValueExW(enum_key, L"Driver", nullptr, &driver_type,
                                        reinterpret_cast<LPBYTE>(driver_value), &driver_size);
      RegCloseKey(enum_key);

      if (status != ERROR_SUCCESS || driver_type != REG_SZ) {
        return std::nullopt;
      }

      std::wstring driver_str(driver_value);
      size_t backslash = driver_str.rfind(L'\\');
      if (backslash == std::wstring::npos) return std::nullopt;
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
      LSTATUS status = RegQueryValueExW(profile_key, L"ICMProfileAC", nullptr, &profile_type,
                                        reinterpret_cast<LPBYTE>(profile_value), &profile_size);
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
      LSTATUS status = RegCreateKeyExW(root.key, profile_assoc_path->c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                                       &profile_key, nullptr);
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
      (void) RegSetValueExW(profile_key, L"UsePerUserProfiles", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&use_per_user),
                            sizeof(use_per_user));

      // Write ICMProfileAC as REG_MULTI_SZ
      std::vector<wchar_t> multi_sz(profile_filename.begin(), profile_filename.end());
      multi_sz.push_back(L'\0');
      multi_sz.push_back(L'\0');

      status = RegSetValueExW(profile_key, L"ICMProfileAC", 0, REG_MULTI_SZ,
                              reinterpret_cast<const BYTE *>(multi_sz.data()),
                              static_cast<DWORD>(multi_sz.size() * sizeof(wchar_t)));
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

    void apply_hdr_profile_if_available(
      const std::optional<std::wstring> &display_name,
      const std::optional<std::string> &device_id,
      const std::optional<std::wstring> &monitor_device_path,
      const std::optional<std::string> &client_name_utf8,
      const std::optional<std::string> &hdr_profile_utf8,
      bool is_virtual_display = true
    ) {
      // Only apply HDR profiles when explicitly selected by the user.
      if (!hdr_profile_utf8 || hdr_profile_utf8->empty()) {
        return;
      }

      const std::string client_name = (client_name_utf8 && !client_name_utf8->empty()) ? *client_name_utf8 : "unknown";

      const std::optional<fs::path> profile_path = find_hdr_profile_by_selection(*hdr_profile_utf8);
      if (!profile_path) {
        BOOST_LOG(warning) << "HDR profile: configured profile '" << *hdr_profile_utf8 << "' not found in '"
                           << platf::to_utf8(default_color_profile_directory().wstring()) << "' for client '" << client_name
                           << "'.";
        return;
      }

      // For virtual displays, clear mismatched associations (Windows can reuse IDs).
      const bool should_clear_mismatched = is_virtual_display;

      // Run asynchronously to avoid blocking stream startup
      std::thread([
        profile_path,
        client_name,
        monitor_path = monitor_device_path,
        display_name,
        device_id,
        should_clear_mismatched
      ]() {
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
            existing = read_color_profile_from_registry(*device_name_w, scope);
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
              if (expected_filename.empty() || _wcsicmp(existing->c_str(), expected_filename.c_str()) != 0) {
                BOOST_LOG(debug) << "HDR profile: clearing mismatched profile '" << platf::to_utf8(*existing)
                                 << "' from virtual display for client '" << client_name << "'.";
                if (clear_color_profile_from_registry(*device_name_w, scope)) {
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
              _wcsicmp(existing->c_str(), profile_filename.c_str()) == 0;

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

            // Write directly to registry (WCS APIs don't work reliably for new virtual displays)
            LSTATUS reg_status = ERROR_SUCCESS;
            local_success = write_color_profile_to_registry(*device_name_w, profile_filename, scope, &reg_status);
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
      }).detach();
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
      if (QueryDisplayConfig(flags, &path_count, paths.data(), &mode_count, modes.data(), nullptr) != ERROR_SUCCESS) {
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
      LONG qdc_result = QueryDisplayConfig(flags, &path_count, paths.data(), &mode_count, modes.data(), nullptr);
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
      try {
        if (fs::exists(path)) {
          pt::read_json(path.string(), tree);
        }
      } catch (...) {
        tree = pt::ptree {};
      }

      tree.put("root.virtual_display_guid", uuid.string());

      try {
        if (!path.empty()) {
          auto dir = path;
          dir.remove_filename();
          if (!dir.empty()) {
            fs::create_directories(dir);
          }
        }
        pt::write_json(path.string(), tree);
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
    constexpr auto RECOVERY_CHECK_INTERVAL = std::chrono::milliseconds(200);
    constexpr auto RECOVERY_RETRY_DELAY = std::chrono::milliseconds(350);
    constexpr auto RECOVERY_MISSING_GRACE = std::chrono::seconds(1);
    constexpr auto RECOVERY_INACTIVE_GRACE = std::chrono::seconds(2);
    constexpr auto RECOVERY_NO_ACTIVE_GRACE = std::chrono::seconds(10);
    constexpr auto RECOVERY_POST_SUCCESS_GRACE = std::chrono::seconds(2);
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

    MonitorTargetPresence monitor_target_presence(RecoveryMonitorState &state) {
      auto devices = platf::display_helper::Coordinator::instance().enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
      if (!devices) {
        return MonitorTargetPresence::unknown;
      }

      bool matched_inactive = false;
      for (const auto &device : *devices) {
        if (!is_virtual_display_device(device)) {
          continue;
        }

        bool matches = false;
        bool matched_by_client_name = false;
        if (!matches && !state.params.client_name.empty() && !device.m_friendly_name.empty()
            && equals_ci(device.m_friendly_name, state.params.client_name)) {
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
        state.params.framegen_refresh_active
      );
      if (!recreation) {
        BOOST_LOG(warning) << "Virtual display recovery: createVirtualDisplay failed for " << state.describe_target();
        return false;
      }

      state.update_identifiers(recreation->display_name, recreation->device_id, recreation->monitor_device_path);
      if (state.params.on_recovery_success) {
        state.params.on_recovery_success(*recreation);
      }
      return true;
    }

    void run_virtual_display_recovery_monitor(RecoveryMonitorState state) {
      unsigned int attempts = 0;
      unsigned int backoff_cycles = 0;
      bool observed_active = false;
      std::optional<std::chrono::steady_clock::time_point> active_since;
      std::optional<std::chrono::steady_clock::time_point> inactive_since;
      std::optional<std::chrono::steady_clock::time_point> missing_since;
      auto recovery_cooldown_until = std::chrono::steady_clock::now();

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
            ok = write_color_profile_to_registry(monitor_path, *previous, color_profile_scope_e::current_user);
          } else {
            ok = clear_color_profile_from_registry(monitor_path, color_profile_scope_e::current_user);
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
    BOOST_LOG(debug) << "Virtual display recovery monitor scheduled for " << state.describe_target()
                     << " (max_attempts=" << params.max_attempts << ").";
    std::thread monitor_thread([state = std::move(state)]() mutable {
      run_virtual_display_recovery_monitor(std::move(state));
    });
    monitor_thread.detach();
  }

  // {dff7fd29-5b75-41d1-9731-b32a17a17104}
  // static const GUID DEFAULT_DISPLAY_GUID = { 0xdff7fd29, 0x5b75, 0x41d1, { 0x97, 0x31, 0xb3, 0x2a, 0x17, 0xa1, 0x71, 0x04 } };

  HANDLE SUDOVDA_DRIVER_HANDLE = INVALID_HANDLE_VALUE;

  void closeVDisplayDevice() {
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

  DRIVER_STATUS openVDisplayDevice() {
    uint32_t retryInterval = 20;
    bool attempted_recovery = false;
    while (true) {
      SUDOVDA_DRIVER_HANDLE = OpenDevice(&SUVDA_INTERFACE_GUID);
      if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
        if (retryInterval > 320) {
          if (!attempted_recovery) {
            attempted_recovery = true;
            if (ensure_driver_is_ready()) {
              retryInterval = 20;
              continue;
            }
          }

          printf("[SUDOVDA] Open device failed!\n");
          return DRIVER_STATUS::FAILED;
        }
        retryInterval *= 2;
        Sleep(retryInterval);
        continue;
      }

      break;
    }

    if (!CheckProtocolCompatible(SUDOVDA_DRIVER_HANDLE)) {
      printf("[SUDOVDA] SUDOVDA protocol not compatible with driver!\n");
      closeVDisplayDevice();
      return DRIVER_STATUS::VERSION_INCOMPATIBLE;
    }

    return DRIVER_STATUS::OK;
  }

  bool ensure_driver_is_ready() {
    if (driver_handle_responsive(SUDOVDA_DRIVER_HANDLE)) {
      return true;
    }

    if (SUDOVDA_DRIVER_HANDLE != INVALID_HANDLE_VALUE) {
      closeVDisplayDevice();
    }

    if (probe_driver_responsive_once()) {
      return true;
    }

    const auto now = std::chrono::steady_clock::now();
    std::chrono::milliseconds cooldown_remaining {0};
    if (should_skip_restart_attempt(now, cooldown_remaining)) {
      BOOST_LOG(warning) << "Skipping SudoVDA restart attempt due to recent failure (cooldown "
                         << cooldown_remaining.count() << " ms remaining).";
      return false;
    }

    bool waited_for_restart = false;
    auto instance_id = find_sudovda_device_instance_id();
    if (!instance_id) {
      BOOST_LOG(error) << "Unable to locate SudoVDA adapter for recovery; streaming will continue with the active display. A reboot may be required.";
      note_restart_failure(now);
      return false;
    }

    BOOST_LOG(info) << "Attempting to restart SudoVDA adapter " << platf::to_utf8(*instance_id) << '.';

    if (!restart_sudovda_device(*instance_id)) {
      BOOST_LOG(error) << "SudoVDA adapter restart failed; streaming will continue with the active display. A reboot may be required.";
      note_restart_failure(now);
      return false;
    }

    waited_for_restart = true;
    const auto deadline = std::chrono::steady_clock::now() + DRIVER_RESTART_TIMEOUT;
    while (std::chrono::steady_clock::now() < deadline) {
      if (probe_driver_responsive_once()) {
        BOOST_LOG(info) << "SudoVDA driver responded after restart.";
        if (waited_for_restart) {
          std::this_thread::sleep_for(DRIVER_RECOVERY_WARMUP_DELAY);
        }
        return true;
      }
      std::this_thread::sleep_for(DRIVER_RESTART_POLL_INTERVAL);
    }

    BOOST_LOG(error) << "SudoVDA driver did not respond within the restart timeout; streaming will continue with the active display. A reboot may be required.";
    note_restart_failure(std::chrono::steady_clock::now());
    return false;
  }

  bool startPingThread(std::function<void()> failCb) {
    if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
      return false;
    }

    VIRTUAL_DISPLAY_GET_WATCHDOG_OUT watchdogOut;
    if (GetWatchdogTimeout(SUDOVDA_DRIVER_HANDLE, watchdogOut)) {
      printf("[SUDOVDA] Watchdog: Timeout %d, Countdown %d\n", watchdogOut.Timeout, watchdogOut.Countdown);
    } else {
      printf("[SUDOVDA] Watchdog fetch failed!\n");
      return false;
    }

    if (!watchdogOut.Timeout) {
      return true;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto deadline = now + WATCHDOG_INIT_GRACE;
    g_watchdog_grace_deadline_ns.store(steady_ticks_from_time(deadline), std::memory_order_release);
    g_watchdog_feed_requested.store(false, std::memory_order_release);

    const auto interval_ms = std::max<long long>(static_cast<long long>(watchdogOut.Timeout) * 1000ll / 3ll, 100ll);
    const auto sleep_duration = std::chrono::milliseconds(interval_ms);

    std::thread ping_thread([sleep_duration, failCb = std::move(failCb)] {
      uint8_t fail_count = 0;
      for (;;) {
        const auto now_tp = std::chrono::steady_clock::now();
        bool should_feed = g_watchdog_feed_requested.load(std::memory_order_acquire);
        if (!should_feed && within_grace_period(now_tp)) {
          should_feed = true;
        }

        if (!should_feed) {
          std::this_thread::sleep_for(sleep_duration);
          continue;
        }

        if (!PingDriver(SUDOVDA_DRIVER_HANDLE)) {
          fail_count += 1;
          if (fail_count > 3) {
            failCb();
            return;
          }
        } else {
          fail_count = 0;
        }

        std::this_thread::sleep_for(sleep_duration);
      }
    });

    ping_thread.detach();

    return true;
  }

  void setWatchdogFeedingEnabled(bool enable) {
    if (enable) {
      const auto deadline = std::chrono::steady_clock::now() + WATCHDOG_INIT_GRACE;
      g_watchdog_grace_deadline_ns.store(steady_ticks_from_time(deadline), std::memory_order_release);
    }
    g_watchdog_feed_requested.store(enable, std::memory_order_release);
  }

  bool setRenderAdapterByName(const std::wstring &adapterName) {
    if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
      return false;
    }

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (!SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
      return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc;
    int i = 0;
    while (SUCCEEDED(factory->EnumAdapters(i, &adapter))) {
      i += 1;

      if (!SUCCEEDED(adapter->GetDesc(&desc))) {
        continue;
      }

      if (std::wstring_view(desc.Description) != adapterName) {
        continue;
      }

      if (SetRenderAdapter(SUDOVDA_DRIVER_HANDLE, desc.AdapterLuid)) {
        return true;
      }
    }

    return false;
  }

  bool setRenderAdapterWithMostDedicatedMemory() {
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

    if (!SetRenderAdapter(SUDOVDA_DRIVER_HANDLE, best_luid)) {
      printf("[SUDOVDA] Failed to set render adapter with most dedicated memory.\n");
      return false;
    }

    const unsigned long long dedicated_mib = static_cast<unsigned long long>(best_dedicated / (1024ull * 1024ull));
    const unsigned long long shared_mib = static_cast<unsigned long long>(best_shared / (1024ull * 1024ull));
    wprintf(
      L"[SUDOVDA] Auto-selected render adapter: %ls (dedicated=%llu MiB, shared=%llu MiB)\n",
      best_name.c_str(),
      dedicated_mib,
      shared_mib
    );
    return true;
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
        }

        if (enumerated_at && now - *enumerated_at >= activation_grace) {
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

          const auto candidate_display_name = !candidate.m_display_name.empty()
                                                ? std::make_optional(normalize_display_name(candidate.m_display_name))
                                                : std::nullopt;
          const auto candidate_friendly_name = !candidate.m_friendly_name.empty()
                                                 ? std::make_optional(normalize_display_name(candidate.m_friendly_name))
                                                 : std::nullopt;

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
      bool framegen_refresh_active
  ) {
    if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
      return std::nullopt;
    }

    uuid_util::uuid_t requested_uuid {};
    std::memcpy(requested_uuid.b8, &guid, sizeof(requested_uuid.b8));

    // Log entry and inputs for deeper diagnostics
    BOOST_LOG(debug) << "createVirtualDisplay called: client_uid='" << (s_client_uid ? s_client_uid : "(null)")
                     << "' client_name='" << (s_client_name ? s_client_name : "(null)")
                     << "' hdr_profile='" << (s_hdr_profile ? s_hdr_profile : "(null)")
                     << "' width=" << width << " height=" << height << " fps=" << fps
                     << " guid=" << requested_uuid.string();

    teardown_conflicting_virtual_displays(requested_uuid);
    BOOST_LOG(debug) << "teardown_conflicting_virtual_displays completed for guid=" << requested_uuid.string();
    enforce_teardown_cooldown_if_needed();

    const uint32_t requested_fps = apply_refresh_overrides(fps, base_fps_millihz, framegen_refresh_active);
    VIRTUAL_DISPLAY_ADD_OUT output {};
    BOOST_LOG(debug) << "Calling AddVirtualDisplay (driver handle present).";
    if (!AddVirtualDisplay(SUDOVDA_DRIVER_HANDLE, width, height, requested_fps, guid, s_client_name, s_client_uid, output)) {
      const DWORD error_code = GetLastError();
      BOOST_LOG(warning) << "AddVirtualDisplay failed: error=" << error_code << " guid=" << requested_uuid.string();

      auto reuse_name = resolve_virtual_display_name_from_devices_for_client(s_client_name);
      if (!reuse_name) {
        reuse_name = resolve_virtual_display_name_from_devices();
      }
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

      if (auto dpi = read_virtual_display_dpi_value()) {
        (void) apply_virtual_display_dpi_value(*dpi);
      }

      if (reuse_name || device_id) {
        BOOST_LOG(debug) << "Waiting for virtual display ready (reuse). display_name='"
                         << (reuse_name ? platf::to_utf8(*reuse_name) : std::string("(none)"))
                         << "' device_id='" << (device_id ? *device_id : std::string("(none)")) << "'";
        std::optional<std::wstring> display_name = reuse_name;
        if (wait_for_virtual_display_ready(display_name, device_id, width, height)) {
          if (display_name) {
            wprintf(
              L"[SUDOVDA] Reusing existing virtual display (error=%lu): %ls\n",
              static_cast<unsigned long>(error_code),
              display_name->c_str()
            );
          } else {
            printf("[SUDOVDA] Reusing existing virtual display (error=%lu).\n", static_cast<unsigned long>(error_code));
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
            if (auto gdi_name = resolve_virtual_display_name_from_devices()) {
              if (!gdi_name->empty() && is_gdi_display_name(*gdi_name)) {
                BOOST_LOG(debug) << "Virtual display: resolved GDI name '" << platf::to_utf8(*gdi_name) << "' after reuse.";
                result.display_name = gdi_name;
              }
            }
          }

          result.monitor_device_path = resolve_monitor_device_path(display_name, result.device_id);
          result.reused_existing = true;
          result.ready_since = ready_since;
          std::optional<std::string> hdr_profile;
          if (s_hdr_profile && std::strlen(s_hdr_profile) > 0) {
            hdr_profile = std::string(s_hdr_profile);
          }
          apply_hdr_profile_if_available(result.display_name, result.device_id, result.monitor_device_path, result.client_name, hdr_profile);
          return result;
        }
      }

      printf("[SUDOVDA] Failed to add virtual display (error=%lu).\n", static_cast<unsigned long>(error_code));
      return std::nullopt;
    }

    const auto display_config_identity = query_display_config_identity(output);

    std::optional<std::wstring> resolved_display_name;
    if (display_config_identity) {
      if (display_config_identity->source_gdi_device_name && !display_config_identity->source_gdi_device_name->empty()) {
        resolved_display_name = *display_config_identity->source_gdi_device_name;
      } else if (
        display_config_identity->monitor_friendly_device_name && !display_config_identity->monitor_friendly_device_name->empty()
      ) {
        resolved_display_name = *display_config_identity->monitor_friendly_device_name;
      }
    }

    constexpr int kGetAddedDisplayNameAttempts = 3;
    constexpr DWORD kGetAddedDisplayNameDelayMs = 25;
    wchar_t device_name[CCHDEVICENAME] {};
    if (!resolved_display_name) {
      for (int attempt = 0; attempt < kGetAddedDisplayNameAttempts; ++attempt) {
        if (GetAddedDisplayName(output, device_name)) {
          resolved_display_name = device_name;
          break;
        }
        if (attempt + 1 < kGetAddedDisplayNameAttempts) {
          Sleep(kGetAddedDisplayNameDelayMs);
        }
      }
    }

    if (!resolved_display_name && display_config_identity && display_config_identity->monitor_device_path &&
        !display_config_identity->monitor_device_path->empty()) {
      resolved_display_name = *display_config_identity->monitor_device_path;
    }

    if (!resolved_display_name) {
      resolved_display_name = resolve_virtual_display_name_from_devices_for_client(s_client_name);
      if (!resolved_display_name) {
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

    const auto display_config_ptr = display_config_identity ? &*display_config_identity : nullptr;

    if (!wait_for_virtual_display_ready(resolved_display_name, device_id, width, height, display_config_ptr)) {
      printf("[SUDOVDA] Timed out waiting for Windows to enumerate the new virtual display; reverting creation.\n");
      (void) removeVirtualDisplay(guid);
      return std::nullopt;
    }

    // Prefer a real GDI display name (\\.\DISPLAYx) over GUID placeholders once enumeration is complete.
    if (resolved_display_name && !resolved_display_name->empty() && !is_gdi_display_name(*resolved_display_name)) {
      auto gdi_name = resolve_virtual_display_name_from_devices_for_client(s_client_name);
      if (!gdi_name) {
        gdi_name = resolve_virtual_display_name_from_devices();
      }
      if (gdi_name && !gdi_name->empty() && is_gdi_display_name(*gdi_name)) {
        BOOST_LOG(debug) << "Virtual display: resolved GDI name '" << platf::to_utf8(*gdi_name) << "' after creation.";
        resolved_display_name = gdi_name;
      }
    }

    if (resolved_display_name) {
      wprintf(L"[SUDOVDA] Virtual display added successfully: %ls\n", resolved_display_name->c_str());
    } else {
      wprintf(L"[SUDOVDA] Virtual display added; device name pending enumeration (target=%u).\n", output.TargetId);
    }
    printf("[SUDOVDA] Configuration: W: %d, H: %d, FPS: %d\n", width, height, requested_fps);

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
    apply_hdr_profile_if_available(result.display_name, result.device_id, result.monitor_device_path, result.client_name, hdr_profile);
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
    bool framegen_refresh_active
  ) {
    constexpr int kMaxInitializationAttempts = 3;
    const auto requested_uuid = guid_to_uuid(guid);

    for (int attempt = 1; attempt <= kMaxInitializationAttempts; ++attempt) {
      if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
        if (openVDisplayDevice() != DRIVER_STATUS::OK) {
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
        framegen_refresh_active
      );
      if (!result) {
        BOOST_LOG(warning) << "Virtual display creation attempt " << attempt << '/' << kMaxInitializationAttempts
                           << " failed.";

        if (attempt == kMaxInitializationAttempts) {
          BOOST_LOG(error) << "Virtual display could not be created after " << kMaxInitializationAttempts << " attempts.";
          return std::nullopt;
        }

        closeVDisplayDevice();

        if (!ensure_driver_is_ready()) {
          BOOST_LOG(warning) << "Driver recovery failed after virtual display creation failure.";
          return std::nullopt;
        }

        if (openVDisplayDevice() != DRIVER_STATUS::OK) {
          BOOST_LOG(warning) << "Failed to re-open SudoVDA driver after recovery.";
          return std::nullopt;
        }

        BOOST_LOG(info) << "Retrying SudoVDA virtual display initialization (attempt "
                        << (attempt + 1) << '/' << kMaxInitializationAttempts << ").";
        continue;
      }

      if (confirm_virtual_display_persistence(*result, width, height)) {
        write_guid_to_state_locked(requested_uuid);
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

      if (!ensure_driver_is_ready()) {
        BOOST_LOG(warning) << "Driver recovery failed after virtual display vanished.";
        return std::nullopt;
      }

      if (openVDisplayDevice() != DRIVER_STATUS::OK) {
        BOOST_LOG(warning) << "Failed to re-open SudoVDA driver after recovery.";
        return std::nullopt;
      }

      BOOST_LOG(info) << "Retrying SudoVDA virtual display initialization (attempt "
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
      if (!VDISPLAY::removeVirtualDisplay(native_guid)) {
        all_removed = false;
      }
    }

    if(all_removed){
      BOOST_LOG(info) << "Virtual display devices have been removed successfully.";
    }
    else {
      BOOST_LOG(warning) << "Virtual display devices failed to be removed.";
    }

   return all_removed;
  }

  bool removeVirtualDisplay(const GUID &guid) {
    abort_recovery_monitor(guid_to_uuid(guid));
    auto cached_display_name = resolve_virtual_display_name_from_devices();

    const bool initial_handle_invalid = (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE);
    bool opened_handle = false;

    auto ensure_handle = [&]() -> bool {
      if (SUDOVDA_DRIVER_HANDLE != INVALID_HANDLE_VALUE) {
        return true;
      }
      if (openVDisplayDevice() != DRIVER_STATUS::OK) {
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
      if (openVDisplayDevice() == DRIVER_STATUS::OK) {
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

    printf("[SUDOVDA] Failed to remove virtual display (error=%lu).\n", static_cast<unsigned long>(error_code));
    return false;
  }

  bool isSudaVDADriverInstalled() {
    if (driver_handle_responsive(SUDOVDA_DRIVER_HANDLE)) {
      return true;
    }

    return ensure_driver_is_ready();
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

    std::optional<std::string> fallback;
    std::optional<std::string> active_fallback;
    for (const auto &device : *devices) {
      if (is_virtual_display_device(device) && !device.m_device_id.empty()) {
        if (!fallback) {
          fallback = device.m_device_id;
        }
        if (!active_fallback && device.m_info) {
          active_fallback = device.m_device_id;
        }
      }

      const auto device_name = normalize_display_name(device.m_display_name);
      if (!device_name.empty() && device_name == target && !device.m_device_id.empty()) {
        return device.m_device_id;
      }
    }

    if (active_fallback) {
      return active_fallback;
    }
    if (fallback) {
      return fallback;
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
      bool assumed_active = device.m_info.has_value();
      if (!assumed_active) {
        if (!device.m_display_name.empty() || !device.m_device_id.empty()) {
          assumed_active = true;
        }
      }
      info.is_active = assumed_active;
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
}  // namespace VDISPLAY

bool VDISPLAY::has_active_physical_display() {
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

bool VDISPLAY::should_auto_enable_virtual_display() {
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

uuid_util::uuid_t VDISPLAY::persistentVirtualDisplayUuid() {
  return ensure_persistent_guid();
}

VDISPLAY::ensure_display_result VDISPLAY::ensure_display() {
  ensure_display_result result {false, false, {}};

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

  auto virtual_displays = enumerateSudaVDADisplays();
  bool has_active_virtual = std::any_of(
    virtual_displays.begin(),
    virtual_displays.end(),
    [](const SudaVDADisplayInfo &info) {
      return info.is_active;
    }
  );

  if (has_active_virtual) {
    BOOST_LOG(debug) << "Active virtual display already exists.";
    result.success = true;
    return result;
  }

  auto uuid = persistentVirtualDisplayUuid();
  std::memcpy(&result.temporary_guid, uuid.b8, sizeof(result.temporary_guid));

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
    false
  );
  if (!display_info) {
    BOOST_LOG(warning) << "Failed to create temporary virtual display.";
    return result;
  }

  result.created_temporary = true;
  result.success = true;
  BOOST_LOG(info) << "Temporary virtual display ready.";
  return result;
}

void VDISPLAY::cleanup_ensure_display(const ensure_display_result &result) {
  if (result.created_temporary) {
    if (!removeVirtualDisplay(result.temporary_guid)) {
      BOOST_LOG(warning) << "Failed to remove temporary virtual display.";
    } else {
      BOOST_LOG(info) << "Removed temporary virtual display.";
    }
  }
}
