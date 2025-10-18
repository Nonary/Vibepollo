#pragma once

#include <functional>
#include <vector>
#include <windows.h>

#ifndef FILE_DEVICE_UNKNOWN
  #define FILE_DEVICE_UNKNOWN 0x00000022
#endif

#include <ddk/d4drvif.h>
#include <ddk/d4iface.h>
#include <sudovda/sudovda.h>

namespace VDISPLAY {
  inline constexpr const char *SUDOVDA_VIRTUAL_DISPLAY_SELECTION = "sunshine:sudovda_virtual_display";

  enum class DRIVER_STATUS {
    UNKNOWN = 1,
    OK = 0,
    FAILED = -1,
    VERSION_INCOMPATIBLE = -2,
    WATCHDOG_FAILED = -3
  };

  extern HANDLE SUDOVDA_DRIVER_HANDLE;

  void closeVDisplayDevice();
  DRIVER_STATUS openVDisplayDevice();
  bool startPingThread(std::function<void()> failCb);
  bool setRenderAdapterByName(const std::wstring &adapterName);
  std::wstring createVirtualDisplay(
    const char *s_client_uid,
    const char *s_client_name,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    const GUID &guid
  );
  bool removeVirtualDisplay(const GUID &guid);

  std::vector<std::wstring> matchDisplay(std::wstring sMatch);

  struct SudaVDADisplayInfo {
    std::wstring device_name;
    std::wstring friendly_name;
    bool is_active;
    int width;
    int height;
  };

  bool isSudaVDADriverInstalled();
  std::vector<SudaVDADisplayInfo> enumerateSudaVDADisplays();
}  // namespace VDISPLAY
