#include "virtual_display.h"

#include "src/display_helper_integration.h"
#include "src/state_storage.h"
#include "src/platform/common.h"
#include "src/platform/windows/misc.h"
#include "src/uuid.h"

#include <cctype>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <combaseapi.h>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <dxgi.h>
#include <dxgi1_6.h>
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
#include <setupapi.h>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <windows.h>
#include <winreg.h>
#include <wrl/client.h>

namespace fs = std::filesystem;

using namespace SUDOVDA;

namespace VDISPLAY {
  namespace {
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
        std::string device_id;
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
          if (auto devid = json.find("device_id"); devid != json.end() && devid->is_string()) {
            tmp.device_id = devid->get<std::string>();
          }
          if (auto dpi = json.find("dpi_value"); dpi != json.end() && dpi->is_number_unsigned()) {
            tmp.dpi_value = static_cast<uint32_t>(dpi->get<uint64_t>());
          }

          if (!tmp.display_name.empty() || !tmp.device_id.empty()) {
            entry = std::move(tmp);
          }
        } catch (...) {
          entry.reset();
        }
      }

      void save_locked() {
        const auto path = storage_path();
        try {
          if (!entry) {
            std::error_code ec;
            fs::remove(path, ec);
            return;
          }

          nlohmann::json json = nlohmann::json::object();
          if (!entry->display_name.empty()) {
            json["display_name"] = platf::to_utf8(entry->display_name);
          }
          if (!entry->device_id.empty()) {
            json["device_id"] = entry->device_id;
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
    };

    namespace pt = boost::property_tree;

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
      const auto &path_str = statefile::vibeshine_state_path();
      if (path_str.empty()) {
        return;
      }

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
      return;
    }

    CloseHandle(SUDOVDA_DRIVER_HANDLE);

    SUDOVDA_DRIVER_HANDLE = INVALID_HANDLE_VALUE;
  }

  DRIVER_STATUS openVDisplayDevice() {
    uint32_t retryInterval = 20;
    while (true) {
      SUDOVDA_DRIVER_HANDLE = OpenDevice(&SUVDA_INTERFACE_GUID);
      if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
        if (retryInterval > 320) {
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

    if (watchdogOut.Timeout) {
      auto sleepInterval = watchdogOut.Timeout * 1000 / 3;
      std::thread ping_thread([sleepInterval, failCb = std::move(failCb)] {
        uint8_t fail_count = 0;
        for (;;) {
          if (!sleepInterval) {
            return;
          }
          if (!PingDriver(SUDOVDA_DRIVER_HANDLE)) {
            fail_count += 1;
            if (fail_count > 3) {
              failCb();
              return;
            }
          };
          Sleep(sleepInterval);
        }
      });

      ping_thread.detach();
    }

    return true;
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

  std::wstring createVirtualDisplay(
    const char *s_client_uid,
    const char *s_client_name,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    const GUID &guid
  ) {
    if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
      return std::wstring();
    }

    auto cached_entry = VirtualDisplayCache::instance().get_entry();
    VIRTUAL_DISPLAY_ADD_OUT output;
    if (!AddVirtualDisplay(SUDOVDA_DRIVER_HANDLE, width, height, fps, guid, s_client_name, s_client_uid, output)) {
      const DWORD error_code = GetLastError();
      if (cached_entry && !cached_entry->display_name.empty()) {
        if (cached_entry->dpi_value) {
          (void) apply_virtual_display_dpi_value(*cached_entry->dpi_value);
        }
        auto reuse_name = cached_entry->display_name;
        auto device_id = resolveVirtualDisplayDeviceId(reuse_name);

        VirtualDisplayCache::entry_t updated;
        updated.display_name = std::move(reuse_name);
        if (device_id && !device_id->empty()) {
          updated.device_id = *device_id;
        }
        if (auto dpi = read_virtual_display_dpi_value()) {
          updated.dpi_value = *dpi;
        } else {
          updated.dpi_value = cached_entry->dpi_value;
        }
        VirtualDisplayCache::instance().set_entry(std::move(updated));

        wprintf(
          L"[SUDOVDA] Reusing existing virtual display (error=%lu): %ls\n",
          static_cast<unsigned long>(error_code),
          cached_entry->display_name.c_str()
        );
        return cached_entry->display_name;
      }
      printf("[SUDOVDA] Failed to add virtual display (error=%lu).\n", static_cast<unsigned long>(error_code));
      return std::wstring();
    }

    uint32_t retryInterval = 20;
    wchar_t deviceName[CCHDEVICENAME] {};
    while (!GetAddedDisplayName(output, deviceName)) {
      Sleep(retryInterval);
      if (retryInterval > 320) {
        printf("[SUDOVDA] Cannot get name for newly added virtual display!\n");
        return std::wstring();
      }
      retryInterval *= 2;
    }

    wprintf(L"[SUDOVDA] Virtual display added successfully: %ls\n", deviceName);
    printf("[SUDOVDA] Configuration: W: %d, H: %d, FPS: %d\n", width, height, fps);

    if (cached_entry && cached_entry->dpi_value) {
      (void) apply_virtual_display_dpi_value(*cached_entry->dpi_value);
    }

    std::optional<std::string> device_id;
    if (auto resolved = resolveVirtualDisplayDeviceId(deviceName)) {
      device_id = *resolved;
    }

    VirtualDisplayCache::entry_t cache_entry;
    cache_entry.display_name = deviceName;
    if (device_id && !device_id->empty()) {
      cache_entry.device_id = *device_id;
    }
    if (auto dpi = read_virtual_display_dpi_value()) {
      cache_entry.dpi_value = *dpi;
    } else if (cached_entry && cached_entry->dpi_value) {
      cache_entry.dpi_value = cached_entry->dpi_value;
    }
    VirtualDisplayCache::instance().set_entry(std::move(cache_entry));

    return std::wstring(deviceName);
  }

  bool removeVirtualDisplay(const GUID &guid) {
    if (!has_active_physical_display()) {
      printf("[SUDOVDA] No physical displays detected; keeping virtual display active.\n");
      return true;
    }

    if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
      return false;
    }

    if (RemoveVirtualDisplay(SUDOVDA_DRIVER_HANDLE, guid)) {
      printf("[SUDOVDA] Virtual display removed successfully.\n");
      return true;
    }

    printf("[SUDOVDA] Failed to remove virtual display (error=%lu).\n", static_cast<unsigned long>(GetLastError()));
    return false;
  }

  bool isSudaVDADriverInstalled() {
    if (SUDOVDA_DRIVER_HANDLE != INVALID_HANDLE_VALUE) {
      return true;
    }
    HANDLE hDevice = CreateFileW(
      L"\\\\.\\SudoVda",
      GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      NULL,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      NULL
    );
    if (hDevice != INVALID_HANDLE_VALUE) {
      CloseHandle(hDevice);
      return true;
    }
    return false;
  }

  std::optional<std::string> resolveVirtualDisplayDeviceId(const std::wstring &display_name) {
    if (display_name.empty()) {
      return std::nullopt;
    }

    auto devices = display_helper_integration::enumerate_devices();
    if (!devices) {
      return std::nullopt;
    }

    const auto utf8_name = platf::to_utf8(display_name);
    const auto target = normalize_display_name(utf8_name);
    if (target.empty()) {
      return std::nullopt;
    }

    for (const auto &device : *devices) {
      const auto device_name = normalize_display_name(device.m_display_name);
      if (!device_name.empty() && device_name == target) {
        return device.m_device_id;
      }
    }

    return resolveAnyVirtualDisplayDeviceId();
  }

  std::optional<std::string> resolveAnyVirtualDisplayDeviceId() {
    auto devices = display_helper_integration::enumerate_devices();
    if (!devices) {
      return std::nullopt;
    }

    const std::string sudoMakerDeviceString = "SudoMaker Virtual Display Adapter";
    std::optional<std::string> active_match;
    std::optional<std::string> any_match;

    for (const auto &device : *devices) {
      const bool is_virtual_id = equals_ci(device.m_device_id, SUDOVDA_VIRTUAL_DISPLAY_SELECTION);
      const bool is_virtual_friendly = equals_ci(device.m_friendly_name, sudoMakerDeviceString);
      if (!is_virtual_id && !is_virtual_friendly) {
        continue;
      }

      if (!any_match) {
        any_match = device.m_device_id;
      }
      if (device.m_info && !active_match) {
        active_match = device.m_device_id;
      }
    }

    if (active_match) {
      return active_match;
    }
    return any_match;
  }

  std::vector<SudaVDADisplayInfo> enumerateSudaVDADisplays() {
    std::vector<SudaVDADisplayInfo> result;

    if (!isSudaVDADriverInstalled()) {
      return result;
    }

    const auto devices = display_helper_integration::enumerate_devices();
    if (!devices) {
      return result;
    }

    const std::string sudoMakerDeviceString = "SudoMaker Virtual Display Adapter";
    for (const auto &device : *devices) {
      const bool is_virtual_id = equals_ci(device.m_device_id, SUDOVDA_VIRTUAL_DISPLAY_SELECTION);
      const bool is_friendly = equals_ci(device.m_friendly_name, sudoMakerDeviceString);
      if (!is_virtual_id && !is_friendly) {
        continue;
      }

      SudaVDADisplayInfo info;
      info.device_name = !device.m_display_name.empty() ? platf::from_utf8(device.m_display_name) : platf::from_utf8(device.m_device_id.empty() ? device.m_friendly_name : device.m_device_id);
      info.friendly_name = !device.m_friendly_name.empty() ? platf::from_utf8(device.m_friendly_name) : info.device_name;
      info.is_active = device.m_info.has_value();
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
  auto devices = display_helper_integration::enumerate_devices();
  if (!devices) {
    return true;
  }

  const std::string sudoMakerDeviceString = "SudoMaker Virtual Display Adapter";
  for (const auto &device : *devices) {
    const bool is_virtual_id = equals_ci(device.m_device_id, SUDOVDA_VIRTUAL_DISPLAY_SELECTION);
    const bool is_virtual_friendly = equals_ci(device.m_friendly_name, sudoMakerDeviceString);
    if (device.m_info && !is_virtual_id && !is_virtual_friendly) {
      return true;
    }
  }

  return false;
}

bool VDISPLAY::should_auto_enable_virtual_display() {
  if (!isSudaVDADriverInstalled()) {
    return false;
  }

  if (has_active_physical_display()) {
    return false;
  }

  return true;
}

uuid_util::uuid_t VDISPLAY::persistentVirtualDisplayUuid() {
  return ensure_persistent_guid();
}
