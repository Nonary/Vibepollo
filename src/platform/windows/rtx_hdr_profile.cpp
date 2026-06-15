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
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <string>
#include <unordered_set>

namespace platf::rtx_hdr {
  namespace {
    // NVIDIA Profile Inspector exposes two DRS-resident RTX HDR activation signals:
    //   0x00432F84: "RTX HDR - Driver Flags"; any nonzero value forces driver-applied HDR.
    //   0x00DD48FB: "RTX HDR - Enable"; the NVIDIA App profile enable bit.
    //
    // The 0x00DD48FC..FF settings below are tuning dials. They provide values only and do not
    // activate RTX HDR unless one of the activation signals above is set.
    constexpr NvU32 RTX_HDR_DRIVER_FLAGS_ID = 0x00432F84;
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
      BOOST_LOG(debug) << "RTX HDR: GetSetting 0x" << std::hex << setting_id << std::dec
                       << " [" << profile_name << " app=" << (application_profile ? 1 : 0) << "]"
                       << " status=" << static_cast<int>(status)
                       << " type=" << static_cast<int>(setting.settingType)
                       << " loc=" << static_cast<int>(setting.settingLocation)
                       << " predef=" << (setting.isCurrentPredefined ? 1 : 0)
                       << " u32=" << setting.u32CurrentValue;
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

    std::optional<bool> decode_activation_state(std::optional<NvU32> driver_flags, std::optional<NvU32> profile_enable) {
      if (driver_flags && *driver_flags != 0) {
        return true;
      }
      if (profile_enable) {
        if (*profile_enable > 1) {
          return std::nullopt;
        }
        return *profile_enable != 0;
      }
      if (driver_flags) {
        return false;
      }
      return std::nullopt;
    }

    std::optional<bool> read_activation_setting(NvDRSSessionHandle session, NvDRSProfileHandle profile, const std::string &profile_name, bool application_profile) {
      const auto driver_flags = get_dword_setting(session, profile, RTX_HDR_DRIVER_FLAGS_ID, profile_name, application_profile);
      auto profile_enable = get_dword_setting(session, profile, RTX_HDR_ENABLE_ID, profile_name, application_profile);
      if (profile_enable && *profile_enable > 1) {
        log_invalid_once(profile_name, RTX_HDR_ENABLE_ID, *profile_enable, "enable outside 0..1");
        profile_enable = std::nullopt;
      }
      return decode_activation_state(driver_flags, profile_enable);
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

    // RTX HDR contrast and saturation are stored in SDK percent units (0..200, 100 = neutral).
    // Live NVIDIA App profiles commonly store saturation values above 100 (for example PoE2 uses
    // 151 and the global profile may use 200), so treat both dials symmetrically.
    std::optional<int> decode_percent_units(NvU32 raw) {
      if (raw <= 200) {
        return static_cast<int>(raw);
      }
      return std::nullopt;
    }

    std::optional<int> read_contrast_setting(NvDRSSessionHandle session, NvDRSProfileHandle profile, NvU32 setting_id, const std::string &profile_name, bool application_profile) {
      auto raw = get_dword_setting(session, profile, setting_id, profile_name, application_profile);
      if (!raw) {
        return std::nullopt;
      }
      auto decoded = decode_percent_units(*raw);
      if (!decoded) {
        log_invalid_once(profile_name, setting_id, *raw, "contrast outside 0..200");
      }
      return decoded;
    }

    std::optional<int> read_saturation_setting(NvDRSSessionHandle session, NvDRSProfileHandle profile, NvU32 setting_id, const std::string &profile_name, bool application_profile) {
      auto raw = get_dword_setting(session, profile, setting_id, profile_name, application_profile);
      if (!raw) {
        return std::nullopt;
      }
      auto decoded = decode_percent_units(*raw);
      if (!decoded) {
        log_invalid_once(profile_name, setting_id, *raw, "saturation outside 0..200");
      }
      return decoded;
    }

    profile_values_t read_profile_values(NvDRSSessionHandle session, NvDRSProfileHandle profile, const std::string &profile_name, bool application_profile) {
      profile_values_t values;
      values.enabled = read_activation_setting(session, profile, profile_name, application_profile);
      values.peak_brightness = read_int_range_setting(session, profile, RTX_HDR_PEAK_ID, 400, 2000, profile_name, application_profile);
      values.middle_gray = read_int_range_setting(session, profile, RTX_HDR_MIDDLE_GRAY_ID, 10, 100, profile_name, application_profile);
      values.contrast = read_contrast_setting(session, profile, RTX_HDR_CONTRAST_ID, profile_name, application_profile);
      values.saturation = read_saturation_setting(session, profile, RTX_HDR_SATURATION_ID, profile_name, application_profile);
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

    std::string fmt_opt_bool(const std::optional<bool> &v) {
      return v ? (*v ? "1" : "0") : "-";
    }

    std::string fmt_opt_int(const std::optional<int> &v) {
      return v ? std::to_string(*v) : "-";
    }

    bool has_override(const char *key) {
      return config::has_runtime_config_override(key);
    }

    bool has_runtime_enable_override() {
      return has_override("rtx_hdr");
    }

    bool has_runtime_tuning_override() {
      return has_override("rtx_hdr_contrast") ||
             has_override("rtx_hdr_saturation") ||
             has_override("rtx_hdr_middle_gray") ||
             has_override("rtx_hdr_peak_brightness");
    }

    bool has_tuning_values(const profile_values_t &values) {
      return values.contrast || values.saturation || values.middle_gray || values.peak_brightness;
    }

    profile_source_e tuning_source(const resolved_profile_t &resolved) {
      if (has_runtime_tuning_override()) {
        return profile_source_e::config;
      }
      if (has_tuning_values(resolved.application)) {
        return profile_source_e::application;
      }
      if (has_tuning_values(resolved.global)) {
        return profile_source_e::global;
      }
      return profile_source_e::config;
    }

    // Build the active tuning dials with the effective precedence:
    //   per-app Sunshine dial override > NVIDIA application profile > NVIDIA global profile >
    //   Sunshine config default.
    runtime_values_t active_values(
      const resolved_profile_t &resolved,
      const runtime_values_t &config_fallback
    ) {
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
      values.source = tuning_source(resolved);
      return values;
    }

    runtime_values_t merge_runtime_values(const resolved_profile_t &resolved, const runtime_values_t &config_fallback) {
      runtime_values_t disabled;

      // RTX HDR conversion is application opt-in only. NVIDIA profile enable bits are read for
      // compatibility diagnostics, but they never activate or block conversion.
      if (!has_runtime_enable_override()) {
        return disabled;
      }

      if (!config_fallback.enabled) {
        disabled.source = profile_source_e::config;
        return disabled;
      }

      return active_values(resolved, config_fallback);
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

  runtime_values_t materialize_live_tuning_values(
    const resolved_profile_t &resolved,
    const runtime_values_t &config_fallback,
    bool enabled
  ) {
    auto values = active_values(resolved, config_fallback);
    values.enabled = enabled;
    return values;
  }

  std::optional<bool> decode_rtx_hdr_activation_for_tests(std::optional<std::uint32_t> driver_flags, std::optional<std::uint32_t> profile_enable) {
    return decode_activation_state(driver_flags, profile_enable);
  }

  std::optional<int> decode_rtx_hdr_contrast_units_for_tests(std::uint32_t raw) {
    return decode_percent_units(raw);
  }

  std::optional<int> decode_rtx_hdr_saturation_units_for_tests(std::uint32_t raw) {
    return decode_percent_units(raw);
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

    BOOST_LOG(debug) << "RTX HDR: resolved '" << executable << "'"
                     << " lookup=" << (resolved.lookup_available ? "ok" : "fail")
                     << " app_profile='" << resolved.profile_name << "'"
                     << " app{enabled=" << fmt_opt_bool(resolved.application.enabled)
                     << " contrast=" << fmt_opt_int(resolved.application.contrast)
                     << " saturation=" << fmt_opt_int(resolved.application.saturation)
                     << " middle_gray=" << fmt_opt_int(resolved.application.middle_gray)
                     << " peak=" << fmt_opt_int(resolved.application.peak_brightness) << "}"
                     << " global{enabled=" << fmt_opt_bool(resolved.global.enabled)
                     << " contrast=" << fmt_opt_int(resolved.global.contrast)
                     << " saturation=" << fmt_opt_int(resolved.global.saturation)
                     << " peak=" << fmt_opt_int(resolved.global.peak_brightness) << "}";

    destroy_session();
    cleanup.disable();
    return resolved;
  }

  std::optional<int> resolve_session_peak_brightness(const std::string &executable) {
    // Runs on the capture/encode thread when a session is (re)created -- including when a second
    // HDR client joins a live stream. A full NvAPI DRS cycle (LoadSettings alone can exceed 100 ms)
    // here hitches the shared capture thread and any already-streaming client, so reuse a recent
    // result for the same app instead of re-querying the driver on every join.
    static std::mutex cache_mutex;
    static std::string cached_executable;
    static std::optional<int> cached_peak;
    static std::chrono::steady_clock::time_point cached_at {};
    static bool cache_valid = false;
    constexpr auto cache_ttl = std::chrono::seconds(10);

    const auto now = std::chrono::steady_clock::now();
    {
      std::scoped_lock lk(cache_mutex);
      if (cache_valid && cached_executable == executable && now - cached_at < cache_ttl) {
        return cached_peak;
      }
    }

    const auto resolved = resolve_profile_for_executable(executable);
    const auto runtime = merge_runtime_values(resolved, config_runtime_values());
    const std::optional<int> result = runtime.enabled ? std::optional<int> {runtime.peak_brightness} : std::nullopt;

    {
      std::scoped_lock lk(cache_mutex);
      cached_executable = executable;
      cached_peak = result;
      cached_at = now;
      cache_valid = true;
    }
    return result;
  }

}  // namespace platf::rtx_hdr
