#include "virtual_display.h"

#include "src/config.h"

namespace VDISPLAY {
  HANDLE VIRTUAL_DISPLAY_DRIVER_HANDLE = INVALID_HANDLE_VALUE;
}

namespace VDISPLAY_SUNSHINE {
  using VDISPLAY::DRIVER_STATUS;
  using VDISPLAY::VirtualDisplayCreationResult;
  using VDISPLAY::VirtualDisplayInfo;
  using VDISPLAY::VirtualDisplayRecoveryParams;
  using VDISPLAY::ensure_display_result;

  void closeVDisplayDevice();
  DRIVER_STATUS openVDisplayDevice();
  bool ensure_driver_is_ready();
  bool startPingThread(std::function<void()> failCb);
  void setWatchdogFeedingEnabled(bool enable);
  bool setRenderAdapterByName(const std::wstring &adapterName);
  bool setRenderAdapterWithMostDedicatedMemory();
  void ensureVirtualDisplayRegistryDefaults();
  std::optional<VirtualDisplayCreationResult> createVirtualDisplay(
    const char *s_client_uid,
    const char *s_client_name,
    const char *s_hdr_profile,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    const GUID &guid,
    uint32_t base_fps_millihz,
    bool framegen_refresh_active,
    int framegen_refresh_multiplier,
    bool hdr_requested,
    bool allow_pending_enumeration,
    bool replace_existing
  );
  void applyHdrProfileToOutput(const char *s_client_name, const char *s_hdr_profile, const char *s_device_id);
  void restorePhysicalHdrProfiles();
  bool removeVirtualDisplay(const GUID &guid);
  bool removeAllVirtualDisplays();
  void schedule_virtual_display_recovery_monitor(const VirtualDisplayRecoveryParams &params);
  bool is_virtual_display_guid_tracked(const GUID &guid);
  std::optional<std::string> resolveVirtualDisplayDeviceId(const std::wstring &display_name);
  std::optional<std::string> resolveVirtualDisplayDeviceIdForClient(const std::string &client_name);
  std::optional<std::string> resolveActiveVirtualDisplayDeviceId(const std::string &preferred_output_identifier, const std::string &client_name, bool allow_any_fallback);
  std::optional<std::string> resolveActiveVirtualDisplayDeviceIdForStableId(const std::string &stable_id, const std::string &preferred_output_identifier, const std::string &client_name, bool allow_any_fallback);
  std::optional<std::string> resolveAnyVirtualDisplayDeviceId();
  bool is_virtual_display_output(const std::string &output_identifier);
  bool is_virtual_display_selection(const std::string &output_identifier);
  uint64_t client_uuid_to_virtual_display_id(const GUID &client_guid);
  uuid_util::uuid_t virtualDisplayUuidFromStableId(const std::string &stable_id);
  GUID sharedVirtualDisplayGuid();
  bool is_sunshine_virtual_display_identity(const std::string &device_path, const std::string &friendly_name, const std::string &edid_manufacturer_id, const std::string &edid_product_code);
  bool isVirtualDisplayDriverInstalled();
  std::vector<VirtualDisplayInfo> enumerateVirtualDisplays();
  uuid_util::uuid_t persistentVirtualDisplayUuid();
  bool has_active_physical_display();
  bool should_auto_enable_virtual_display();
  ensure_display_result ensure_display();
  void cleanup_ensure_display(const ensure_display_result &result, bool probe_succeeded, bool allow_temporary_teardown);
  bool has_retained_ensure_display();
}  // namespace VDISPLAY_SUNSHINE

namespace VDISPLAY_SUDOVDA {
  using VDISPLAY::DRIVER_STATUS;
  using VDISPLAY::VirtualDisplayCreationResult;
  using VDISPLAY::VirtualDisplayInfo;
  using VDISPLAY::VirtualDisplayRecoveryParams;
  using VDISPLAY::ensure_display_result;

  void closeVDisplayDevice();
  DRIVER_STATUS openVDisplayDevice();
  bool ensure_driver_is_ready();
  bool startPingThread(std::function<void()> failCb);
  void setWatchdogFeedingEnabled(bool enable);
  bool setRenderAdapterByName(const std::wstring &adapterName);
  bool setRenderAdapterWithMostDedicatedMemory();
  void ensureVirtualDisplayRegistryDefaults();
  std::optional<VirtualDisplayCreationResult> createVirtualDisplay(
    const char *s_client_uid,
    const char *s_client_name,
    const char *s_hdr_profile,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    const GUID &guid,
    uint32_t base_fps_millihz,
    bool framegen_refresh_active,
    int framegen_refresh_multiplier,
    bool hdr_requested,
    bool replace_existing
  );
  void applyHdrProfileToOutput(const char *s_client_name, const char *s_hdr_profile, const char *s_device_id);
  void restorePhysicalHdrProfiles();
  bool removeVirtualDisplay(const GUID &guid);
  bool removeAllVirtualDisplays();
  void schedule_virtual_display_recovery_monitor(const VirtualDisplayRecoveryParams &params);
  bool is_virtual_display_guid_tracked(const GUID &guid);
  std::optional<std::string> resolveVirtualDisplayDeviceId(const std::wstring &display_name);
  std::optional<std::string> resolveVirtualDisplayDeviceIdForClient(const std::string &client_name);
  std::optional<std::string> resolveActiveVirtualDisplayDeviceId(const std::string &preferred_output_identifier, const std::string &client_name, bool allow_any_fallback);
  std::optional<std::string> resolveAnyVirtualDisplayDeviceId();
  bool is_virtual_display_output(const std::string &output_identifier);
  bool is_virtual_display_selection(const std::string &output_identifier);
  uint64_t client_uuid_to_vdd_display_id(const GUID &client_guid);
  GUID sharedVirtualDisplayGuid();
  bool isSudaVDADriverInstalled();
  std::vector<VirtualDisplayInfo> enumerateSudaVDADisplays();
  uuid_util::uuid_t persistentVirtualDisplayUuid();
  bool has_active_physical_display();
  bool should_auto_enable_virtual_display();
  ensure_display_result ensure_display();
  void cleanup_ensure_display(const ensure_display_result &result, bool probe_succeeded, bool allow_temporary_teardown);
  bool has_retained_ensure_display();
}  // namespace VDISPLAY_SUDOVDA

namespace {
  bool use_sunshine_driver() {
    return config::video.dd.use_sunshine_virtual_display_driver;
  }
}  // namespace

namespace VDISPLAY {
  void closeVDisplayDevice() {
    VDISPLAY_SUNSHINE::closeVDisplayDevice();
    VDISPLAY_SUDOVDA::closeVDisplayDevice();
  }

  DRIVER_STATUS openVDisplayDevice() {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::openVDisplayDevice() : VDISPLAY_SUDOVDA::openVDisplayDevice();
  }

  bool ensure_driver_is_ready() {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::ensure_driver_is_ready() : VDISPLAY_SUDOVDA::ensure_driver_is_ready();
  }

  bool startPingThread(std::function<void()> failCb) {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::startPingThread(std::move(failCb)) : VDISPLAY_SUDOVDA::startPingThread(std::move(failCb));
  }

  void setWatchdogFeedingEnabled(bool enable) {
    if (use_sunshine_driver()) {
      VDISPLAY_SUNSHINE::setWatchdogFeedingEnabled(enable);
    } else {
      VDISPLAY_SUDOVDA::setWatchdogFeedingEnabled(enable);
    }
  }

  bool setRenderAdapterByName(const std::wstring &adapterName) {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::setRenderAdapterByName(adapterName) : VDISPLAY_SUDOVDA::setRenderAdapterByName(adapterName);
  }

  bool setRenderAdapterWithMostDedicatedMemory() {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::setRenderAdapterWithMostDedicatedMemory() : VDISPLAY_SUDOVDA::setRenderAdapterWithMostDedicatedMemory();
  }

  void ensureVirtualDisplayRegistryDefaults() {
    if (use_sunshine_driver()) {
      VDISPLAY_SUNSHINE::ensureVirtualDisplayRegistryDefaults();
    } else {
      VDISPLAY_SUDOVDA::ensureVirtualDisplayRegistryDefaults();
    }
  }

  std::optional<VirtualDisplayCreationResult> createVirtualDisplay(
    const char *s_client_uid,
    const char *s_client_name,
    const char *s_hdr_profile,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    const GUID &guid,
    uint32_t base_fps_millihz,
    bool framegen_refresh_active,
    int framegen_refresh_multiplier,
    bool hdr_requested,
    bool allow_pending_enumeration,
    bool replace_existing
  ) {
    if (use_sunshine_driver()) {
      return VDISPLAY_SUNSHINE::createVirtualDisplay(s_client_uid, s_client_name, s_hdr_profile, width, height, fps, guid, base_fps_millihz, framegen_refresh_active, framegen_refresh_multiplier, hdr_requested, allow_pending_enumeration, replace_existing);
    }
    return VDISPLAY_SUDOVDA::createVirtualDisplay(s_client_uid, s_client_name, s_hdr_profile, width, height, fps, guid, base_fps_millihz, framegen_refresh_active, framegen_refresh_multiplier, hdr_requested, replace_existing);
  }

  void applyHdrProfileToOutput(const char *s_client_name, const char *s_hdr_profile, const char *s_device_id) {
    if (use_sunshine_driver()) {
      VDISPLAY_SUNSHINE::applyHdrProfileToOutput(s_client_name, s_hdr_profile, s_device_id);
    } else {
      VDISPLAY_SUDOVDA::applyHdrProfileToOutput(s_client_name, s_hdr_profile, s_device_id);
    }
  }

  void restorePhysicalHdrProfiles() {
    VDISPLAY_SUNSHINE::restorePhysicalHdrProfiles();
    VDISPLAY_SUDOVDA::restorePhysicalHdrProfiles();
  }

  bool removeVirtualDisplay(const GUID &guid) {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::removeVirtualDisplay(guid) : VDISPLAY_SUDOVDA::removeVirtualDisplay(guid);
  }

  bool removeAllVirtualDisplays() {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::removeAllVirtualDisplays() : VDISPLAY_SUDOVDA::removeAllVirtualDisplays();
  }

  void schedule_virtual_display_recovery_monitor(const VirtualDisplayRecoveryParams &params) {
    if (use_sunshine_driver()) {
      VDISPLAY_SUNSHINE::schedule_virtual_display_recovery_monitor(params);
    } else {
      VDISPLAY_SUDOVDA::schedule_virtual_display_recovery_monitor(params);
    }
  }

  bool is_virtual_display_guid_tracked(const GUID &guid) {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::is_virtual_display_guid_tracked(guid) : VDISPLAY_SUDOVDA::is_virtual_display_guid_tracked(guid);
  }

  std::optional<std::string> resolveVirtualDisplayDeviceId(const std::wstring &display_name) {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::resolveVirtualDisplayDeviceId(display_name) : VDISPLAY_SUDOVDA::resolveVirtualDisplayDeviceId(display_name);
  }

  std::optional<std::string> resolveVirtualDisplayDeviceIdForClient(const std::string &client_name) {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::resolveVirtualDisplayDeviceIdForClient(client_name) : VDISPLAY_SUDOVDA::resolveVirtualDisplayDeviceIdForClient(client_name);
  }

  std::optional<std::string> resolveActiveVirtualDisplayDeviceId(const std::string &preferred_output_identifier, const std::string &client_name, bool allow_any_fallback) {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::resolveActiveVirtualDisplayDeviceId(preferred_output_identifier, client_name, allow_any_fallback) : VDISPLAY_SUDOVDA::resolveActiveVirtualDisplayDeviceId(preferred_output_identifier, client_name, allow_any_fallback);
  }

  std::optional<std::string> resolveActiveVirtualDisplayDeviceIdForStableId(
    const std::string &stable_id,
    const std::string &preferred_output_identifier,
    const std::string &client_name,
    bool allow_any_fallback
  ) {
    if (use_sunshine_driver()) {
      return VDISPLAY_SUNSHINE::resolveActiveVirtualDisplayDeviceIdForStableId(stable_id, preferred_output_identifier, client_name, allow_any_fallback);
    }
    return VDISPLAY_SUDOVDA::resolveActiveVirtualDisplayDeviceId(preferred_output_identifier, client_name, allow_any_fallback);
  }

  std::optional<std::string> resolveAnyVirtualDisplayDeviceId() {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::resolveAnyVirtualDisplayDeviceId() : VDISPLAY_SUDOVDA::resolveAnyVirtualDisplayDeviceId();
  }

  bool is_virtual_display_output(const std::string &output_identifier) {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::is_virtual_display_output(output_identifier) : VDISPLAY_SUDOVDA::is_virtual_display_output(output_identifier);
  }

  bool is_virtual_display_selection(const std::string &output_identifier) {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::is_virtual_display_selection(output_identifier) : VDISPLAY_SUDOVDA::is_virtual_display_selection(output_identifier);
  }

  uint64_t client_uuid_to_virtual_display_id(const GUID &client_guid) {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::client_uuid_to_virtual_display_id(client_guid) : VDISPLAY_SUDOVDA::client_uuid_to_vdd_display_id(client_guid);
  }

  uuid_util::uuid_t virtualDisplayUuidFromStableId(const std::string &stable_id) {
    return VDISPLAY_SUNSHINE::virtualDisplayUuidFromStableId(stable_id);
  }

  GUID sharedVirtualDisplayGuid() {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::sharedVirtualDisplayGuid() : VDISPLAY_SUDOVDA::sharedVirtualDisplayGuid();
  }

  bool is_sunshine_virtual_display_identity(
    const std::string &device_path,
    const std::string &friendly_name,
    const std::string &edid_manufacturer_id,
    const std::string &edid_product_code
  ) {
    return VDISPLAY_SUNSHINE::is_sunshine_virtual_display_identity(device_path, friendly_name, edid_manufacturer_id, edid_product_code);
  }

  std::vector<std::wstring> matchDisplay(std::wstring sMatch) {
    const auto displays = enumerateVirtualDisplays();
    std::vector<std::wstring> matches;
    for (const auto &display : displays) {
      if (display.device_name.find(sMatch) != std::wstring::npos) {
        matches.push_back(display.device_name);
      } else if (display.friendly_name.find(sMatch) != std::wstring::npos) {
        matches.push_back(display.friendly_name);
      }
    }
    return matches;
  }

  bool isVirtualDisplayDriverInstalled() {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::isVirtualDisplayDriverInstalled() : VDISPLAY_SUDOVDA::isSudaVDADriverInstalled();
  }

  std::vector<VirtualDisplayInfo> enumerateVirtualDisplays() {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::enumerateVirtualDisplays() : VDISPLAY_SUDOVDA::enumerateSudaVDADisplays();
  }

  uuid_util::uuid_t persistentVirtualDisplayUuid() {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::persistentVirtualDisplayUuid() : VDISPLAY_SUDOVDA::persistentVirtualDisplayUuid();
  }

  bool has_active_physical_display() {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::has_active_physical_display() : VDISPLAY_SUDOVDA::has_active_physical_display();
  }

  bool should_auto_enable_virtual_display() {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::should_auto_enable_virtual_display() : VDISPLAY_SUDOVDA::should_auto_enable_virtual_display();
  }

  ensure_display_result ensure_display() {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::ensure_display() : VDISPLAY_SUDOVDA::ensure_display();
  }

  void cleanup_ensure_display(const ensure_display_result &result, bool probe_succeeded, bool allow_temporary_teardown) {
    if (use_sunshine_driver()) {
      VDISPLAY_SUNSHINE::cleanup_ensure_display(result, probe_succeeded, allow_temporary_teardown);
    } else {
      VDISPLAY_SUDOVDA::cleanup_ensure_display(result, probe_succeeded, allow_temporary_teardown);
    }
  }

  bool has_retained_ensure_display() {
    return use_sunshine_driver() ? VDISPLAY_SUNSHINE::has_retained_ensure_display() : VDISPLAY_SUDOVDA::has_retained_ensure_display();
  }
}  // namespace VDISPLAY
