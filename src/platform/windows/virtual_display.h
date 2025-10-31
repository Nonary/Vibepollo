#pragma once

#include "src/utility.h"
#include "src/uuid.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <windows.h>

#ifndef FILE_DEVICE_UNKNOWN
  #define FILE_DEVICE_UNKNOWN 0x00000022
#endif

#include <ddk/d4iface.h>
#include <ddk/d4drvif.h>
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
  void setWatchdogFeedingEnabled(bool enable);
  bool setRenderAdapterByName(const std::wstring &adapterName);
  bool setRenderAdapterWithMostDedicatedMemory();
  struct VirtualDisplayCreationResult {
    std::optional<std::wstring> display_name;
    std::optional<std::string> device_id;
    bool reused_existing;
  };
  std::optional<VirtualDisplayCreationResult> createVirtualDisplay(
    const char *s_client_uid,
    const char *s_client_name,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    const GUID &guid
  );
  bool removeVirtualDisplay(const GUID &guid);

  std::optional<std::string> resolveVirtualDisplayDeviceId(const std::wstring &display_name);
  std::optional<std::string> resolveAnyVirtualDisplayDeviceId();

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

  uuid_util::uuid_t persistentVirtualDisplayUuid();
  bool has_active_physical_display();
  bool should_auto_enable_virtual_display();
}  // namespace VDISPLAY
