#pragma once

#include "src/utility.h"
#include "src/uuid.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <winsock2.h>
#include <windows.h>

namespace VDISPLAY {
  inline constexpr const char *VIRTUAL_DISPLAY_SELECTION = "sunshine:virtual_display";
  inline constexpr const char *SUDOVDA_VIRTUAL_DISPLAY_SELECTION = "sunshine:sudovda_virtual_display";

  struct advanced_color_profile_result_t {
    bool api_available = false;
    bool target_found = false;
    bool attempted = false;
    bool success = false;
    HRESULT association_status = E_NOTIMPL;
    HRESULT default_status = E_NOTIMPL;
  };

  // Windows HDR calibration profiles are Advanced Color associations. These helpers use
  // the display's CCD adapter/source identity so Windows consumes the profile's MHC2
  // luminance metadata and refreshes the effective HDR capabilities.
  std::optional<std::wstring> get_advanced_color_profile(
    const std::wstring &monitor_device_path,
    bool system_wide
  );
  advanced_color_profile_result_t set_advanced_color_profile(
    const std::wstring &monitor_device_path,
    const std::wstring &profile_name,
    bool system_wide
  );
  advanced_color_profile_result_t remove_advanced_color_profile(
    const std::wstring &monitor_device_path,
    const std::wstring &profile_name,
    bool system_wide
  );

  struct display_scale_result_t {
    bool target_found = false;
    bool queried = false;
    bool applied = false;
    std::uint32_t requested_percent = 0;
    std::uint32_t recommended_percent = 0;
    std::uint32_t previous_percent = 0;
    std::uint32_t current_percent = 0;
    LONG status = ERROR_NOT_SUPPORTED;
  };

  // Set the exact Windows scale for an active monitor. The monitor EDID's physical size is
  // also chosen to make this value Windows' recommended scale for new virtual displays.
  display_scale_result_t set_display_scale_percent(
    const std::wstring &monitor_device_path,
    std::uint32_t scale_percent
  );

  // Read the MHC2 peak-luminance value from a Windows HDR calibration profile selection.
  std::optional<std::uint32_t> hdr_profile_peak_luminance_nits(std::string_view selection);

  enum class DRIVER_STATUS {
    UNKNOWN = 1,
    OK = 0,
    FAILED = -1,
    VERSION_INCOMPATIBLE = -2,
    WATCHDOG_FAILED = -3
  };

  extern HANDLE VIRTUAL_DISPLAY_DRIVER_HANDLE;

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
    uint32_t base_fps_millihz = 0;
    bool framegen_refresh_active = false;
    int framegen_refresh_multiplier = 1;
    bool hdr_requested = false;
    std::string client_uid;
    std::string client_name;
    std::optional<std::string> hdr_profile;
    std::optional<std::wstring> display_name;
    std::optional<std::string> device_id;
    std::optional<std::wstring> monitor_device_path;
    unsigned int max_attempts = 3;
    std::function<void(const VirtualDisplayCreationResult &)> on_recovery_success;
    std::function<bool()> should_abort;
  };

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

  // Apply an HDR color profile to a physical output (best-effort).
  // If s_hdr_profile is null/empty, we fall back to matching by client name.
  void applyHdrProfileToOutput(
    const char *s_client_name,
    const char *s_hdr_profile,
    const char *s_device_id
  );

  // Restore any physical display color profiles that Sunshine overrode for streaming.
  // Virtual display associations are not restored.
  void restorePhysicalHdrProfiles();
  bool removeVirtualDisplay(const GUID &guid);
  bool removeAllVirtualDisplays();
  void schedule_virtual_display_recovery_monitor(const VirtualDisplayRecoveryParams &params);
  bool is_virtual_display_guid_tracked(const GUID &guid);

  std::optional<std::string> resolveVirtualDisplayDeviceId(const std::wstring &display_name);
  std::optional<std::string> resolveVirtualDisplayDeviceIdForClient(const std::string &client_name);
  std::optional<std::string> resolveActiveVirtualDisplayDeviceId(
    const std::string &preferred_output_identifier,
    const std::string &client_name,
    bool allow_any_fallback = true
  );
  std::optional<std::string> resolveActiveVirtualDisplayDeviceIdForStableId(
    const std::string &stable_id,
    const std::string &preferred_output_identifier,
    const std::string &client_name,
    bool allow_any_fallback = true
  );
  std::optional<std::string> resolveAnyVirtualDisplayDeviceId();
  bool is_virtual_display_output(const std::string &output_identifier);
  bool is_virtual_display_selection(const std::string &output_identifier);

  uint64_t client_uuid_to_virtual_display_id(const GUID &client_guid);
  uuid_util::uuid_t virtualDisplayUuidFromStableId(const std::string &stable_id);
  GUID sharedVirtualDisplayGuid();
  bool is_sunshine_virtual_display_identity(
    const std::string &device_path,
    const std::string &friendly_name,
    const std::string &edid_manufacturer_id,
    const std::string &edid_product_code
  );

  std::vector<std::wstring> matchDisplay(std::wstring sMatch);

  struct VirtualDisplayInfo {
    std::wstring device_name;
    std::wstring friendly_name;
    bool is_active;
    int width;
    int height;
  };

  bool isVirtualDisplayDriverInstalled();
  std::vector<VirtualDisplayInfo> enumerateVirtualDisplays();

  uuid_util::uuid_t persistentVirtualDisplayUuid();
  bool has_active_physical_display();
  bool should_auto_enable_virtual_display();

  struct ensure_display_result {
    bool success;
    bool created_temporary;
    bool tracks_temporary_for_probe;
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
   * @param probe_succeeded True when probe finished successfully.
   * @param allow_temporary_teardown False keeps the temporary display retained.
   */
  void cleanup_ensure_display(const ensure_display_result &result, bool probe_succeeded, bool allow_temporary_teardown = true);

  /**
   * @brief Returns true when ensure_display() is currently retaining a temporary display for probe retries.
   */
  bool has_retained_ensure_display();
}  // namespace VDISPLAY
