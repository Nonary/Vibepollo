#include "virtual_display_legacy.h"

#include <algorithm>
#include <cmath>
#include <combaseapi.h>
#include <cstdlib>
#include <cstdio>
#include <dxgi.h>
#include <dxgi1_6.h>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

namespace VDISPLAY::legacy {

namespace {
  struct coordinates {
    int x;
    int y;
  };

  struct positionwidthheight {
    coordinates position;
    int width;
    int height;
    int modeindex;
  };

  struct coordinatesdifferences {
    coordinates left;
    coordinates right;
    coordinates Difference;
    coordinates AbsDifference;
  };

  std::vector<coordinates> moveToBeConnected(std::vector<coordinates> unknown, std::vector<coordinates> connected);

  std::vector<std::wstring> matchDisplay(std::wstring sMatch) {
    std::vector<std::wstring> displays;

    DISPLAYCONFIG_PATH_INFO pathInfo[256];
    UINT32 num_paths = 256;
    DISPLAYCONFIG_MODE_INFO modeInfo[256];
    UINT32 num_modes = 256;
    LONG result;

    result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &num_paths, &num_modes);
    if (result != ERROR_SUCCESS) {
      return displays;
    }

    result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &num_paths, pathInfo, &num_modes, modeInfo, nullptr);
    if (result != ERROR_SUCCESS) {
      return displays;
    }

    DISPLAYCONFIG_SOURCE_DEVICE_NAME deviceName;
    deviceName.header.size = sizeof(deviceName);
    deviceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;

    for (UINT32 i = 0; i < num_paths; i++) {
      deviceName.header.adapterId = pathInfo[i].sourceInfo.adapterId;
      deviceName.header.id = pathInfo[i].sourceInfo.id;
      result = DisplayConfigGetDeviceInfo(&deviceName.header);
      if (result != ERROR_SUCCESS) {
        continue;
      }

      std::wstring wDisplayName(deviceName.viewGdiDeviceName);
      if (wDisplayName.find(sMatch) != std::wstring::npos) {
        displays.push_back(wDisplayName);
      }
    }

    return displays;
  }

  std::vector<positionwidthheight *> rearrangeVirtualDisplayForLowerRight(std::vector<positionwidthheight *> displays) {
    std::vector<positionwidthheight *> displayArray = displays;
    int iIndex;
    
    struct coordinatesdifferences differences;
    std::vector<struct coordinates> notplaced;
    std::vector<struct coordinates> connected;
    struct coordinates display;
    struct coordinates position;

    // Start with unconnected display
    notplaced.push_back(displayArray[0]->position);

    for (iIndex = 1; iIndex < displayArray.size(); iIndex++) {
      connected.clear();

      differences.left.x = notplaced[0].x - displayArray[iIndex]->position.x;
      differences.left.y = notplaced[0].y - displayArray[iIndex]->position.y;

      differences.right.x = differences.left.x + displayArray[iIndex]->width;
      differences.right.y = differences.left.y + displayArray[iIndex]->height;

      differences.Difference.x = differences.right.x - differences.left.x;
      differences.Difference.y = differences.right.y - differences.left.y;

    differences.AbsDifference.x = std::abs(differences.right.x - differences.left.x);
    differences.AbsDifference.y = std::abs(differences.right.y - differences.left.y);

      if (differences.AbsDifference.x == displayArray[iIndex]->width) {
        display.x = displayArray[iIndex]->position.x + differences.left.x;
        display.y = displayArray[iIndex]->position.y;
        connected.push_back(display);

        display.x = displayArray[iIndex]->position.x + differences.right.x;
        display.y = displayArray[iIndex]->position.y + differences.Difference.y;
        connected.push_back(display);
      }

      if (differences.AbsDifference.y == displayArray[iIndex]->height) {
        display.x = displayArray[iIndex]->position.x;
        display.y = displayArray[iIndex]->position.y + differences.left.y;
        connected.push_back(display);

        display.x = displayArray[iIndex]->position.x + differences.Difference.x;
        display.y = displayArray[iIndex]->position.y + differences.right.y;
        connected.push_back(display);
      }

      if (connected.size() > 0) {
        // Move the notplaced display to be connected
        auto moveDisplay = moveToBeConnected(notplaced, connected);
        position.x = moveDisplay[1].x;
        position.y = moveDisplay[1].y;

        displayArray[0]->position.x = position.x;
        displayArray[0]->position.y = position.y;
        break;
      }
    }

    return displayArray;
  }

  std::string printAllDisplays(const std::vector<positionwidthheight *> &displays) {
    std::string output;
    for (size_t i = 0; i < displays.size(); ++i) {
      output += "Index: " + std::to_string(i);
      output += ", X : " + std::to_string(displays[i]->position.x);
      output += ", Y : " + std::to_string(displays[i]->position.y);
      output += ", width : " + std::to_string(displays[i]->width);
      output += ", height : " + std::to_string(displays[i]->height);
      output += "\n";
    }
    return output;
  }

  std::vector<coordinates> moveToBeConnected(std::vector<coordinates> unknown, std::vector<coordinates> connected) {
    std::vector<coordinatesdifferences> differences;
    std::vector<coordinatesdifferences> vertical;
    std::vector<coordinatesdifferences> horizontal;

    for (size_t unknown_index = 0; unknown_index < unknown.size(); unknown_index += 1) {
      for (size_t connected_index = 0; connected_index < connected.size(); connected_index += 1) {
        coordinatesdifferences difference {};
        difference.left.x = connected[connected_index].x - unknown[unknown_index].x;
        difference.left.y = connected[connected_index].y - unknown[unknown_index].y;
        differences.push_back(difference);

        if (difference.left.x == 0) {
          vertical.push_back(difference);
        }
        if (difference.left.y == 0) {
          horizontal.push_back(difference);
        }
      }
    }

    if (vertical.size() > 0) {
      return {
        {0, vertical[0].left.y},
        {vertical[0].left.x, vertical[0].left.y}
      };
    }

    return {
      {horizontal[0].left.x, 0},
      {horizontal[0].left.x, horizontal[0].left.y}
    };
  }

  void freeDisplayArray(std::vector<positionwidthheight *> &displayArray) {
    for (auto *display : displayArray) {
      delete display;
    }
    displayArray.clear();
  }

  bool findDisplayIds(const wchar_t *displayName, LUID &adapterId, uint32_t &targetId) {
    UINT32 pathCount;
    UINT32 modeCount;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount)) {
      return false;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr)) {
      return false;
    }

    auto path = std::find_if(paths.begin(), paths.end(), [&displayName](DISPLAYCONFIG_PATH_INFO candidate) {
      DISPLAYCONFIG_PATH_SOURCE_INFO sourceInfo = candidate.sourceInfo;

      DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
      sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
      sourceName.header.size = sizeof(sourceName);
      sourceName.header.adapterId = sourceInfo.adapterId;
      sourceName.header.id = sourceInfo.id;

      if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS) {
        return false;
      }

      return std::wstring_view(displayName) == sourceName.viewGdiDeviceName;
    });

    if (path == paths.end()) {
      return false;
    }

    adapterId = path->sourceInfo.adapterId;
    targetId = path->targetInfo.id;

    return true;
  }

  bool getDisplayHDR(const LUID &adapterLuid, const wchar_t *displayName) {
    Microsoft::WRL::ComPtr<IDXGIFactory1> dxgiFactory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory));
    if (FAILED(hr)) {
      wprintf(L"[SUDOVDA] CreateDXGIFactory1 failed in getDisplayHDR! hr=0x%lx\n", hr);
      return false;
    }

    for (UINT adapterIdx = 0;; ++adapterIdx) {
      Microsoft::WRL::ComPtr<IDXGIAdapter1> currentAdapter;
      hr = dxgiFactory->EnumAdapters1(adapterIdx, currentAdapter.ReleaseAndGetAddressOf());

      if (hr == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      if (FAILED(hr)) {
        wprintf(L"[SUDOVDA] EnumAdapters1 failed for index %u in getDisplayHDR! hr=0x%lx\n", adapterIdx, hr);
        break;
      }

      DXGI_ADAPTER_DESC1 adapterDesc;
      hr = currentAdapter->GetDesc1(&adapterDesc);
      if (FAILED(hr)) {
        wprintf(L"[SUDOVDA] GetDesc1 (Adapter) failed for index %u in getDisplayHDR! hr=0x%lx\n", adapterIdx, hr);
        continue;
      }

      if (adapterDesc.AdapterLuid.LowPart == adapterLuid.LowPart && adapterDesc.AdapterLuid.HighPart == adapterLuid.HighPart) {
        std::wstring_view displayNameView {displayName};

        for (UINT outputIdx = 0;; ++outputIdx) {
          Microsoft::WRL::ComPtr<IDXGIOutput> dxgiOutput;
          hr = currentAdapter->EnumOutputs(outputIdx, dxgiOutput.ReleaseAndGetAddressOf());

          if (hr == DXGI_ERROR_NOT_FOUND) {
            wprintf(L"[SUDOVDA] No more DXGI outputs on matched adapter for GDI name %ls.\n", displayName);
            break;
          }
          if (FAILED(hr) || !dxgiOutput) {
            continue;
          }

          DXGI_OUTPUT_DESC dxgiOutputDesc;
          hr = dxgiOutput->GetDesc(&dxgiOutputDesc);
          if (FAILED(hr)) {
            continue;
          }

          MONITORINFOEXW monitorInfoEx = {};
          monitorInfoEx.cbSize = sizeof(MONITORINFOEXW);
          if (GetMonitorInfoW(dxgiOutputDesc.Monitor, &monitorInfoEx)) {
            if (displayNameView == monitorInfoEx.szDevice) {
              wprintf(L"[SUDOVDA] Matched DXGI output GDI name: %ls\n", monitorInfoEx.szDevice);
              Microsoft::WRL::ComPtr<IDXGIOutput6> dxgiOutput6;
              hr = dxgiOutput.As(&dxgiOutput6);

              if (SUCCEEDED(hr) && dxgiOutput6) {
                DXGI_OUTPUT_DESC1 outputDesc1;
                hr = dxgiOutput6->GetDesc1(&outputDesc1);
                if (SUCCEEDED(hr)) {
                  if (outputDesc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
                    return true;
                  }
                } else {
                  wprintf(L"[SUDOVDA] GetDesc1 (Output) failed for %ls. hr=0x%lx\n", monitorInfoEx.szDevice, hr);
                }
              } else {
                wprintf(L"[SUDOVDA] QueryInterface for IDXGIOutput6 failed for %ls. hr=0x%lx. HDR check method not available or output not capable.\n", monitorInfoEx.szDevice, hr);
              }
              return false;
            }
          } else {
            DWORD lastError = GetLastError();
            wprintf(L"[SUDOVDA] GetMonitorInfoW failed for HMONITOR 0x%p from DXGI output %ls. Error: %lu\n", dxgiOutputDesc.Monitor, dxgiOutputDesc.DeviceName, lastError);
          }
        }

        wprintf(L"[SUDOVDA] Target GDI name %ls not found among DXGI outputs of the matched adapter.\n", displayName);
        return false;
      }
    }

    wprintf(L"[SUDOVDA] Target adapter LUID {%lx-%lx} not found via DXGI.\n", adapterLuid.HighPart, adapterLuid.LowPart);
    return false;
  }

  bool setDisplayHDR(const LUID &adapterId, const uint32_t &targetId, bool enableAdvancedColor) {
    DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE setHdrInfo = {};
    setHdrInfo.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
    setHdrInfo.header.size = sizeof(setHdrInfo);
    setHdrInfo.header.adapterId = adapterId;
    setHdrInfo.header.id = targetId;
    setHdrInfo.enableAdvancedColor = enableAdvancedColor;

    return DisplayConfigSetDeviceInfo(&setHdrInfo.header) == ERROR_SUCCESS;
  }
}  // namespace

LONG changeDisplaySettings2(const wchar_t *deviceName, int width, int height, int refresh_rate, bool apply_isolated) {
  UINT32 pathCount = 0;
  UINT32 modeCount = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount)) {
    wprintf(L"[SUDOVDA] Failed to query display configuration size.\n");
    return ERROR_INVALID_PARAMETER;
  }

  std::vector<DISPLAYCONFIG_PATH_INFO> pathArray(pathCount);
  std::vector<DISPLAYCONFIG_MODE_INFO> modeArray(modeCount);
  std::vector<positionwidthheight *> displayArray;

  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, pathArray.data(), &modeCount, modeArray.data(), nullptr) != ERROR_SUCCESS) {
    wprintf(L"[SUDOVDA] Failed to query display configuration.\n");
    return ERROR_INVALID_PARAMETER;
  }

  bool virtual_display_already_added = false;
  std::string display_output;

  if (apply_isolated) {
    for (UINT32 i = 0; i < pathCount; i++) {
      DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
      sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
      sourceName.header.size = sizeof(sourceName);
      sourceName.header.adapterId = pathArray[i].sourceInfo.adapterId;
      sourceName.header.id = pathArray[i].sourceInfo.id;
      bool at_virtual_display = false;

      if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS) {
        continue;
      }

      auto *sourceInfo = &pathArray[i].sourceInfo;
      auto *targetInfo = &pathArray[i].targetInfo;

      if (std::wstring_view(sourceName.viewGdiDeviceName) == std::wstring_view(deviceName)) {
        at_virtual_display = true;
      }

      for (UINT32 j = 0; j < modeCount; j++) {
        if (
          modeArray[j].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE &&
          modeArray[j].adapterId.HighPart == sourceInfo->adapterId.HighPart &&
          modeArray[j].adapterId.LowPart == sourceInfo->adapterId.LowPart &&
          modeArray[j].id == sourceInfo->id
        ) {
          auto *sourceMode = &modeArray[j].sourceMode;

          wprintf(L"[SUDOVDA] Current mode found: [%dx%dx%d]\n", sourceMode->width, sourceMode->height, targetInfo->refreshRate);

          auto *current = new positionwidthheight;
          current->position.x = modeArray[j].sourceMode.position.x;
          current->position.y = modeArray[j].sourceMode.position.y;
          current->height = modeArray[j].sourceMode.height;
          current->width = modeArray[j].sourceMode.width;
          current->modeindex = j;

          if (at_virtual_display && !virtual_display_already_added) {
            displayArray.insert(displayArray.begin(), current);
            virtual_display_already_added = true;
          } else {
            displayArray.push_back(current);
          }
        }
      }
    }

    display_output = "Before: \n";
    display_output += printAllDisplays(displayArray);

    displayArray = rearrangeVirtualDisplayForLowerRight(displayArray);

    display_output += "After: \n";
    display_output += printAllDisplays(displayArray);

    int primary_index = -1;
    for (int idx = 0; idx < static_cast<int>(displayArray.size()); ++idx) {
      if (
        modeArray[displayArray[idx]->modeindex].sourceMode.position.x == 0 &&
        modeArray[displayArray[idx]->modeindex].sourceMode.position.y == 0
      ) {
        primary_index = idx;
        break;
      }
    }

    int xdifference = 0;
    int ydifference = 0;
    if (primary_index >= 0) {
      xdifference = (displayArray[primary_index]->position.x) * -1;
      ydifference = (displayArray[primary_index]->position.y) * -1;
    }

    for (auto *display : displayArray) {
      auto &mode = modeArray[display->modeindex].sourceMode;
      mode.position.x = display->position.x + xdifference;
      mode.position.y = display->position.y + ydifference;
    }
  }

  auto displays = matchDisplay(deviceName);
  if (displays.empty()) {
    wprintf(L"[SUDOVDA] Display not found: %ls\n", deviceName);
    freeDisplayArray(displayArray);
    return ERROR_DEVICE_NOT_CONNECTED;
  }

  for (UINT32 i = 0; i < pathCount; i++) {
    auto *sourceInfo = &pathArray[i].sourceInfo;
    auto *targetInfo = &pathArray[i].targetInfo;

    DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
    sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    sourceName.header.size = sizeof(sourceName);
    sourceName.header.adapterId = sourceInfo->adapterId;
    sourceName.header.id = sourceInfo->id;

    if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS) {
      continue;
    }

    if (std::wstring_view(sourceName.viewGdiDeviceName) != std::wstring_view(deviceName)) {
      continue;
    }

    DISPLAYCONFIG_PATH_INFO pathInfo = pathArray[i];

    for (UINT32 j = 0; j < modeCount; j++) {
      if (
        modeArray[j].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE &&
        modeArray[j].adapterId.HighPart == sourceInfo->adapterId.HighPart &&
        modeArray[j].adapterId.LowPart == sourceInfo->adapterId.LowPart &&
        modeArray[j].id == sourceInfo->id
      ) {
        modeArray[j].sourceMode.width = width;
        modeArray[j].sourceMode.height = height;
        if (!apply_isolated) {
          modeArray[j].sourceMode.position.x = 0;
          modeArray[j].sourceMode.position.y = 0;
        }
      }
      if (
        modeArray[j].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET &&
        modeArray[j].adapterId.HighPart == targetInfo->adapterId.HighPart &&
        modeArray[j].adapterId.LowPart == targetInfo->adapterId.LowPart &&
        modeArray[j].id == targetInfo->id
      ) {
        modeArray[j].targetMode.targetVideoSignalInfo.vSyncFreq.Denominator = 1000;
        modeArray[j].targetMode.targetVideoSignalInfo.vSyncFreq.Numerator = refresh_rate;
      }
    }

    pathArray.clear();
    pathArray.push_back(pathInfo);
    break;
  }

  LONG result = SetDisplayConfig(pathCount, pathArray.data(), modeCount, modeArray.data(), SDC_APPLY | SDC_SAVE_TO_DATABASE);
  if (result != ERROR_SUCCESS) {
    wprintf(L"[SUDOVDA] Failed to apply display configuration (%ld).\n", result);
  } else if (!display_output.empty()) {
    printf("%s", display_output.c_str());
  }

  freeDisplayArray(displayArray);
  return result;
}

LONG changeDisplaySettings(const wchar_t *deviceName, int width, int height, int refresh_rate) {
  DEVMODEW devMode = {};
  devMode.dmSize = sizeof(devMode);

  if (EnumDisplaySettingsW(deviceName, ENUM_CURRENT_SETTINGS, &devMode)) {
    DWORD targetRefreshRate = refresh_rate / 1000;
    DWORD altRefreshRate = targetRefreshRate;

    if (refresh_rate % 1000) {
      if (refresh_rate % 1000 >= 900) {
        targetRefreshRate += 1;
      } else {
        altRefreshRate += 1;
      }
    } else {
      altRefreshRate -= 1;
    }

    wprintf(L"[SUDOVDA] Applying baseline display mode [%dx%dx%d] for %ls.\n", width, height, targetRefreshRate, deviceName);

    devMode.dmPelsWidth = width;
    devMode.dmPelsHeight = height;
    devMode.dmDisplayFrequency = targetRefreshRate;
    devMode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;

    auto res = ChangeDisplaySettingsExW(deviceName, &devMode, nullptr, CDS_UPDATEREGISTRY, nullptr);

    if (res != ERROR_SUCCESS) {
      wprintf(L"[SUDOVDA] Failed to apply baseline display mode, trying alt mode: [%dx%dx%d].\n", width, height, altRefreshRate);
      devMode.dmDisplayFrequency = altRefreshRate;
      res = ChangeDisplaySettingsExW(deviceName, &devMode, nullptr, CDS_UPDATEREGISTRY, nullptr);
      if (res != ERROR_SUCCESS) {
        wprintf(L"[SUDOVDA] Failed to apply alt baseline display mode.\n");
      }
    }

    if (res == ERROR_SUCCESS) {
      wprintf(L"[SUDOVDA] Baseline display mode applied successfully.");
    }
  }

  return changeDisplaySettings2(deviceName, width, height, refresh_rate, false);
}

bool getDisplayHDRByName(const wchar_t *displayName) {
  LUID adapterId;
  uint32_t targetId;

  if (!findDisplayIds(displayName, adapterId, targetId)) {
    wprintf(L"[SUDOVDA] Failed to find display IDs for %ls!\n", displayName);
    return false;
  }

  return getDisplayHDR(adapterId, displayName);
}

bool setDisplayHDRByName(const wchar_t *displayName, bool enableAdvancedColor) {
  LUID adapterId;
  uint32_t targetId;

  if (!findDisplayIds(displayName, adapterId, targetId)) {
    return false;
  }

  return setDisplayHDR(adapterId, targetId, enableAdvancedColor);
}

}  // namespace VDISPLAY::legacy
