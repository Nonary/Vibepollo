#include "virtual_display.h"

#include "src/config.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <icm.h>
#include <limits>
#include <vector>

#ifndef CPST_EXTENDED_DISPLAY_COLOR_MODE
  // MinGW headers may not expose the extended display color mode constant.
  #define CPST_EXTENDED_DISPLAY_COLOR_MODE 8
#endif

namespace {
  struct advanced_color_target_t {
    LUID target_adapter_id {};
    LUID source_adapter_id {};
    UINT32 source_id {};
  };

  struct advanced_color_api_t {
    using add_fn_t = HRESULT(WINAPI *)(WCS_PROFILE_MANAGEMENT_SCOPE, PCWSTR, LUID, UINT32, BOOL, BOOL);
    using remove_fn_t = HRESULT(WINAPI *)(WCS_PROFILE_MANAGEMENT_SCOPE, PCWSTR, LUID, UINT32, BOOL);
    using set_default_fn_t = HRESULT(WINAPI *)(WCS_PROFILE_MANAGEMENT_SCOPE, PCWSTR, COLORPROFILETYPE, COLORPROFILESUBTYPE, LUID, UINT32);
    using get_default_fn_t = HRESULT(WINAPI *)(WCS_PROFILE_MANAGEMENT_SCOPE, LUID, UINT32, COLORPROFILETYPE, COLORPROFILESUBTYPE, LPWSTR *);

    add_fn_t add {};
    remove_fn_t remove {};
    set_default_fn_t set_default {};
    get_default_fn_t get_default {};

    bool available() const {
      return add && remove && set_default && get_default;
    }
  };

  const advanced_color_api_t &advanced_color_api() {
    static const advanced_color_api_t api = []() {
      advanced_color_api_t result;
      const HMODULE module = LoadLibraryExW(L"Mscms.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (!module) {
        return result;
      }
      result.add = reinterpret_cast<advanced_color_api_t::add_fn_t>(GetProcAddress(module, "ColorProfileAddDisplayAssociation"));
      result.remove = reinterpret_cast<advanced_color_api_t::remove_fn_t>(GetProcAddress(module, "ColorProfileRemoveDisplayAssociation"));
      result.set_default = reinterpret_cast<advanced_color_api_t::set_default_fn_t>(GetProcAddress(module, "ColorProfileSetDisplayDefaultAssociation"));
      result.get_default = reinterpret_cast<advanced_color_api_t::get_default_fn_t>(GetProcAddress(module, "ColorProfileGetDisplayDefault"));
      return result;
    }();
    return api;
  }

  std::optional<advanced_color_target_t> advanced_color_target_for_monitor(const std::wstring &monitor_device_path) {
    if (monitor_device_path.empty()) {
      return std::nullopt;
    }

    constexpr UINT32 flags = QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE;
    for (int attempt = 0; attempt < 3; ++attempt) {
      UINT32 path_count = 0;
      UINT32 mode_count = 0;
      const LONG size_status = GetDisplayConfigBufferSizes(flags, &path_count, &mode_count);
      if (size_status != ERROR_SUCCESS) {
        return std::nullopt;
      }

      std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
      std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
      const LONG query_status = QueryDisplayConfig(
        flags,
        &path_count,
        path_count ? paths.data() : nullptr,
        &mode_count,
        mode_count ? modes.data() : nullptr,
        nullptr
      );
      if (query_status == ERROR_INSUFFICIENT_BUFFER) {
        continue;
      }
      if (query_status != ERROR_SUCCESS) {
        return std::nullopt;
      }

      paths.resize(path_count);
      for (const auto &path : paths) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME target_name {};
        target_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target_name.header.size = sizeof(target_name);
        target_name.header.adapterId = path.targetInfo.adapterId;
        target_name.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target_name.header) != ERROR_SUCCESS ||
            target_name.monitorDevicePath[0] == L'\0') {
          continue;
        }
        if (_wcsicmp(target_name.monitorDevicePath, monitor_device_path.c_str()) == 0) {
          return advanced_color_target_t {
            path.targetInfo.adapterId,
            path.sourceInfo.adapterId,
            path.sourceInfo.id
          };
        }
      }
      if (attempt + 1 < 3) {
        Sleep(100);
      }
    }
    return std::nullopt;
  }

  WCS_PROFILE_MANAGEMENT_SCOPE profile_scope(const bool system_wide) {
    return system_wide ? WCS_PROFILE_MANAGEMENT_SCOPE_SYSTEM_WIDE : WCS_PROFILE_MANAGEMENT_SCOPE_CURRENT_USER;
  }

  constexpr std::array<std::uint32_t, 12> kWindowsScalePercentages {
    100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500
  };

  struct sunshine_displayconfig_get_dpi_scale_t {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header {};
    std::int32_t min_scale_relative {};
    std::int32_t current_scale_relative {};
    std::int32_t max_scale_relative {};
  };

  struct sunshine_displayconfig_set_dpi_scale_t {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header {};
    std::int32_t scale_relative {};
  };

  constexpr auto kDisplayConfigGetDpiScale = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(-3);
  constexpr auto kDisplayConfigSetDpiScale = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(-4);

  void initialize_dpi_header(
    DISPLAYCONFIG_DEVICE_INFO_HEADER &header,
    const advanced_color_target_t &target,
    const DISPLAYCONFIG_DEVICE_INFO_TYPE type,
    const std::uint32_t size
  ) {
    header.type = type;
    header.size = size;
    header.adapterId = target.source_adapter_id;
    header.id = target.source_id;
  }

  std::optional<std::size_t> scale_index(const std::int32_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= kWindowsScalePercentages.size()) {
      return std::nullopt;
    }
    return static_cast<std::size_t>(index);
  }

  std::optional<std::wstring> utf8_to_wide(const std::string_view value) {
    if (value.empty() || value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
      return std::nullopt;
    }
    const int input_size = static_cast<int>(value.size());
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), input_size, nullptr, 0);
    if (required <= 0) {
      return std::nullopt;
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), input_size, result.data(), required) != required) {
      return std::nullopt;
    }
    return result;
  }

  std::optional<std::filesystem::path> hdr_profile_path(const std::string_view selection) {
    const auto selection_w = utf8_to_wide(selection);
    if (!selection_w) {
      return std::nullopt;
    }
    const auto filename = std::filesystem::path(*selection_w).filename().wstring();
    if (filename.empty()) {
      return std::nullopt;
    }

    std::array<wchar_t, MAX_PATH> system_directory {};
    const UINT length = GetSystemDirectoryW(system_directory.data(), static_cast<UINT>(system_directory.size()));
    if (length == 0 || length >= system_directory.size()) {
      return std::nullopt;
    }
    const auto color_directory = std::filesystem::path(system_directory.data()) / L"spool" / L"drivers" / L"color";
    std::vector<std::wstring> candidates {filename};
    if (!std::filesystem::path(filename).has_extension()) {
      candidates.push_back(filename + L".icm");
      candidates.push_back(filename + L".icc");
    }
    for (const auto &candidate_name : candidates) {
      const auto candidate = color_directory / candidate_name;
      std::error_code ec;
      if (std::filesystem::is_regular_file(candidate, ec)) {
        return candidate;
      }
    }
    return std::nullopt;
  }

  std::uint32_t read_be_u32(const std::vector<std::uint8_t> &bytes, const std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
  }
}  // namespace

namespace VDISPLAY {
  HANDLE VIRTUAL_DISPLAY_DRIVER_HANDLE = INVALID_HANDLE_VALUE;

  std::optional<std::wstring> get_advanced_color_profile(
    const std::wstring &monitor_device_path,
    const bool system_wide
  ) {
    const auto target = advanced_color_target_for_monitor(monitor_device_path);
    const auto &api = advanced_color_api();
    if (!target || !api.available()) {
      return std::nullopt;
    }

    LPWSTR profile_name = nullptr;
    const HRESULT status = api.get_default(
      profile_scope(system_wide),
      target->target_adapter_id,
      target->source_id,
      CPT_ICC,
      static_cast<COLORPROFILESUBTYPE>(CPST_EXTENDED_DISPLAY_COLOR_MODE),
      &profile_name
    );
    if (FAILED(status) || !profile_name || profile_name[0] == L'\0') {
      if (profile_name) {
        LocalFree(profile_name);
      }
      return std::nullopt;
    }

    std::wstring result {profile_name};
    LocalFree(profile_name);
    return result;
  }

  advanced_color_profile_result_t set_advanced_color_profile(
    const std::wstring &monitor_device_path,
    const std::wstring &profile_name,
    const bool system_wide
  ) {
    advanced_color_profile_result_t result;
    const auto &api = advanced_color_api();
    result.api_available = api.available();
    const auto target = advanced_color_target_for_monitor(monitor_device_path);
    result.target_found = target.has_value();
    if (!target || profile_name.empty() || !result.api_available) {
      return result;
    }

    result.attempted = true;
    const auto scope = profile_scope(system_wide);
    result.association_status = api.add(
      scope,
      profile_name.c_str(),
      target->target_adapter_id,
      target->source_id,
      TRUE,
      TRUE
    );
    result.default_status = api.set_default(
      scope,
      profile_name.c_str(),
      CPT_ICC,
      static_cast<COLORPROFILESUBTYPE>(CPST_EXTENDED_DISPLAY_COLOR_MODE),
      target->target_adapter_id,
      target->source_id
    );
    result.success = SUCCEEDED(result.association_status) || SUCCEEDED(result.default_status);
    return result;
  }

  advanced_color_profile_result_t remove_advanced_color_profile(
    const std::wstring &monitor_device_path,
    const std::wstring &profile_name,
    const bool system_wide
  ) {
    advanced_color_profile_result_t result;
    const auto &api = advanced_color_api();
    result.api_available = api.available();
    const auto target = advanced_color_target_for_monitor(monitor_device_path);
    result.target_found = target.has_value();
    if (!target || profile_name.empty() || !result.api_available) {
      return result;
    }

    result.attempted = true;
    result.association_status = api.remove(
      profile_scope(system_wide),
      profile_name.c_str(),
      target->target_adapter_id,
      target->source_id,
      TRUE
    );
    result.success = SUCCEEDED(result.association_status);
    return result;
  }

  display_scale_result_t set_display_scale_percent(
    const std::wstring &monitor_device_path,
    const std::uint32_t scale_percent
  ) {
    display_scale_result_t result;
    result.requested_percent = scale_percent;

    const auto desired = std::ranges::find(kWindowsScalePercentages, scale_percent);
    if (desired == kWindowsScalePercentages.end()) {
      result.status = ERROR_INVALID_PARAMETER;
      return result;
    }
    const auto desired_index = static_cast<std::int32_t>(std::distance(kWindowsScalePercentages.begin(), desired));

    const auto target = advanced_color_target_for_monitor(monitor_device_path);
    result.target_found = target.has_value();
    if (!target) {
      result.status = ERROR_NOT_FOUND;
      return result;
    }

    sunshine_displayconfig_get_dpi_scale_t get {};
    initialize_dpi_header(get.header, *target, kDisplayConfigGetDpiScale, sizeof(get));
    result.status = DisplayConfigGetDeviceInfo(&get.header);
    if (result.status != ERROR_SUCCESS) {
      return result;
    }
    result.queried = true;

    const auto recommended_index = -get.min_scale_relative;
    const auto current_index = recommended_index + get.current_scale_relative;
    const auto recommended = scale_index(recommended_index);
    const auto current = scale_index(current_index);
    if (!recommended || !current) {
      result.status = ERROR_INVALID_DATA;
      return result;
    }
    result.recommended_percent = kWindowsScalePercentages[*recommended];
    result.previous_percent = kWindowsScalePercentages[*current];
    result.current_percent = result.previous_percent;

    const auto desired_relative = desired_index - recommended_index;
    if (desired_relative < get.min_scale_relative || desired_relative > get.max_scale_relative) {
      result.status = ERROR_NOT_SUPPORTED;
      return result;
    }
    if (current_index == desired_index) {
      result.applied = true;
      result.status = ERROR_SUCCESS;
      return result;
    }

    sunshine_displayconfig_set_dpi_scale_t set {};
    initialize_dpi_header(set.header, *target, kDisplayConfigSetDpiScale, sizeof(set));
    set.scale_relative = desired_relative;
    result.status = DisplayConfigSetDeviceInfo(&set.header);
    if (result.status != ERROR_SUCCESS) {
      return result;
    }

    result.applied = true;
    result.current_percent = scale_percent;
    for (int attempt = 0; attempt < 3; ++attempt) {
      Sleep(50);
      get = {};
      initialize_dpi_header(get.header, *target, kDisplayConfigGetDpiScale, sizeof(get));
      if (DisplayConfigGetDeviceInfo(&get.header) != ERROR_SUCCESS) {
        continue;
      }
      const auto verified_index = scale_index(-get.min_scale_relative + get.current_scale_relative);
      if (verified_index) {
        result.current_percent = kWindowsScalePercentages[*verified_index];
      }
      if (result.current_percent == scale_percent) {
        break;
      }
    }
    return result;
  }

  std::optional<std::uint32_t> hdr_profile_peak_luminance_nits(const std::string_view selection) {
    const auto profile_path = hdr_profile_path(selection);
    if (!profile_path) {
      return std::nullopt;
    }

    std::ifstream input(*profile_path, std::ios::binary | std::ios::ate);
    if (!input) {
      return std::nullopt;
    }
    const auto end = input.tellg();
    constexpr std::streamoff kMaxProfileBytes = 32 * 1024 * 1024;
    if (end < 132 || end > kMaxProfileBytes) {
      return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
      return std::nullopt;
    }

    const auto tag_count = read_be_u32(bytes, 128);
    if (tag_count > (bytes.size() - 132) / 12) {
      return std::nullopt;
    }
    for (std::uint32_t index = 0; index < tag_count; ++index) {
      const auto entry = static_cast<std::size_t>(132 + index * 12);
      if (bytes[entry] != 'M' || bytes[entry + 1] != 'H' || bytes[entry + 2] != 'C' || bytes[entry + 3] != '2') {
        continue;
      }
      const auto tag_offset = static_cast<std::size_t>(read_be_u32(bytes, entry + 4));
      const auto tag_size = static_cast<std::size_t>(read_be_u32(bytes, entry + 8));
      if (tag_size < 20 || tag_offset > bytes.size() || tag_size > bytes.size() - tag_offset) {
        return std::nullopt;
      }
      if (bytes[tag_offset] != 'M' || bytes[tag_offset + 1] != 'H' ||
          bytes[tag_offset + 2] != 'C' || bytes[tag_offset + 3] != '2') {
        return std::nullopt;
      }

      const auto peak_raw = read_be_u32(bytes, tag_offset + 16);
      std::int32_t peak_fixed = 0;
      std::memcpy(&peak_fixed, &peak_raw, sizeof(peak_fixed));
      const auto peak_nits = static_cast<double>(peak_fixed) / 65536.0;
      if (!std::isfinite(peak_nits) || peak_nits <= 0.0 || peak_nits > 100000.0) {
        return std::nullopt;
      }
      return static_cast<std::uint32_t>(std::lround(peak_nits));
    }
    return std::nullopt;
  }
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
