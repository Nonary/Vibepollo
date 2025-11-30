#pragma once

#include "src/utility.h"
#include "src/uuid.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <winsock2.h>
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
  bool ensure_driver_is_ready();
  bool startPingThread(std::function<void()> failCb);
  void setWatchdogFeedingEnabled(bool enable);
  bool setRenderAdapterByName(const std::wstring &adapterName);
  bool setRenderAdapterWithMostDedicatedMemory();
  void ensureVirtualDisplayRegistryDefaults();
  struct VirtualDisplayCreationResult {
    std::optional<std::wstring> display_name;
    std::optional<std::string> device_id;
    std::optional<std::string> client_name;
    std::optional<std::wstring> monitor_device_path;
    bool reused_existing;
    std::chrono::steady_clock::time_point ready_since;
  };

  struct VirtualDisplayRecoveryParams {
    GUID guid;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    std::string client_uid;
    std::string client_name;
    std::optional<std::wstring> display_name;
    std::optional<std::string> device_id;
    unsigned int max_attempts = 3;
    std::function<void(const VirtualDisplayCreationResult &)> on_recovery_success;
    std::function<bool()> should_abort;
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
  bool removeAllVirtualDisplays();
  void schedule_virtual_display_recovery_monitor(const VirtualDisplayRecoveryParams &params);
  bool is_virtual_display_guid_tracked(const GUID &guid);

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

  struct ensure_display_result {
    bool success;
    bool created_temporary;
    GUID temporary_guid;
  };

  /**
   * @brief Ensures a display is available for capture/encoding.
   * If no active physical displays exist, automatically creates a temporary virtual display.
   * @return Result indicating success and whether a temporary display was created.
   */
  ensure_display_result ensure_display();

  /**
   * @brief Cleans up temporary display created by ensure_display().
   * @param result The result from ensure_display() call.
   */
  void cleanup_ensure_display(const ensure_display_result &result);
}  // namespace VDISPLAY
