#include "virtual_display.h"

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
#include <initguid.h>
#include <iostream>
#include <limits>
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

namespace fs = std::filesystem;

using namespace SUDOVDA;

namespace VDISPLAY {
  namespace {
    constexpr auto WATCHDOG_INIT_GRACE = std::chrono::seconds(30);
    constexpr auto DRIVER_RESTART_TIMEOUT = std::chrono::seconds(15);
    constexpr auto DRIVER_RESTART_POLL_INTERVAL = std::chrono::milliseconds(500);
    constexpr auto DEVICE_RESTART_SETTLE_DELAY = std::chrono::milliseconds(200);
    constexpr std::wstring_view SUDOVDA_HARDWARE_ID = L"root\\sudomaker\\sudovda";
    constexpr std::wstring_view SUDOVDA_FRIENDLY_NAME_W = L"SudoMaker Virtual Display Adapter";

    std::atomic<bool> g_watchdog_feed_requested {false};
    std::atomic<std::int64_t> g_watchdog_grace_deadline_ns {0};

    std::int64_t steady_ticks_from_time(std::chrono::steady_clock::time_point tp) {
      return std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
    }

    std::chrono::steady_clock::time_point time_from_steady_ticks(std::int64_t ticks) {
      return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(ticks));
    }

    bool within_grace_period(std::chrono::steady_clock::time_point now) {
      auto deadline_ticks = g_watchdog_grace_deadline_ns.load(std::memory_order_acquire);
      if (deadline_ticks <= 0) {
        return false;
      }
      return now < time_from_steady_ticks(deadline_ticks);
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

    struct VirtualDisplayCache {
      struct entry_t {
        std::wstring display_name;
        std::optional<uint32_t> dpi_value;
      };

      std::mutex mutex;
      bool loaded = false;
      std::optional<entry_t> entry;

      static VirtualDisplayCache &instance() {
        static VirtualDisplayCache cache;
        return cache;
      }

      static fs::path storage_path() {
        return platf::appdata() / "virtual_display_cache.json";
      }

      void ensure_loaded_locked() {
        if (loaded) {
          return;
        }
        loaded = true;
        entry.reset();

        const auto path = storage_path();
        try {
          if (!fs::exists(path)) {
            return;
          }
          std::ifstream stream(path, std::ios::binary);
          if (!stream) {
            return;
          }
          nlohmann::json json = nlohmann::json::parse(stream, nullptr, false);
          if (!json.is_object()) {
            return;
          }

          entry_t tmp;
          if (auto disp = json.find("display_name"); disp != json.end() && disp->is_string()) {
            tmp.display_name = platf::from_utf8(disp->get<std::string>());
          }
          if (auto dpi = json.find("dpi_value"); dpi != json.end() && dpi->is_number_unsigned()) {
            tmp.dpi_value = static_cast<uint32_t>(dpi->get<uint64_t>());
          }

          if (!tmp.display_name.empty() || tmp.dpi_value) {
            entry = std::move(tmp);
          }
        } catch (...) {
          entry.reset();
        }
      }

      void save_locked() {
        const auto path = storage_path();
        try {
          if (!entry || (entry->display_name.empty() && !entry->dpi_value)) {
            entry.reset();
            std::error_code ec;
            fs::remove(path, ec);
            return;
          }

          nlohmann::json json = nlohmann::json::object();
          if (!entry->display_name.empty()) {
            json["display_name"] = platf::to_utf8(entry->display_name);
          }
          if (entry->dpi_value) {
            json["dpi_value"] = *entry->dpi_value;
          }

          fs::create_directories(path.parent_path());
          std::ofstream stream(path, std::ios::binary | std::ios::trunc);
          if (!stream) {
            return;
          }
          stream << json.dump(2);
        } catch (...) {
        }
      }

      std::optional<entry_t> get_entry() {
        std::lock_guard<std::mutex> lg(mutex);
        ensure_loaded_locked();
        return entry;
      }

      void set_entry(entry_t value) {
        std::lock_guard<std::mutex> lg(mutex);
        ensure_loaded_locked();
        entry = std::move(value);
        save_locked();
      }

      void update_dpi(const std::optional<uint32_t> &value) {
        std::lock_guard<std::mutex> lg(mutex);
        ensure_loaded_locked();
        if (!entry || !value) {
          return;
        }
        entry->dpi_value = *value;
        save_locked();
      }

      void clear_display_name() {
        std::lock_guard<std::mutex> lg(mutex);
        ensure_loaded_locked();
        if (!entry) {
          return;
        }
        if (!entry->display_name.empty()) {
          if (!entry->dpi_value) {
            entry.reset();
          } else {
            entry->display_name.clear();
          }
        } else if (!entry->dpi_value) {
          entry.reset();
        }
        save_locked();
      }
    };

    namespace pt = boost::property_tree;

    bool is_virtual_display_device(
      const display_device::EnumeratedDevice &device,
      const std::optional<VirtualDisplayCache::entry_t> &cached_entry
    ) {
      static const std::string sudoMakerDeviceString = "SudoMaker Virtual Display Adapter";
      if (equals_ci(device.m_friendly_name, sudoMakerDeviceString)) {
        return true;
      }

      if (device.m_edid) {
        static const std::string manufacturer = "SMK";
        static const std::string product = "D1CE";
        if (equals_ci(device.m_edid->m_manufacturer_id, manufacturer) && equals_ci(device.m_edid->m_product_code, product)) {
          return true;
        }
      }

      return false;
    }

    bool luid_equals(const LUID &lhs, const LUID &rhs) {
      return lhs.LowPart == rhs.LowPart && lhs.HighPart == rhs.HighPart;
    }

    std::optional<std::wstring> resolve_display_name_via_display_config(const VIRTUAL_DISPLAY_ADD_OUT &output) {
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

        DISPLAYCONFIG_SOURCE_DEVICE_NAME source_name {};
        source_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source_name.header.size = sizeof(source_name);
        source_name.header.adapterId = path.sourceInfo.adapterId;
        source_name.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source_name.header) == ERROR_SUCCESS && source_name.viewGdiDeviceName[0] != L'\0') {
          return std::wstring(source_name.viewGdiDeviceName);
        }

        DISPLAYCONFIG_TARGET_DEVICE_NAME target_name {};
        target_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target_name.header.size = sizeof(target_name);
        target_name.header.adapterId = path.targetInfo.adapterId;
        target_name.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target_name.header) == ERROR_SUCCESS) {
          if (target_name.monitorFriendlyDeviceName[0] != L'\0') {
            return std::wstring(target_name.monitorFriendlyDeviceName);
          }
          if (target_name.monitorDevicePath[0] != L'\0') {
            return std::wstring(target_name.monitorDevicePath);
          }
        }
      }

      return std::nullopt;
    }

    std::optional<std::wstring> resolve_virtual_display_name_from_devices(const std::optional<VirtualDisplayCache::entry_t> &cached_entry) {
      auto devices = platf::display_helper::Coordinator::instance().enumerate_devices();
      if (!devices) {
        return std::nullopt;
      }
      for (const auto &device : *devices) {
        if (!is_virtual_display_device(device, cached_entry)) {
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
      const auto path = VirtualDisplayCache::storage_path();
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
  }  // namespace

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

    auto instance_id = find_sudovda_device_instance_id();
    if (!instance_id) {
      BOOST_LOG(warning) << "Unable to locate SudoVDA adapter for recovery.";
      return false;
    }

    BOOST_LOG(info) << "Attempting to restart SudoVDA adapter " << platf::to_utf8(*instance_id) << '.';

    if (!restart_sudovda_device(*instance_id)) {
      BOOST_LOG(warning) << "SudoVDA adapter restart failed.";
      return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + DRIVER_RESTART_TIMEOUT;
    while (std::chrono::steady_clock::now() < deadline) {
      if (probe_driver_responsive_once()) {
        BOOST_LOG(info) << "SudoVDA driver responded after restart.";
        return true;
      }
      std::this_thread::sleep_for(DRIVER_RESTART_POLL_INTERVAL);
    }

    BOOST_LOG(warning) << "SudoVDA driver did not respond within the restart timeout.";
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
    const std::optional<VirtualDisplayCache::entry_t> &match_hint,
    uint32_t width,
    uint32_t height
  ) {
    auto hint = match_hint;
    if (!hint) {
      hint = VirtualDisplayCache::instance().get_entry();
    }

    std::optional<std::string> normalized_name;
    if (display_name && !display_name->empty()) {
      normalized_name = normalize_display_name(platf::to_utf8(*display_name));
    }

    const auto start = std::chrono::steady_clock::now();
    std::optional<std::chrono::steady_clock::time_point> enumerated_at;
    const auto enumeration_timeout = std::chrono::seconds(5);
    const auto activation_grace = std::chrono::seconds(2);

    while (true) {
      const auto now = std::chrono::steady_clock::now();
      if (!enumerated_at && now - start >= enumeration_timeout) {
        if (hint && !hint->display_name.empty()) {
          BOOST_LOG(warning) << "Timed out waiting for Windows to enumerate cached virtual display '"
                             << platf::to_utf8(hint->display_name)
                             << "'; clearing cached display name.";
          VirtualDisplayCache::instance().clear_display_name();
        }
        return false;
      }
      if (enumerated_at && now - *enumerated_at >= activation_grace) {
        return true;
      }

      auto devices = platf::display_helper::Coordinator::instance().enumerate_devices();
      if (devices) {
        for (const auto &candidate : *devices) {
          if (!is_virtual_display_device(candidate, hint)) {
            continue;
          }

          bool matches = false;
          if (device_id && !device_id->empty() && !candidate.m_device_id.empty()) {
            matches = equals_ci(candidate.m_device_id, *device_id);
          }

          if (!matches && normalized_name) {
            if (!candidate.m_display_name.empty() &&
                normalize_display_name(candidate.m_display_name) == *normalized_name) {
              matches = true;
            } else if (!candidate.m_friendly_name.empty() &&
                       normalize_display_name(candidate.m_friendly_name) == *normalized_name) {
              matches = true;
            }
          }

          if (!matches && !device_id && !normalized_name) {
            matches = true;
          }

          if (!matches) {
            continue;
          }

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

            if (now - *enumerated_at >= std::chrono::milliseconds(500)) {
              return true;
            }
          } else {
            if (now - *enumerated_at >= std::chrono::milliseconds(500)) {
              return true;
            }
          }
        }
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
      if (auto devices = platf::display_helper::Coordinator::instance().enumerate_devices()) {
        for (const auto &device : *devices) {
          if (!is_virtual_display_device(device, std::nullopt)) {
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

  std::optional<VirtualDisplayCreationResult> createVirtualDisplay(
    const char *s_client_uid,
    const char *s_client_name,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    const GUID &guid
  ) {
    if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
      return std::nullopt;
    }

    uuid_util::uuid_t requested_uuid {};
    std::memcpy(requested_uuid.b8, &guid, sizeof(requested_uuid.b8));

    // Log entry and inputs for deeper diagnostics
    BOOST_LOG(debug) << "createVirtualDisplay called: client_uid='" << (s_client_uid ? s_client_uid : "(null)")
                     << "' client_name='" << (s_client_name ? s_client_name : "(null)")
                     << "' width=" << width << " height=" << height << " fps=" << fps
                     << " guid=" << requested_uuid.string();

    teardown_conflicting_virtual_displays(requested_uuid);
    BOOST_LOG(debug) << "teardown_conflicting_virtual_displays completed for guid=" << requested_uuid.string();

    auto cached_entry = VirtualDisplayCache::instance().get_entry();
    if (cached_entry) {
      BOOST_LOG(debug) << "Cached entry present: display_name='" << platf::to_utf8(cached_entry->display_name)
                       << "' dpi='"
                       << (cached_entry->dpi_value ? std::to_string(*cached_entry->dpi_value) : std::string("(none)")) << "'";
    } else {
      BOOST_LOG(debug) << "No cached virtual display entry present.";
    }

    VIRTUAL_DISPLAY_ADD_OUT output {};
    BOOST_LOG(debug) << "Calling AddVirtualDisplay (driver handle present).";
    if (!AddVirtualDisplay(SUDOVDA_DRIVER_HANDLE, width, height, fps, guid, s_client_name, s_client_uid, output)) {
      const DWORD error_code = GetLastError();
      BOOST_LOG(warning) << "AddVirtualDisplay failed: error=" << error_code << " guid=" << requested_uuid.string();

      // If we have a cache entry, attempt to reuse it and wait for Windows to enumerate the device
      if (cached_entry && !cached_entry->display_name.empty()) {
        BOOST_LOG(debug) << "Cached display name available; attempting reuse path: '" << platf::to_utf8(cached_entry->display_name) << "'";
        if (cached_entry->dpi_value) {
          BOOST_LOG(debug) << "Applying cached DPI (before reuse)=" << *cached_entry->dpi_value;
          (void) apply_virtual_display_dpi_value(*cached_entry->dpi_value);
        }

        auto reuse_name = cached_entry->display_name;
        auto resolved_id = resolveVirtualDisplayDeviceId(reuse_name);
        BOOST_LOG(debug) << "resolveVirtualDisplayDeviceId(" << platf::to_utf8(reuse_name) << ") returned '"
                         << (resolved_id ? *resolved_id : std::string("(none)")) << "'";

        std::optional<VirtualDisplayCache::entry_t> cache_entry;
        cache_entry.emplace();
        cache_entry->display_name = reuse_name;
        if (auto dpi = read_virtual_display_dpi_value()) {
          cache_entry->dpi_value = *dpi;
          BOOST_LOG(debug) << "Read DPI from registry during reuse: " << *dpi;
        } else {
          cache_entry->dpi_value = cached_entry->dpi_value;
          BOOST_LOG(debug) << "No DPI read from registry; using cached DPI: "
                           << (cached_entry->dpi_value ? std::to_string(*cached_entry->dpi_value) : std::string("(none)"));
        }

        std::optional<std::wstring> display_name {cache_entry->display_name};
        std::optional<std::string> device_id;
        if (resolved_id && !resolved_id->empty()) {
          device_id = *resolved_id;
        }

        BOOST_LOG(debug) << "Waiting for virtual display ready (reuse). display_name='" << platf::to_utf8(display_name.value())
                         << "' device_id='" << (device_id ? *device_id : std::string("(none)")) << "'";
        if (!wait_for_virtual_display_ready(display_name, device_id, cache_entry, width, height)) {
          BOOST_LOG(warning) << "Timed out waiting for existing virtual display to be acknowledged by Windows (reuse path).";
          printf("[SUDOVDA] Timed out waiting for existing virtual display to be acknowledged by Windows.\n");
          VirtualDisplayCache::instance().clear_display_name();
          return std::nullopt;
        }

        // Persist display_name and dpi to cache (device_id is ephemeral and not cached)
        if (cache_entry) {
          if (!cache_entry->display_name.empty() || cache_entry->dpi_value) {
            VirtualDisplayCache::instance().set_entry(*cache_entry);
            BOOST_LOG(debug) << "Updated virtual display cache after reuse: display_name='" << platf::to_utf8(cache_entry->display_name)
                             << "' dpi='"
                             << (cache_entry->dpi_value ? std::to_string(*cache_entry->dpi_value) : std::string("(none)")) << "'";
          }
        }

        write_guid_to_state_locked(requested_uuid);

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
                        << " display_name='" << platf::to_utf8(display_name.value()) << "' device_id='"
                        << (device_id ? *device_id : std::string("(none)")) << "'";

        track_virtual_display_created(requested_uuid);
        VirtualDisplayCreationResult result;
        result.display_name = display_name;
        if (device_id) {
          result.device_id = *device_id;
        }
        result.reused_existing = true;
        return result;
      }

      // No cached entry to reuse; report failure
      printf("[SUDOVDA] Failed to add virtual display (error=%lu).\n", static_cast<unsigned long>(error_code));
      BOOST_LOG(warning) << "Failed to add virtual display and no cached entry available. error=" << error_code;
      return std::nullopt;
    }

    if (cached_entry && cached_entry->dpi_value) {
      (void) apply_virtual_display_dpi_value(*cached_entry->dpi_value);
    }

    std::optional<std::wstring> resolved_display_name;
    uint32_t retry_interval = 20;
    wchar_t device_name[CCHDEVICENAME] {};
    for (int attempt = 0; attempt < 10; ++attempt) {
      if (GetAddedDisplayName(output, device_name)) {
        resolved_display_name = device_name;
        break;
      }
      if (auto via_config = resolve_display_name_via_display_config(output)) {
        resolved_display_name = via_config;
        break;
      }
      Sleep(retry_interval);
      if (retry_interval < 640) {
        retry_interval *= 2;
      }
    }

    if (!resolved_display_name) {
      resolved_display_name = resolve_virtual_display_name_from_devices(cached_entry);
    }

    if (!resolved_display_name && cached_entry && !cached_entry->display_name.empty()) {
      resolved_display_name = cached_entry->display_name;
    }

    std::optional<std::string> device_id;
    if (resolved_display_name) {
      device_id = resolveVirtualDisplayDeviceId(*resolved_display_name);
      BOOST_LOG(debug) << "resolveVirtualDisplayDeviceId(" << platf::to_utf8(*resolved_display_name) << ") returned '"
                       << (device_id ? *device_id : std::string("(none)")) << "'";
    }
    if (!device_id) {
      device_id = resolveAnyVirtualDisplayDeviceId();
    }

    std::optional<VirtualDisplayCache::entry_t> cache_entry;
    cache_entry.emplace();
    if (resolved_display_name) {
      cache_entry->display_name = *resolved_display_name;
    } else if (cached_entry && !cached_entry->display_name.empty()) {
      cache_entry->display_name = cached_entry->display_name;
    }
    if (auto dpi = read_virtual_display_dpi_value()) {
      cache_entry->dpi_value = *dpi;
    } else if (cached_entry && cached_entry->dpi_value) {
      cache_entry->dpi_value = cached_entry->dpi_value;
    }

    if (!wait_for_virtual_display_ready(resolved_display_name, device_id, cache_entry, width, height)) {
      printf("[SUDOVDA] Timed out waiting for Windows to enumerate the new virtual display; reverting creation.\n");
      (void) removeVirtualDisplay(guid);
      VirtualDisplayCache::instance().clear_display_name();
      return std::nullopt;
    }

    // Persist display_name and dpi to cache (device_id is ephemeral and not cached)
    if (cache_entry) {
      if (!cache_entry->display_name.empty() || cache_entry->dpi_value) {
        VirtualDisplayCache::instance().set_entry(*cache_entry);
      }
    }

    write_guid_to_state_locked(requested_uuid);
    track_virtual_display_created(requested_uuid);

    if (resolved_display_name) {
      wprintf(L"[SUDOVDA] Virtual display added successfully: %ls\n", resolved_display_name->c_str());
    } else {
      wprintf(L"[SUDOVDA] Virtual display added; device name pending enumeration (target=%u).\n", output.TargetId);
    }
    printf("[SUDOVDA] Configuration: W: %d, H: %d, FPS: %d\n", width, height, fps);

    VirtualDisplayCreationResult result;
    result.display_name = resolved_display_name;
    if (device_id && !device_id->empty()) {
      result.device_id = *device_id;
    }
    result.reused_existing = false;
    return result;
  }

  bool removeAllVirtualDisplays() {
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
    auto current_dpi = read_virtual_display_dpi_value();
    if (current_dpi) {
      VirtualDisplayCache::instance().update_dpi(current_dpi);
    }

    auto cached_entry = VirtualDisplayCache::instance().get_entry();
    std::optional<std::wstring> cached_display_name;
    if (cached_entry && !cached_entry->display_name.empty()) {
      cached_display_name = cached_entry->display_name;
    }

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
      } else if (error_code == ERROR_FILE_NOT_FOUND || error_code == ERROR_INVALID_PARAMETER) {
        track_virtual_display_removed(guid_to_uuid(guid));
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
      VirtualDisplayCache::instance().clear_display_name();
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
    auto cached_entry = VirtualDisplayCache::instance().get_entry();

    if (display_name.empty()) {
      return resolveAnyVirtualDisplayDeviceId();
    }

    auto devices = platf::display_helper::Coordinator::instance().enumerate_devices();
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
      if (is_virtual_display_device(device, cached_entry) && !device.m_device_id.empty()) {
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

  std::optional<std::string> resolveAnyVirtualDisplayDeviceId() {
    auto cached_entry = VirtualDisplayCache::instance().get_entry();
    auto devices = platf::display_helper::Coordinator::instance().enumerate_devices();
    std::optional<std::string> active_match;
    std::optional<std::string> any_match;

    if (devices) {
      for (const auto &device : *devices) {
        if (!is_virtual_display_device(device, cached_entry) || device.m_device_id.empty()) {
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

    const auto devices = platf::display_helper::Coordinator::instance().enumerate_devices();
    if (!devices) {
      return result;
    }

    auto cached_entry = VirtualDisplayCache::instance().get_entry();
    for (const auto &device : *devices) {
      if (!is_virtual_display_device(device, cached_entry)) {
        continue;
      }

      SudaVDADisplayInfo info;
      info.device_name = !device.m_display_name.empty() ? platf::from_utf8(device.m_display_name) : platf::from_utf8(device.m_device_id.empty() ? device.m_friendly_name : device.m_device_id);
      info.friendly_name = !device.m_friendly_name.empty() ? platf::from_utf8(device.m_friendly_name) : info.device_name;
      bool assumed_active = device.m_info.has_value();
      if (!assumed_active && is_virtual_display_device(device, cached_entry)) {
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

bool VDISPLAY::has_active_physical_display(const std::optional<bool> &active) {
  auto devices = platf::display_helper::Coordinator::instance().enumerate_devices();
  BOOST_LOG(debug) << "Enumerated devices count: " << (devices ? devices->size() : 0);
  if (!devices) {
    BOOST_LOG(debug) << "No devices enumerated, returning true";
    return true;
  }

  auto cached_entry = VirtualDisplayCache::instance().get_entry();
  BOOST_LOG(debug) << "Cached entry present: " << cached_entry.has_value();
  bool found_physical = false;
  for (const auto &device : *devices) {
    bool has_info = device.m_info.has_value();
    bool is_virtual = is_virtual_display_device(device, cached_entry);
    BOOST_LOG(debug) << "Device: " << device.m_display_name << ", has_info: " << has_info << ", is_virtual: " << is_virtual;
    if (!is_virtual) {
      found_physical = true;
      if (active.has_value()) {
        if (has_info == *active) {
          BOOST_LOG(debug) << "Found physical display matching active state, returning true";
          return true;
        }
      } else if (has_info) {
        BOOST_LOG(debug) << "Found active physical display, returning true";
        return true;
      }
    }
  }

  if (found_physical && !active.has_value()) {
    BOOST_LOG(debug) << "Found physical display in enumeration (inactive), returning true";
    return true;
  }

  BOOST_LOG(debug) << "No physical display matching criteria found, returning false";
  return false;
}

bool VDISPLAY::should_auto_enable_virtual_display() {
  if (!isSudaVDADriverInstalled()) {
    BOOST_LOG(warning) << "Suda VDA driver not installed, not enabling virtual display.";
    return false;
  }

  if (has_active_physical_display(true)) {
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

  if (has_active_physical_display(true)) {
    result.success = true;
    return result;
  }

  if (!should_auto_enable_virtual_display()) {
    BOOST_LOG(debug) << "No active physical displays and virtual display auto-enable is disabled.";
    return result;
  }

  if (proc::vDisplayDriverStatus != DRIVER_STATUS::OK) {
    proc::initVDisplayDriver();
  }

  if (proc::vDisplayDriverStatus != DRIVER_STATUS::OK) {
    BOOST_LOG(warning) << "Virtual display driver unavailable for display ensure. Status=" << static_cast<int>(proc::vDisplayDriverStatus);
    return result;
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
  auto display_info = createVirtualDisplay("sunshine-ensure", "Sunshine Temporary", 1920u, 1080u, 60000u, result.temporary_guid);
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
