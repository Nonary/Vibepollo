#include "virtual_display.h"

#include "src/display_helper_integration.h"
#include "src/platform/windows/misc.h"

#include <cctype>
#include <combaseapi.h>
#include <dxgi.h>
#include <dxgi1_6.h>
#include <highlevelmonitorconfigurationapi.h>
#include <initguid.h>
#include <iostream>
#include <physicalmonitorenumerationapi.h>
#include <setupapi.h>
#include <thread>
#include <utility>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

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

    VIRTUAL_DISPLAY_ADD_OUT output;
    if (!AddVirtualDisplay(SUDOVDA_DRIVER_HANDLE, width, height, fps, guid, s_client_name, s_client_uid, output)) {
      printf("[SUDOVDA] Failed to add virtual display.\n");
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

    return std::wstring(deviceName);
  }

  bool removeVirtualDisplay(const GUID &guid) {
    if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
      return false;
    }

    if (RemoveVirtualDisplay(SUDOVDA_DRIVER_HANDLE, guid)) {
      printf("[SUDOVDA] Virtual display removed successfully.\n");
      return true;
    } else {
      return false;
    }
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
