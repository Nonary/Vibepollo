#pragma once

#include "src/utility.h"
#include "src/uuid.h"
#include "src/platform/windows/virtual_display_policy.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
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

  // Only monitor paths identified as one of our virtual display devices may be
  // passed to the Windows DPI setter.
  bool is_virtual_display_monitor_path(const std::wstring &monitor_device_path);

  // Set the exact Windows scale for an active monitor. The monitor EDID's physical size is
  // also chosen to make this value Windows' recommended scale for new virtual displays.
  display_scale_result_t set_display_scale_percent(
    const std::wstring &monitor_device_path,
    std::uint32_t scale_percent
  );

  // Resolve the configured virtual-display scale. -1 selects a resolution-based
  // recommendation, 0 preserves Windows' existing choice, and positive values are exact.
  std::uint32_t effective_virtual_display_scale_percent(
    int configured_scale_percent,
    std::uint32_t width,
    std::uint32_t height
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
  bool setRenderAdapterByLuid(
    const LUID &adapter_luid,
    const std::wstring &adapter_name,
    std::uint64_t dedicated_video_memory,
    std::uint64_t shared_system_memory
  );
  bool setRenderAdapterByName(const std::wstring &adapterName);
  bool setRenderAdapterWithMostDedicatedMemory();
  bool applyConfiguredRenderAdapterPreference(std::string_view context);
  /**
   * Compare an owned display's stored render-adapter request provenance with
   * the currently configured preference. This does not issue a driver request
   * and does not claim to observe the adapter used by AssignSwapChain.
   */
  bool configuredRenderAdapterMatchesVirtualDisplay(const GUID &guid, std::string_view context);
  void ensureVirtualDisplayRegistryDefaults();

  struct VirtualDisplayCreationResult {
    std::optional<std::wstring> display_name;
    std::optional<std::string> device_id;
    std::optional<std::string> client_name;
    std::optional<std::wstring> monitor_device_path;
    bool reused_existing;
    bool confirmed_active = false;
    // Set when direct HDR handling confirms the target state. `false` means
    // the target was confirmed SDR; an unset value means direct handling was
    // unavailable or activation was deferred to the display helper.
    std::optional<bool> hdr_enabled;
    // Set only when this exact target was observed active. Consumers treat it as
    // an activation hint and skip their own activation wait, so publishing it for
    // a merely-enumerated target lets them skip a wait that was never satisfied.
    std::optional<std::chrono::steady_clock::time_point> ready_since;
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
    bool confirmed_active_at_schedule = false;
    unsigned int max_attempts = 3;
    // A successful callback may publish state (such as a runtime output
    // override) before the monitor makes its final cancellation decision.
    // Return rollback work for the monitor to invoke if that final decision
    // rejects the recreation; discard it on a committed recovery.
    std::function<std::function<void()>(const VirtualDisplayCreationResult &, std::stop_token)> on_recovery_success;
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
  // Stop session recovery workers without removing or untracking their displays.
  // Unlike process shutdown, later sessions may schedule fresh monitors.
  void cancel_all_virtual_display_recovery_monitors();
  void request_virtual_display_recovery_shutdown();
  void join_virtual_display_recovery_monitors();
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

  enum class ensure_display_readiness_e : std::uint8_t {
    unavailable,
    existing_display,
    request_retained,
    target_enumerated,
    target_ready,
  };

  enum class ensure_display_backend_e : std::uint8_t {
    none,
    sunshine,
    sudovda,
  };

  struct ensure_display_result {
    ensure_display_readiness_e readiness = ensure_display_readiness_e::unavailable;
    ensure_display_backend_e backend = ensure_display_backend_e::none;
    bool created_temporary = false;
    bool tracks_temporary_for_probe = false;
    std::uint64_t temporary_generation = 0;
    GUID temporary_guid {};
    std::string device_id;
    std::string display_name;

    [[nodiscard]] bool ready_for_probe() const {
      return readiness == ensure_display_readiness_e::existing_display ||
             (readiness == ensure_display_readiness_e::target_ready && !display_name.empty());
    }

  };

  /**
   * @brief Ensures a display is available for capture/encoding.
   * If no active physical displays exist, automatically creates a temporary virtual display.
   * @return Ownership, exact target identity, and readiness for encoder probing.
   */
  ensure_display_result ensure_display(const std::optional<LUID> &required_adapter_luid = std::nullopt);

  /**
   * @brief Resolve an exact Windows device id to a usable GDI display name.
   * @details Devices with a blank display name are intentionally rejected even
   * if Windows publishes mode or other device information for them.
   */
  std::optional<std::string> resolveUsableDisplayName(const std::string &device_id);

  /**
   * @brief Removes the temporary display created by a completed ensure_display() probe.
   * @param result The result from ensure_display() call.
   * @details Probe displays have no idle owner. Call this on every terminal
   *          probe path, including unavailable and failed probes.
   */
  void cleanup_ensure_display(const ensure_display_result &result);

  /**
   * @brief Removes the retained encoder-probe temporary display, if any.
   * @details Includes a display accepted by the driver before Windows publishes
   * a monitor identity.
   */
  void cleanup_retained_ensure_display();

  /**
   * @brief Returns true when ensure_display() is currently retaining a temporary display for probe retries.
   */
  bool has_retained_ensure_display();
}  // namespace VDISPLAY
