/**
 * @file src/platform/windows/rtx_hdr_profile.cpp
 */

#include "rtx_hdr_profile.h"

#include "nvapi_driver_settings.h"
#include "utf_utils.h"

#include "src/config.h"
#include "src/logging.h"
#include "src/utility.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <string>
#include <unordered_set>

namespace platf::rtx_hdr {
  namespace {
    constexpr NvU32 RTX_HDR_ENABLE_ID = 0x00DD48FB;
    constexpr NvU32 RTX_HDR_PEAK_ID = 0x00DD48FC;
    constexpr NvU32 RTX_HDR_MIDDLE_GRAY_ID = 0x00DD48FD;
    constexpr NvU32 RTX_HDR_CONTRAST_ID = 0x00DD48FE;
    constexpr NvU32 RTX_HDR_SATURATION_ID = 0x00DD48FF;

    std::mutex g_invalid_log_mutex;
    std::unordered_set<std::string> g_invalid_logged;

    void log_invalid_once(const std::string &profile_name, NvU32 setting_id, NvU32 raw_value, const char *reason) {
      const auto key = profile_name + ":" + std::to_string(setting_id) + ":" + reason;
      {
        std::scoped_lock lk(g_invalid_log_mutex);
        if (!g_invalid_logged.insert(key).second) {
          return;
        }
      }

      BOOST_LOG(warning) << "RTX HDR: ignoring invalid NVIDIA profile setting 0x"
                         << std::hex << setting_id << std::dec
                         << " value " << raw_value << " in profile '" << profile_name
                         << "': " << reason;
    }

    void fill_nvapi_string(NvAPI_UnicodeString &dest, const std::wstring &src) {
      static_assert(sizeof(NvU16) == sizeof(wchar_t));
      std::memset(dest, 0, sizeof(NvAPI_UnicodeString));
      const auto count = std::min<std::size_t>(src.size(), NVAPI_UNICODE_STRING_MAX - 1);
      std::memcpy(dest, src.c_str(), count * sizeof(wchar_t));
    }

    std::string nvapi_string_to_utf8(const NvAPI_UnicodeString &src) {
      static_assert(sizeof(NvU16) == sizeof(wchar_t));
      try {
        return utf_utils::to_utf8(reinterpret_cast<const wchar_t *>(src));
      } catch (...) {
        return {};
      }
    }

    std::wstring utf8_to_nvapi_path(const std::string &path) {
      auto wide = utf_utils::from_utf8(path);
      try {
        auto normalized = std::filesystem::path(wide).lexically_normal();
        return normalized.generic_wstring();
      } catch (...) {
        std::replace(wide.begin(), wide.end(), L'\\', L'/');
        return wide;
      }
    }

    std::wstring utf8_to_basename(const std::string &path) {
      auto wide = utf_utils::from_utf8(path);
      try {
        return std::filesystem::path(wide).filename().wstring();
      } catch (...) {
        const auto pos = wide.find_last_of(L"\\/");
        return pos == std::wstring::npos ? wide : wide.substr(pos + 1);
      }
    }

    std::optional<NvU32> get_dword_setting(
      NvDRSSessionHandle session,
      NvDRSProfileHandle profile,
      NvU32 setting_id,
      const std::string &profile_name,
      bool application_profile
    ) {
      NVDRS_SETTING setting = {};
      setting.version = NVDRS_SETTING_VER;
      const auto status = NvAPI_DRS_GetSetting(session, profile, setting_id, &setting);
      if (status == NVAPI_SETTING_NOT_FOUND) {
        return std::nullopt;
      }
      if (status != NVAPI_OK) {
        return std::nullopt;
      }
      if (setting.settingType != NVDRS_DWORD_TYPE) {
        return std::nullopt;
      }

      const bool profile_value_visible =
        setting.settingLocation == NVDRS_CURRENT_PROFILE_LOCATION ||
        (application_profile &&
          setting.settingLocation == NVDRS_BASE_PROFILE_LOCATION) ||
        (!application_profile &&
          setting.settingLocation == NVDRS_GLOBAL_PROFILE_LOCATION);
      if (!profile_value_visible) {
        return std::nullopt;
      }
      return setting.u32CurrentValue;
    }

    std::optional<bool> read_bool_setting(NvDRSSessionHandle session, NvDRSProfileHandle profile, NvU32 setting_id, const std::string &profile_name, bool application_profile) {
      auto raw = get_dword_setting(session, profile, setting_id, profile_name, application_profile);
      if (!raw) {
        return std::nullopt;
      }
      if (*raw == 0) {
        return false;
      }
      if (*raw == 1) {
        return true;
      }
      log_invalid_once(profile_name, setting_id, *raw, "expected 0 or 1");
      return std::nullopt;
    }

    std::optional<int> read_int_range_setting(NvDRSSessionHandle session, NvDRSProfileHandle profile, NvU32 setting_id, int min_value, int max_value, const std::string &profile_name, bool application_profile) {
      auto raw = get_dword_setting(session, profile, setting_id, profile_name, application_profile);
      if (!raw) {
        return std::nullopt;
      }

      const auto signed_value = static_cast<int32_t>(*raw);
      if (signed_value < min_value || signed_value > max_value) {
        log_invalid_once(profile_name, setting_id, *raw, "outside expected range");
        return std::nullopt;
      }
      return signed_value;
    }

    std::optional<int> read_sdk_percent_setting(NvDRSSessionHandle session, NvDRSProfileHandle profile, NvU32 setting_id, const std::string &profile_name, bool application_profile) {
      auto raw = get_dword_setting(session, profile, setting_id, profile_name, application_profile);
      if (!raw) {
        return std::nullopt;
      }

      const auto signed_value = static_cast<int32_t>(*raw);
      if (*raw <= 200) {
        return static_cast<int>(*raw);
      }
      if (signed_value >= -100 && signed_value < 0) {
        return signed_value + 100;
      }

      log_invalid_once(profile_name, setting_id, *raw, "outside expected percentage range");
      return std::nullopt;
    }

    profile_values_t read_profile_values(NvDRSSessionHandle session, NvDRSProfileHandle profile, const std::string &profile_name, bool application_profile) {
      profile_values_t values;
      values.enabled = read_bool_setting(session, profile, RTX_HDR_ENABLE_ID, profile_name, application_profile);
      values.peak_brightness = read_int_range_setting(session, profile, RTX_HDR_PEAK_ID, 400, 2000, profile_name, application_profile);
      values.middle_gray = read_int_range_setting(session, profile, RTX_HDR_MIDDLE_GRAY_ID, 10, 100, profile_name, application_profile);
      values.contrast = read_sdk_percent_setting(session, profile, RTX_HDR_CONTRAST_ID, profile_name, application_profile);
      values.saturation = read_sdk_percent_setting(session, profile, RTX_HDR_SATURATION_ID, profile_name, application_profile);
      return values;
    }

    std::optional<std::pair<NvDRSProfileHandle, std::string>> find_application_profile(NvDRSSessionHandle session, const std::wstring &app_name) {
      NvAPI_UnicodeString nv_app_name = {};
      fill_nvapi_string(nv_app_name, app_name);

      NVDRS_APPLICATION application = {};
      application.version = NVDRS_APPLICATION_VER;
      NvDRSProfileHandle profile = nullptr;
      const auto status = NvAPI_DRS_FindApplicationByName(session, nv_app_name, &profile, &application);
      if (status != NVAPI_OK || !profile) {
        return std::nullopt;
      }

      NVDRS_PROFILE profile_info = {};
      profile_info.version = NVDRS_PROFILE_VER;
      std::string profile_name;
      if (NvAPI_DRS_GetProfileInfo(session, profile, &profile_info) == NVAPI_OK) {
        profile_name = nvapi_string_to_utf8(profile_info.profileName);
      }
      if (profile_name.empty()) {
        profile_name = "application";
      }

      return std::make_pair(profile, profile_name);
    }

    runtime_values_t config_runtime_values() {
      runtime_values_t values;
      values.enabled = config::video.rtx_hdr.enabled;
      values.contrast = std::clamp(config::video.rtx_hdr.contrast + 100, 0, 200);
      values.saturation = std::clamp(config::video.rtx_hdr.saturation + 100, 0, 200);
      values.middle_gray = config::video.rtx_hdr.middle_gray;
      values.peak_brightness = config::video.rtx_hdr.peak_brightness;
      values.source = profile_source_e::config;
      return values;
    }

    bool has_override(const char *key) {
      return config::has_runtime_config_override(key);
    }

    bool has_runtime_profile_override() {
      return has_override("rtx_hdr") ||
             has_override("rtx_hdr_contrast") ||
             has_override("rtx_hdr_saturation") ||
             has_override("rtx_hdr_middle_gray") ||
             has_override("rtx_hdr_peak_brightness");
    }

    runtime_values_t merge_runtime_values(const resolved_profile_t &resolved, const runtime_values_t &config_fallback) {
      runtime_values_t disabled;

      // The effective Sunshine config is the feature gate. A false value from global
      // config or runtime app/client overrides disables RTX HDR completely.
      if (!config_fallback.enabled) {
        disabled.source = profile_source_e::config;
        return disabled;
      }

      const bool runtime_profile_override = has_runtime_profile_override();

      if (!resolved.lookup_available) {
        return config_fallback;
      }
      if (!resolved.application.has_any() && !resolved.global.has_any()) {
        return config_fallback;
      }

      if (resolved.application.enabled == false && !runtime_profile_override) {
        disabled.source = profile_source_e::application;
        return disabled;
      }

      const bool enabled = resolved.application.enabled.value_or(resolved.global.enabled.value_or(config_fallback.enabled));
      if (!enabled) {
        disabled.source = resolved.application.enabled.has_value() ?
                            profile_source_e::application :
                            (resolved.global.enabled.has_value() ? profile_source_e::global : profile_source_e::config);
        return disabled;
      }

      runtime_values_t values;
      values.enabled = true;
      values.contrast = has_override("rtx_hdr_contrast") ?
                          config_fallback.contrast :
                          resolved.application.contrast.value_or(resolved.global.contrast.value_or(config_fallback.contrast));
      values.saturation = has_override("rtx_hdr_saturation") ?
                            config_fallback.saturation :
                            resolved.application.saturation.value_or(resolved.global.saturation.value_or(config_fallback.saturation));
      values.middle_gray = has_override("rtx_hdr_middle_gray") ?
                             config_fallback.middle_gray :
                             resolved.application.middle_gray.value_or(resolved.global.middle_gray.value_or(config_fallback.middle_gray));
      values.peak_brightness = has_override("rtx_hdr_peak_brightness") ?
                                 config_fallback.peak_brightness :
                                 resolved.application.peak_brightness.value_or(resolved.global.peak_brightness.value_or(config_fallback.peak_brightness));
      values.source = resolved.application.has_any() ?
                        profile_source_e::application :
                        (resolved.global.has_any() ? profile_source_e::global : profile_source_e::config);
      return values;
    }

  }  // namespace

  const char *source_name(profile_source_e source) {
    switch (source) {
      case profile_source_e::application:
        return "profile";
      case profile_source_e::global:
        return "global";
      case profile_source_e::config:
        return "config";
      case profile_source_e::none:
        break;
    }
    return "none";
  }

  runtime_values_t materialize_runtime_values_for_tests(
    const resolved_profile_t &resolved,
    const runtime_values_t &config_fallback
  ) {
    return merge_runtime_values(resolved, config_fallback);
  }

  resolved_profile_t resolve_profile_for_executable(const std::string &executable) {
    resolved_profile_t resolved;
    resolved.executable = executable;

    if (NvAPI_Initialize() != NVAPI_OK) {
      return resolved;
    }

    NvDRSSessionHandle session = nullptr;
    auto destroy_session = [&]() {
      if (session) {
        NvAPI_DRS_DestroySession(session);
        session = nullptr;
      }
      NvAPI_Unload();
    };

    if (NvAPI_DRS_CreateSession(&session) != NVAPI_OK || !session) {
      destroy_session();
      return resolved;
    }

    if (NvAPI_DRS_LoadSettings(session) != NVAPI_OK) {
      destroy_session();
      return resolved;
    }

    resolved.lookup_available = true;
    auto cleanup = util::fail_guard(destroy_session);

    if (!executable.empty()) {
      std::optional<std::pair<NvDRSProfileHandle, std::string>> app_profile;
      try {
        app_profile = find_application_profile(session, utf8_to_nvapi_path(executable));
        if (!app_profile) {
          app_profile = find_application_profile(session, utf8_to_basename(executable));
        }
      } catch (...) {
      }

      if (app_profile) {
        resolved.source = profile_source_e::application;
        resolved.profile_name = app_profile->second;
        resolved.application = read_profile_values(session, app_profile->first, app_profile->second, true);
      }
    }

    NvDRSProfileHandle base_profile = nullptr;
    if (NvAPI_DRS_GetBaseProfile(session, &base_profile) == NVAPI_OK && base_profile) {
      resolved.global = read_profile_values(session, base_profile, "global", false);
      if (resolved.source == profile_source_e::none && resolved.global.has_any()) {
        resolved.source = profile_source_e::global;
      }
    }

    destroy_session();
    cleanup.disable();
    return resolved;
  }

  std::optional<int> resolve_session_peak_brightness(const std::string &executable) {
    const auto resolved = resolve_profile_for_executable(executable);
    const auto runtime = merge_runtime_values(resolved, config_runtime_values());
    if (!runtime.enabled) {
      return std::nullopt;
    }
    return runtime.peak_brightness;
  }

}  // namespace platf::rtx_hdr
