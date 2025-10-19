#include "virtual_display.h"

#include "src/display_helper_integration.h"
#include "src/platform/common.h"
#include "src/platform/windows/misc.h"
#include "src/uuid.h"

#include <cctype>
#include <combaseapi.h>
#include <cstdlib>
#include <cstring>
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

    std::string guid_to_string(const GUID &guid) {
      wchar_t buffer[64] {};
      if (StringFromGUID2(guid, buffer, static_cast<int>(_countof(buffer))) <= 0) {
        return {};
      }
      return platf::to_utf8(std::wstring(buffer));
    }

    std::optional<GUID> string_to_guid(const std::string &text) {
      if (text.empty()) {
        return std::nullopt;
      }
      auto wide = platf::from_utf8(text);
      GUID parsed {};
      if (FAILED(CLSIDFromString(wide.c_str(), &parsed))) {
        return std::nullopt;
      }
      return parsed;
    }

    bool force_remove_virtual_display() {
      static const bool force = []() {
        const char *env = std::getenv("SUNSHINE_VDISPLAY_FORCE_REMOVE");
        return env && env[0] != '\0' && env[0] != '0';
      }();
      return force;
    }

    struct VirtualDisplayCache {
      struct entry_t {
        std::wstring display_name;
        std::string device_id;
        std::optional<GUID> guid;
        std::optional<uint32_t> fps;
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
          if (auto guid_it = json.find("guid"); guid_it != json.end() && guid_it->is_string()) {
            if (auto parsed = string_to_guid(guid_it->get<std::string>())) {
              tmp.guid = *parsed;
            }
          }
          if (auto fps_it = json.find("fps"); fps_it != json.end() && fps_it->is_number()) {
            try {
              const auto value = fps_it->get<int64_t>();
              if (value > 0 && value <= static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
                tmp.fps = static_cast<uint32_t>(value);
              }
            } catch (...) {
              tmp.fps.reset();
            }
          }

          if (!tmp.display_name.empty() || tmp.guid) {
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
          if (entry->guid) {
            json["guid"] = guid_to_string(*entry->guid);
          }
          if (entry->fps) {
            json["fps"] = *entry->fps;
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

      void clear_entry() {
        std::lock_guard<std::mutex> lg(mutex);
        ensure_loaded_locked();
        entry.reset();
        save_locked();
      }

      std::optional<GUID> cached_guid() {
        std::lock_guard<std::mutex> lg(mutex);
        ensure_loaded_locked();
        if (entry && entry->guid) {
          return entry->guid;
        }
        return std::nullopt;
      }
    };
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
    std::optional<VirtualDisplayCache::entry_t> previous_entry;
    bool cache_cleared_for_refresh_change = false;
    if (cached_entry && cached_entry->guid && IsEqualGUID(*cached_entry->guid, guid)) {
      const bool fps_known = cached_entry->fps.has_value();
      const uint32_t cached_fps = cached_entry->fps.value_or(0u);
      previous_entry = *cached_entry;
      if (!fps_known || cached_fps != fps) {
        printf(
          "[SUDOVDA] Virtual display refresh change detected (cached=%u, requested=%u); recreating instance.\n",
          static_cast<unsigned int>(cached_fps),
          static_cast<unsigned int>(fps)
        );
        if (RemoveVirtualDisplay(SUDOVDA_DRIVER_HANDLE, guid)) {
          VirtualDisplayCache::instance().clear_entry();
          cached_entry.reset();
          cache_cleared_for_refresh_change = true;
          Sleep(50);
        } else {
          const DWORD remove_error = GetLastError();
          printf(
            "[SUDOVDA] Failed to remove virtual display for refresh change (error=%lu).\n",
            static_cast<unsigned long>(remove_error)
          );
        }
      }
    }

    VIRTUAL_DISPLAY_ADD_OUT output;
    if (!AddVirtualDisplay(SUDOVDA_DRIVER_HANDLE, width, height, fps, guid, s_client_name, s_client_uid, output)) {
      const DWORD error_code = GetLastError();
      if (cache_cleared_for_refresh_change && previous_entry) {
        VirtualDisplayCache::instance().set_entry(*previous_entry);
      }
      if (!force_remove_virtual_display()) {
        if (auto cached = VirtualDisplayCache::instance().get_entry()) {
          const bool guid_matches = cached->guid && IsEqualGUID(*cached->guid, guid);
          const bool fps_known = cached->fps.has_value();
          const bool fps_matches = fps_known && *cached->fps == fps;
          if (guid_matches && (!fps_known || !fps_matches)) {
            printf("[SUDOVDA] Skipping reuse of virtual display due to refresh mismatch.\n");
          } else if (fps_known && !fps_matches) {
            printf(
              "[SUDOVDA] Skipping reuse of cached virtual display with mismatched refresh (%u != %u).\n",
              static_cast<unsigned int>(*cached->fps),
              static_cast<unsigned int>(fps)
            );
          } else if (!cached->display_name.empty() && (!fps_known || fps_matches)) {
            auto reuse_name = cached->display_name;
            auto device_id = resolveVirtualDisplayDeviceId(reuse_name);

            VirtualDisplayCache::entry_t updated;
            updated.display_name = reuse_name;
            if (device_id && !device_id->empty()) {
              updated.device_id = *device_id;
            }
            updated.guid = guid;
            updated.fps = cached->fps;
            VirtualDisplayCache::instance().set_entry(std::move(updated));

            wprintf(
              L"[SUDOVDA] Reusing existing virtual display (error=%lu): %ls\n",
              static_cast<unsigned long>(error_code),
              reuse_name.c_str()
            );
            return reuse_name;
          }
        }
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

    std::optional<std::string> device_id;
    if (auto resolved = resolveVirtualDisplayDeviceId(deviceName)) {
      device_id = *resolved;
    }

    VirtualDisplayCache::entry_t cache_entry;
    cache_entry.display_name = deviceName;
    if (device_id && !device_id->empty()) {
      cache_entry.device_id = *device_id;
    }
    cache_entry.guid = guid;
    cache_entry.fps = fps;
    VirtualDisplayCache::instance().set_entry(std::move(cache_entry));

    return std::wstring(deviceName);
  }

  bool removeVirtualDisplay(const GUID &guid) {
    if (!force_remove_virtual_display()) {
      printf("[SUDOVDA] Keeping virtual display active to preserve Windows settings.\n");
      return true;
    }

    if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
      return false;
    }

    if (RemoveVirtualDisplay(SUDOVDA_DRIVER_HANDLE, guid)) {
      VirtualDisplayCache::instance().clear_entry();
      printf("[SUDOVDA] Virtual display removed successfully.\n");
      return true;
    }

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

std::optional<uuid_util::uuid_t> VDISPLAY::cachedVirtualDisplayUuid() {
  if (force_remove_virtual_display()) {
    return std::nullopt;
  }

  const auto cached_guid = VirtualDisplayCache::instance().cached_guid();
  if (!cached_guid) {
    return std::nullopt;
  }

  uuid_util::uuid_t out {};
  static_assert(sizeof(out.b8) == sizeof(GUID), "GUID size mismatch");
  std::memcpy(out.b8, &*cached_guid, sizeof(GUID));
  return out;
}

bool VDISPLAY::shouldForceVirtualDisplayRemove() {
  return force_remove_virtual_display();
}
