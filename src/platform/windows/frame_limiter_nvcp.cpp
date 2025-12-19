/**
 * @file src/platform/windows/frame_limiter_nvcp.cpp
 * @brief NVIDIA Control Panel frame limiter provider implementation.
 */

#ifdef _WIN32

  #include "frame_limiter_nvcp.h"

  #include "nvapi_driver_settings.h"
  #include "src/logging.h"

  #include <algorithm>
  #include <cstdint>
  #include <filesystem>
  #include <fstream>
  #include <iomanip>
  #include <nlohmann/json.hpp>
  #include <optional>
  #include <sstream>
  #include <string>
  #include <system_error>
  #include <windows.h>

namespace platf::frame_limiter_nvcp {

  namespace {

    struct state_t {
      NvDRSSessionHandle session = nullptr;
      NvDRSProfileHandle profile = nullptr;
      bool profile_is_global = false;
      bool initialized = false;
      bool frame_limit_applied = false;
      bool vsync_applied = false;
      bool llm_applied = false;
      bool ull_cpl_applied = false;
      bool ull_enabled_applied = false;
      bool smooth_motion_applied = false;
      bool smooth_motion_mask_applied = false;
      bool smooth_motion_supported = false;
      NvU32 driver_version = 0;
      std::optional<NvU32> original_frame_limit;
      std::optional<NvU32> original_vsync;
      std::optional<NvU32> original_prerender_limit;
      std::optional<NvU32> original_ull_cpl_state;
      std::optional<NvU32> original_ull_enabled;
      std::optional<NvU32> original_ull_cpl_state_value;
      std::optional<NvU32> original_ull_enabled_value;
      bool original_ull_cpl_state_override = false;
      bool original_ull_enabled_override = false;
      std::optional<NvU32> original_smooth_motion;
      std::optional<NvU32> original_smooth_motion_mask;
      std::optional<NvU32> original_smooth_motion_value;
      std::optional<NvU32> original_smooth_motion_mask_value;
      bool original_smooth_motion_override = false;
      bool original_smooth_motion_mask_override = false;
    };

    state_t g_state;
    bool g_recovery_file_owned = false;

    constexpr NvU32 SMOOTH_MOTION_ENABLE_ID = 0xB0D384C0;
    constexpr NvU32 SMOOTH_MOTION_API_MASK_ID = 0xB0CC0875;
    constexpr NvU32 SMOOTH_MOTION_ON = 0x00000001;
    constexpr NvU32 SMOOTH_MOTION_API_MASK_VALUE = 0x00000007;
    constexpr NvU32 SMOOTH_MOTION_MIN_DRIVER_VERSION = 57186;
    constexpr NvU32 NVAPI_DRIVER_AND_BRANCH_VERSION_ID = 0x2926AAAD;
    constexpr NvU32 NVAPI_DRS_GET_CURRENT_GLOBAL_PROFILE_ID = 0x617bff9f;
    constexpr NvU32 ULTRA_LOW_LATENCY_CPL_STATE_ID = 0x0005F543;
    constexpr NvU32 ULTRA_LOW_LATENCY_ENABLED_ID = 0x10835000;
    constexpr NvU32 ULTRA_LOW_LATENCY_CPL_STATE_ULTRA = 0x00000002;
    constexpr NvU32 ULTRA_LOW_LATENCY_ENABLED_ON = 0x00000001;

    using query_interface_fn = void *(__cdecl *) (NvU32);
    using driver_version_fn = NvAPI_Status(__cdecl *)(NvU32 *, NvAPI_ShortString);
    using get_current_global_profile_fn = NvAPI_Status(__cdecl *)(NvDRSSessionHandle, NvDRSProfileHandle *);

    void log_nvapi_error(NvAPI_Status status, const char *label) {
      NvAPI_ShortString message = {};
      NvAPI_GetErrorMessage(status, message);
      BOOST_LOG(warning) << "NvAPI " << label << " failed: " << message;
    }

    std::string format_driver_version(NvU32 version) {
      if (version == 0) {
        return "unknown";
      }
      std::ostringstream out;
      NvU32 major = version / 100;
      NvU32 minor = version % 100;
      out << major << '.' << std::setw(2) << std::setfill('0') << minor;
      return out.str();
    }

    query_interface_fn resolve_query_interface() {
      static query_interface_fn cached = nullptr;
      static bool attempted = false;
      if (attempted) {
        return cached;
      }
      attempted = true;

      auto ensure_module = [](const wchar_t *name) -> HMODULE {
        HMODULE module = GetModuleHandleW(name);
        if (!module) {
          module = LoadLibraryW(name);
        }
        return module;
      };

      HMODULE module = ensure_module(L"nvapi64.dll");
      if (!module) {
        module = ensure_module(L"nvapi.dll");
      }
      if (!module) {
        return nullptr;
      }

      FARPROC symbol = GetProcAddress(module, "nvapi_QueryInterface");
      if (!symbol) {
        symbol = GetProcAddress(module, "NvAPI_QueryInterface");
      }
      if (!symbol) {
        return nullptr;
      }

      cached = reinterpret_cast<query_interface_fn>(symbol);
      return cached;
    }

    NvAPI_Status get_driver_and_branch_version(NvU32 *version, NvAPI_ShortString branch) {
      static driver_version_fn fn = nullptr;
      static bool attempted = false;
      if (!fn && !attempted) {
        attempted = true;
        auto query = resolve_query_interface();
        if (query) {
          fn = reinterpret_cast<driver_version_fn>(query(NVAPI_DRIVER_AND_BRANCH_VERSION_ID));
        }
      }
      if (!fn) {
        return NVAPI_NO_IMPLEMENTATION;
      }
      return fn(version, branch);
    }

    NvAPI_Status get_current_global_profile(NvDRSSessionHandle session, NvDRSProfileHandle *profile) {
      static get_current_global_profile_fn fn = nullptr;
      static bool attempted = false;
      if (!fn && !attempted) {
        attempted = true;
        auto query = resolve_query_interface();
        if (query) {
          fn = reinterpret_cast<get_current_global_profile_fn>(query(NVAPI_DRS_GET_CURRENT_GLOBAL_PROFILE_ID));
        }
      }
      if (!fn) {
        return NVAPI_NO_IMPLEMENTATION;
      }
      return fn(session, profile);
    }

    void cleanup() {
      if (g_state.session) {
        NvAPI_DRS_DestroySession(g_state.session);
        g_state.session = nullptr;
      }
      if (g_state.initialized) {
        NvAPI_Unload();
        g_state.initialized = false;
      }
      g_state.profile = nullptr;
      g_state.profile_is_global = false;
      g_state.frame_limit_applied = false;
      g_state.vsync_applied = false;
      g_state.llm_applied = false;
      g_state.ull_cpl_applied = false;
      g_state.ull_enabled_applied = false;
      g_state.smooth_motion_applied = false;
      g_state.smooth_motion_mask_applied = false;
      g_state.original_frame_limit.reset();
      g_state.original_vsync.reset();
      g_state.original_prerender_limit.reset();
      g_state.original_ull_cpl_state.reset();
      g_state.original_ull_enabled.reset();
      g_state.original_ull_cpl_state_value.reset();
      g_state.original_ull_enabled_value.reset();
      g_state.original_ull_cpl_state_override = false;
      g_state.original_ull_enabled_override = false;
      g_state.original_smooth_motion.reset();
      g_state.original_smooth_motion_mask.reset();
      g_state.original_smooth_motion_value.reset();
      g_state.original_smooth_motion_mask_value.reset();
      g_state.original_smooth_motion_override = false;
      g_state.original_smooth_motion_mask_override = false;
      g_state.smooth_motion_supported = false;
      g_state.driver_version = 0;
    }

    NvDRSProfileHandle resolve_profile(NvDRSSessionHandle session, bool *is_global);

    bool ensure_initialized() {
      if (g_state.initialized && g_state.session && g_state.profile) {
        return true;
      }

      cleanup();

      NvAPI_Status status = NvAPI_Initialize();
      if (status != NVAPI_OK) {
        log_nvapi_error(status, "Initialize");
        return false;
      }
      g_state.initialized = true;

      NvU32 driver_version = 0;
      NvAPI_ShortString branch = {};
      NvAPI_Status version_status = get_driver_and_branch_version(&driver_version, branch);
      if (version_status != NVAPI_OK) {
        log_nvapi_error(version_status, "SYS_GetDriverAndBranchVersion");
        g_state.driver_version = 0;
        g_state.smooth_motion_supported = false;
      } else {
        g_state.driver_version = driver_version;
        g_state.smooth_motion_supported = driver_version >= SMOOTH_MOTION_MIN_DRIVER_VERSION;
      }

      status = NvAPI_DRS_CreateSession(&g_state.session);
      if (status != NVAPI_OK) {
        log_nvapi_error(status, "DRS_CreateSession");
        cleanup();
        return false;
      }

      status = NvAPI_DRS_LoadSettings(g_state.session);
      if (status != NVAPI_OK) {
        log_nvapi_error(status, "DRS_LoadSettings");
        cleanup();
        return false;
      }

      g_state.profile = resolve_profile(g_state.session, &g_state.profile_is_global);
      if (!g_state.profile) {
        cleanup();
        return false;
      }

      return true;
    }

    NvAPI_Status get_current_setting(NvU32 setting_id, std::optional<NvU32> &storage, bool *had_override = nullptr, NvU32 *current_value = nullptr) {
      NVDRS_SETTING existing = {};
      existing.version = NVDRS_SETTING_VER;
      NvAPI_Status status = NvAPI_DRS_GetSetting(g_state.session, g_state.profile, setting_id, &existing);
      if (status == NVAPI_OK) {
        const bool setting_in_profile =
          existing.settingLocation == NVDRS_CURRENT_PROFILE_LOCATION ||
          (g_state.profile_is_global && existing.settingLocation == NVDRS_GLOBAL_PROFILE_LOCATION) ||
          (!g_state.profile_is_global && existing.settingLocation == NVDRS_BASE_PROFILE_LOCATION);
        if (had_override) {
          *had_override = setting_in_profile;
        }
        if (current_value) {
          *current_value = existing.u32CurrentValue;
        }
        if (setting_in_profile) {
          storage = existing.u32CurrentValue;
        } else {
          storage.reset();
        }
      } else if (status == NVAPI_SETTING_NOT_FOUND) {
        if (had_override) {
          *had_override = false;
        }
        if (current_value) {
          *current_value = 0;
        }
        storage.reset();
        status = NVAPI_OK;
      }
      if (status != NVAPI_OK && had_override) {
        *had_override = false;
      }
      return status;
    }

    NvDRSProfileHandle resolve_profile(NvDRSSessionHandle session, bool *is_global) {
      if (is_global) {
        *is_global = false;
      }

      NVDRS_SETTING probe = {};
      probe.version = NVDRS_SETTING_VER;
      NvDRSProfileHandle profile = nullptr;
      NvAPI_Status status = get_current_global_profile(session, &profile);
      if (status == NVAPI_OK && profile) {
        NvAPI_Status probe_status = NvAPI_DRS_GetSetting(session, profile, FRL_FPS_ID, &probe);
        if (probe_status == NVAPI_OK || probe_status == NVAPI_SETTING_NOT_FOUND) {
          if (is_global) {
            *is_global = true;
          }
          return profile;
        }
        log_nvapi_error(probe_status, "DRS_GetSetting(FRL_FPS global)");
      } else if (status != NVAPI_OK && status != NVAPI_NO_IMPLEMENTATION) {
        log_nvapi_error(status, "DRS_GetCurrentGlobalProfile");
      }

      status = NvAPI_DRS_GetBaseProfile(session, &profile);
      if (status != NVAPI_OK) {
        log_nvapi_error(status, "DRS_GetBaseProfile");
        return nullptr;
      }
      return profile;
    }

    struct restore_info_t {
      bool frame_limit_applied = false;
      std::optional<NvU32> frame_limit_value;
      bool vsync_applied = false;
      std::optional<NvU32> vsync_value;
      bool llm_applied = false;
      std::optional<NvU32> prerender_value;
      bool ull_cpl_applied = false;
      std::optional<NvU32> ull_cpl_value;
      std::optional<NvU32> ull_cpl_fallback_value;
      bool ull_cpl_override = false;
      bool ull_enabled_applied = false;
      std::optional<NvU32> ull_enabled_value;
      std::optional<NvU32> ull_enabled_fallback_value;
      bool ull_enabled_override = false;
      bool smooth_motion_applied = false;
      std::optional<NvU32> smooth_motion_value;
      std::optional<NvU32> smooth_motion_fallback_value;
      bool smooth_motion_override = false;
      bool smooth_motion_mask_applied = false;
      std::optional<NvU32> smooth_motion_mask_value;
      std::optional<NvU32> smooth_motion_mask_fallback_value;
      bool smooth_motion_mask_override = false;
    };

    bool restore_with_fresh_session(const restore_info_t &restore_data);

    std::optional<std::filesystem::path> overrides_dir_path() {
      static std::optional<std::filesystem::path> cached;
      if (cached.has_value()) {
        return cached;
      }

      wchar_t program_data_env[MAX_PATH] = {};
      DWORD len = GetEnvironmentVariableW(L"ProgramData", program_data_env, _countof(program_data_env));
      if (len == 0 || len >= _countof(program_data_env)) {
        return std::nullopt;
      }

      std::filesystem::path base(program_data_env);
      std::error_code ec;
      if (!std::filesystem::exists(base, ec)) {
        return std::nullopt;
      }

      cached = base / L"Sunshine";
      return cached;
    }

    std::optional<std::filesystem::path> overrides_file_path() {
      auto dir = overrides_dir_path();
      if (!dir) {
        return std::nullopt;
      }
      return *dir / L"nvcp_overrides.json";
    }

    bool write_overrides_file(const restore_info_t &info) {
      if (!info.frame_limit_applied && !info.vsync_applied && !info.llm_applied && !info.ull_cpl_applied && !info.ull_enabled_applied && !info.smooth_motion_applied && !info.smooth_motion_mask_applied) {
        return true;
      }

      auto file_path_opt = overrides_file_path();
      if (!file_path_opt) {
        BOOST_LOG(warning) << "NVIDIA Control Panel overrides: unable to resolve ProgramData path for crash recovery";
        return false;
      }

      const auto &file_path = *file_path_opt;
      std::error_code ec;
      if (auto dir = file_path.parent_path(); !dir.empty()) {
        if (!std::filesystem::exists(dir, ec)) {
          if (!std::filesystem::create_directories(dir, ec) && ec) {
            BOOST_LOG(warning) << "NVIDIA Control Panel overrides: failed to create recovery directory: " << ec.message();
            return false;
          }
        }
      }

      nlohmann::json j;
      auto encode_section = [&](const char *key, bool applied, bool had_override, const std::optional<NvU32> &value, const std::optional<NvU32> &fallback) {
        nlohmann::json node;
        node["applied"] = applied;
        node["had_override"] = had_override;
        if (value.has_value()) {
          node["value"] = *value;
        } else {
          node["value"] = nullptr;
        }
        if (fallback.has_value()) {
          node["fallback"] = *fallback;
        }
        j[key] = node;
      };

      encode_section("frame_limit", info.frame_limit_applied, info.frame_limit_value.has_value(), info.frame_limit_value, std::nullopt);
      encode_section("vsync", info.vsync_applied, info.vsync_value.has_value(), info.vsync_value, std::nullopt);
      encode_section("low_latency", info.llm_applied, info.prerender_value.has_value(), info.prerender_value, std::nullopt);
      encode_section("ultra_low_latency_cpl", info.ull_cpl_applied, info.ull_cpl_override, info.ull_cpl_value, info.ull_cpl_fallback_value);
      encode_section("ultra_low_latency_enabled", info.ull_enabled_applied, info.ull_enabled_override, info.ull_enabled_value, info.ull_enabled_fallback_value);
      encode_section("smooth_motion", info.smooth_motion_applied, info.smooth_motion_override, info.smooth_motion_value, info.smooth_motion_fallback_value);
      encode_section("smooth_motion_mask", info.smooth_motion_mask_applied, info.smooth_motion_mask_override, info.smooth_motion_mask_value, info.smooth_motion_mask_fallback_value);

      std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
      if (!out.is_open()) {
        BOOST_LOG(warning) << "NVIDIA Control Panel overrides: failed to open recovery file for write";
        return false;
      }

      try {
        out << j.dump();
        if (!out.good()) {
          BOOST_LOG(warning) << "NVIDIA Control Panel overrides: failed to write recovery file";
          return false;
        }
      } catch (const std::exception &ex) {
        BOOST_LOG(warning) << "NVIDIA Control Panel overrides: exception while writing recovery file: " << ex.what();
        return false;
      }

      return true;
    }

    std::optional<restore_info_t> read_overrides_file() {
      auto file_path_opt = overrides_file_path();
      if (!file_path_opt) {
        return std::nullopt;
      }

      std::error_code ec;
      if (!std::filesystem::exists(*file_path_opt, ec) || ec) {
        return std::nullopt;
      }

      std::ifstream in(*file_path_opt, std::ios::binary);
      if (!in.is_open()) {
        BOOST_LOG(warning) << "NVIDIA Control Panel overrides: unable to open recovery file for read";
        return std::nullopt;
      }

      nlohmann::json j;
      try {
        in >> j;
      } catch (const std::exception &ex) {
        BOOST_LOG(warning) << "NVIDIA Control Panel overrides: failed to parse recovery file: " << ex.what();
        return std::nullopt;
      }

      auto decode_section = [&](const char *key, bool &applied, bool &had_override, std::optional<NvU32> &value, std::optional<NvU32> &fallback) {
        applied = false;
        value.reset();
        fallback.reset();
        if (!j.contains(key)) {
          had_override = false;
          return;
        }
        const auto &node = j[key];
        if (!node.is_object()) {
          had_override = false;
          return;
        }
        applied = node.value("applied", false);
        bool had_override_present = node.contains("had_override");
        had_override = node.value("had_override", false);
        if (node.contains("value") && !node["value"].is_null()) {
          try {
            auto v = node["value"].get<std::uint32_t>();
            value = static_cast<NvU32>(v);
          } catch (...) {
            value.reset();
          }
        }
        if (node.contains("fallback") && !node["fallback"].is_null()) {
          try {
            auto v = node["fallback"].get<std::uint32_t>();
            fallback = static_cast<NvU32>(v);
          } catch (...) {
            fallback.reset();
          }
        }
        if (!had_override_present) {
          had_override = value.has_value();
        }
      };

      restore_info_t info;
      bool dummy_override = false;
      std::optional<NvU32> dummy_fallback;
      decode_section("frame_limit", info.frame_limit_applied, dummy_override, info.frame_limit_value, dummy_fallback);
      dummy_override = false;
      dummy_fallback.reset();
      decode_section("vsync", info.vsync_applied, dummy_override, info.vsync_value, dummy_fallback);
      dummy_override = false;
      dummy_fallback.reset();
      decode_section("low_latency", info.llm_applied, dummy_override, info.prerender_value, dummy_fallback);
      decode_section("ultra_low_latency_cpl", info.ull_cpl_applied, info.ull_cpl_override, info.ull_cpl_value, info.ull_cpl_fallback_value);
      decode_section("ultra_low_latency_enabled", info.ull_enabled_applied, info.ull_enabled_override, info.ull_enabled_value, info.ull_enabled_fallback_value);
      decode_section("smooth_motion", info.smooth_motion_applied, info.smooth_motion_override, info.smooth_motion_value, info.smooth_motion_fallback_value);
      decode_section("smooth_motion_mask", info.smooth_motion_mask_applied, info.smooth_motion_mask_override, info.smooth_motion_mask_value, info.smooth_motion_mask_fallback_value);

      if (!info.frame_limit_applied && !info.vsync_applied && !info.llm_applied && !info.ull_cpl_applied && !info.ull_enabled_applied && !info.smooth_motion_applied && !info.smooth_motion_mask_applied) {
        return std::nullopt;
      }

      return info;
    }

    void delete_overrides_file() {
      auto file_path_opt = overrides_file_path();
      if (!file_path_opt) {
        return;
      }
      std::error_code ec;
      std::filesystem::remove(*file_path_opt, ec);
      if (ec) {
        BOOST_LOG(warning) << "NVIDIA Control Panel overrides: failed to delete recovery file: " << ec.message();
      }
    }

    void maybe_restore_from_overrides_file() {
      if (g_recovery_file_owned) {
        return;
      }
      auto info_opt = read_overrides_file();
      if (!info_opt) {
        return;
      }

      BOOST_LOG(info) << "NVIDIA Control Panel overrides: pending recovery file detected; attempting restore";
      if (restore_with_fresh_session(*info_opt)) {
        delete_overrides_file();
      }
    }

    bool restore_with_fresh_session(const restore_info_t &restore_data) {
      if (!restore_data.frame_limit_applied && !restore_data.vsync_applied && !restore_data.llm_applied && !restore_data.ull_cpl_applied && !restore_data.ull_enabled_applied && !restore_data.smooth_motion_applied && !restore_data.smooth_motion_mask_applied) {
        return true;
      }

      NvAPI_Status status = NvAPI_Initialize();
      if (status != NVAPI_OK) {
        log_nvapi_error(status, "Initialize(restore)");
        return false;
      }

      NvDRSSessionHandle session = nullptr;
      status = NvAPI_DRS_CreateSession(&session);
      if (status != NVAPI_OK) {
        log_nvapi_error(status, "DRS_CreateSession(restore)");
        NvAPI_Unload();
        return false;
      }

      NvAPI_Status result = NVAPI_OK;

      do {
        result = NvAPI_DRS_LoadSettings(session);
        if (result != NVAPI_OK) {
          log_nvapi_error(result, "DRS_LoadSettings(restore)");
          break;
        }

        bool profile_is_global = false;
        NvDRSProfileHandle profile = resolve_profile(session, &profile_is_global);
        if (!profile) {
          result = NVAPI_ERROR;
          break;
        }

        auto restore_setting = [&](NvU32 setting_id, bool had_override, const std::optional<NvU32> &value, const std::optional<NvU32> &fallback, const char *label, bool skip_delete = false) -> bool {
          NVDRS_SETTING setting = {};
          setting.version = NVDRS_SETTING_VER;
          setting.settingId = setting_id;
          setting.settingType = NVDRS_DWORD_TYPE;
          setting.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;

          if (had_override) {
            if (value) {
              setting.u32CurrentValue = *value;
              NvAPI_Status s = NvAPI_DRS_SetSetting(session, profile, &setting);
              if (s != NVAPI_OK) {
                log_nvapi_error(s, label);
                return false;
              }
            } else if (!skip_delete) {
              NvAPI_Status s = NvAPI_DRS_DeleteProfileSetting(session, profile, setting_id);
              if (s != NVAPI_OK && s != NVAPI_SETTING_NOT_FOUND) {
                log_nvapi_error(s, label);
                return false;
              }
            }
          } else {
            if (fallback) {
              setting.u32CurrentValue = *fallback;
              NvAPI_Status s = NvAPI_DRS_SetSetting(session, profile, &setting);
              if (s != NVAPI_OK) {
                log_nvapi_error(s, label);
                return false;
              }
            } else if (!skip_delete) {
              NvAPI_Status s = NvAPI_DRS_DeleteProfileSetting(session, profile, setting_id);
              if (s != NVAPI_OK && s != NVAPI_SETTING_NOT_FOUND) {
                log_nvapi_error(s, label);
                return false;
              }
            }
          }
          return true;
        };

        if (restore_data.frame_limit_applied) {
          if (!restore_setting(FRL_FPS_ID, restore_data.frame_limit_value.has_value(), restore_data.frame_limit_value, std::nullopt, "DRS_SetSetting(FRL_FPS restore)")) {
            break;
          }
        }

        if (restore_data.vsync_applied) {
          if (!restore_setting(VSYNCMODE_ID, restore_data.vsync_value.has_value(), restore_data.vsync_value, std::nullopt, "DRS_SetSetting(VSYNCMODE restore)")) {
            break;
          }
        }

        if (restore_data.llm_applied) {
          if (!restore_setting(PRERENDERLIMIT_ID, restore_data.prerender_value.has_value(), restore_data.prerender_value, std::nullopt, "DRS_SetSetting(PRERENDERLIMIT restore)")) {
            break;
          }
        }

        if (restore_data.ull_cpl_applied) {
          if (!restore_setting(ULTRA_LOW_LATENCY_CPL_STATE_ID, restore_data.ull_cpl_override, restore_data.ull_cpl_value, restore_data.ull_cpl_fallback_value, "DRS_SetSetting(ULL_CPL restore)", true)) {
            break;
          }
          NvU32 restored_value = restore_data.ull_cpl_override && restore_data.ull_cpl_value ? *restore_data.ull_cpl_value : (restore_data.ull_cpl_fallback_value ? *restore_data.ull_cpl_fallback_value : 0u);
          BOOST_LOG(info) << "NVIDIA Ultra Low Latency CPL state restored to value " << restored_value;
        }

        if (restore_data.ull_enabled_applied) {
          if (!restore_setting(ULTRA_LOW_LATENCY_ENABLED_ID, restore_data.ull_enabled_override, restore_data.ull_enabled_value, restore_data.ull_enabled_fallback_value, "DRS_SetSetting(ULL_ENABLED restore)", true)) {
            break;
          }
          NvU32 restored_value = restore_data.ull_enabled_override && restore_data.ull_enabled_value ? *restore_data.ull_enabled_value : (restore_data.ull_enabled_fallback_value ? *restore_data.ull_enabled_fallback_value : 0u);
          BOOST_LOG(info) << "NVIDIA Ultra Low Latency enable restored to value " << restored_value;
        }

        if (restore_data.smooth_motion_applied) {
          // Skip delete for smooth motion settings to avoid breaking driver state.
          // Deleting undocumented settings can mark them as unsupported in the driver profile.
          if (!restore_setting(SMOOTH_MOTION_ENABLE_ID, restore_data.smooth_motion_override, restore_data.smooth_motion_value, restore_data.smooth_motion_fallback_value, "DRS_SetSetting(SMOOTH_MOTION restore)", true)) {
            break;
          }
          NvU32 restored_value = restore_data.smooth_motion_override && restore_data.smooth_motion_value ? *restore_data.smooth_motion_value : (restore_data.smooth_motion_fallback_value ? *restore_data.smooth_motion_fallback_value : 0u);
          BOOST_LOG(info) << "NVIDIA Smooth Motion restored to value " << restored_value;
        }

        if (restore_data.smooth_motion_mask_applied) {
          // Skip delete for smooth motion settings to avoid breaking driver state.
          // Deleting undocumented settings can mark them as unsupported in the driver profile.
          if (!restore_setting(SMOOTH_MOTION_API_MASK_ID, restore_data.smooth_motion_mask_override, restore_data.smooth_motion_mask_value, restore_data.smooth_motion_mask_fallback_value, "DRS_SetSetting(SMOOTH_MOTION_MASK restore)", true)) {
            break;
          }
          NvU32 restored_mask = restore_data.smooth_motion_mask_override && restore_data.smooth_motion_mask_value ? *restore_data.smooth_motion_mask_value : (restore_data.smooth_motion_mask_fallback_value ? *restore_data.smooth_motion_mask_fallback_value : 0u);
          BOOST_LOG(info) << "NVIDIA Smooth Motion API mask restored to value " << restored_mask;
        }

        result = NvAPI_DRS_SaveSettings(session);
        if (result != NVAPI_OK) {
          log_nvapi_error(result, "DRS_SaveSettings(restore)");
          break;
        }
      } while (false);

      NvAPI_DRS_DestroySession(session);
      NvAPI_Unload();

      if (result == NVAPI_OK) {
        BOOST_LOG(info) << "NVIDIA Control Panel overrides restored";
        return true;
      }
      return false;
    }

  }  // namespace

  void restore_pending_overrides() {
    maybe_restore_from_overrides_file();
  }

  bool is_available() {
    maybe_restore_from_overrides_file();

    if (g_state.initialized) {
      return true;
    }

    NvAPI_Status status = NvAPI_Initialize();
    if (status != NVAPI_OK) {
      return false;
    }

    NvDRSSessionHandle session = nullptr;
    status = NvAPI_DRS_CreateSession(&session);
    if (status != NVAPI_OK) {
      NvAPI_Unload();
      return false;
    }

    status = NvAPI_DRS_LoadSettings(session);
    NvAPI_DRS_DestroySession(session);
    NvAPI_Unload();

    return status == NVAPI_OK;
  }

  bool streaming_start(int fps, bool apply_frame_limit, bool force_frame_limit_off, bool force_vsync_off, bool force_low_latency_off, bool apply_smooth_motion) {
    maybe_restore_from_overrides_file();

    g_state.frame_limit_applied = false;
    g_state.vsync_applied = false;
    g_state.llm_applied = false;
    g_state.ull_cpl_applied = false;
    g_state.ull_enabled_applied = false;
    g_state.smooth_motion_applied = false;
    g_state.smooth_motion_mask_applied = false;
    g_state.original_frame_limit.reset();
    g_state.original_vsync.reset();
    g_state.original_prerender_limit.reset();
    g_state.original_ull_cpl_state.reset();
    g_state.original_ull_enabled.reset();
    g_state.original_ull_cpl_state_value.reset();
    g_state.original_ull_enabled_value.reset();
    g_state.original_ull_cpl_state_override = false;
    g_state.original_ull_enabled_override = false;
    g_state.original_smooth_motion.reset();
    g_state.original_smooth_motion_mask.reset();
    g_state.original_smooth_motion_value.reset();
    g_state.original_smooth_motion_mask_value.reset();
    g_state.original_smooth_motion_override = false;
    g_state.original_smooth_motion_mask_override = false;

    if (!apply_frame_limit && !force_frame_limit_off && !force_vsync_off && !force_low_latency_off && !apply_smooth_motion) {
      return false;
    }

    if (apply_frame_limit && force_frame_limit_off) {
      force_frame_limit_off = false;
    }

    if (apply_frame_limit && fps <= 0) {
      BOOST_LOG(warning) << "NVIDIA Control Panel limiter requested with non-positive FPS";
      apply_frame_limit = false;
    }

    if (!ensure_initialized()) {
      return false;
    }

    bool dirty = false;
    bool frame_limit_success = false;
    bool smooth_motion_already_enabled = false;

    if (apply_frame_limit || force_frame_limit_off) {
      NvU32 current_value = 0;
      NvAPI_Status status = get_current_setting(FRL_FPS_ID, g_state.original_frame_limit, nullptr, &current_value);
      if (status != NVAPI_OK) {
        log_nvapi_error(status, "DRS_GetSetting(FRL_FPS)");
      } else {
        const bool already_disabled = current_value == FRL_FPS_DISABLED;
        NVDRS_SETTING setting = {};
        setting.version = NVDRS_SETTING_VER;
        setting.settingId = FRL_FPS_ID;
        setting.settingType = NVDRS_DWORD_TYPE;
        setting.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
        int clamped_fps = 0;
        if (apply_frame_limit) {
          clamped_fps = std::clamp(fps, (int) (FRL_FPS_MIN + 1), (int) FRL_FPS_MAX);
          setting.u32CurrentValue = (NvU32) clamped_fps;
        } else {
          clamped_fps = 0;
          setting.u32CurrentValue = FRL_FPS_DISABLED;
        }

        if (!already_disabled || apply_frame_limit) {
          status = NvAPI_DRS_SetSetting(g_state.session, g_state.profile, &setting);
          if (status != NVAPI_OK) {
            log_nvapi_error(status, "DRS_SetSetting(FRL_FPS)");
          } else {
            g_state.frame_limit_applied = true;
            dirty = true;
            frame_limit_success = true;
            if (apply_frame_limit) {
              BOOST_LOG(info) << "NVIDIA Control Panel frame limiter set to " << clamped_fps;
            } else {
              BOOST_LOG(info) << "NVIDIA Control Panel frame limiter disabled for stream";
            }
          }
        }
      }
    }

    if (force_vsync_off) {
      NvAPI_Status status = get_current_setting(VSYNCMODE_ID, g_state.original_vsync);
      if (status != NVAPI_OK) {
        log_nvapi_error(status, "DRS_GetSetting(VSYNCMODE)");
      } else {
        NVDRS_SETTING setting = {};
        setting.version = NVDRS_SETTING_VER;
        setting.settingId = VSYNCMODE_ID;
        setting.settingType = NVDRS_DWORD_TYPE;
        setting.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
        setting.u32CurrentValue = VSYNCMODE_FORCEOFF;

        status = NvAPI_DRS_SetSetting(g_state.session, g_state.profile, &setting);
        if (status != NVAPI_OK) {
          log_nvapi_error(status, "DRS_SetSetting(VSYNCMODE)");
        } else {
          g_state.vsync_applied = true;
          dirty = true;
          BOOST_LOG(info) << "NVIDIA Control Panel VSYNC forced off for stream";
        }
      }
    }

    if (force_low_latency_off) {
      NvAPI_Status status = get_current_setting(PRERENDERLIMIT_ID, g_state.original_prerender_limit);
      if (status != NVAPI_OK) {
        log_nvapi_error(status, "DRS_GetSetting(PRERENDERLIMIT)");
      } else {
        NVDRS_SETTING setting = {};
        setting.version = NVDRS_SETTING_VER;
        setting.settingId = PRERENDERLIMIT_ID;
        setting.settingType = NVDRS_DWORD_TYPE;
        setting.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
        setting.u32CurrentValue = 1u;

        status = NvAPI_DRS_SetSetting(g_state.session, g_state.profile, &setting);
        if (status != NVAPI_OK) {
          log_nvapi_error(status, "DRS_SetSetting(PRERENDERLIMIT)");
        } else {
          g_state.llm_applied = true;
          dirty = true;
          BOOST_LOG(info) << "NVIDIA Control Panel pre-rendered frames forced to 1 for stream";
        }
      }
    }

    if (apply_smooth_motion) {
      if (!g_state.smooth_motion_supported) {
        const std::string required_version = format_driver_version(SMOOTH_MOTION_MIN_DRIVER_VERSION);
        const std::string current_version = format_driver_version(g_state.driver_version);
        BOOST_LOG(warning) << "NVIDIA Smooth Motion requires driver " << required_version << " or newer; current driver version " << current_version;
      } else {
        NvU32 original_smooth_value = 0;
        NvAPI_Status status = get_current_setting(SMOOTH_MOTION_ENABLE_ID, g_state.original_smooth_motion, &g_state.original_smooth_motion_override, &original_smooth_value);
        if (status != NVAPI_OK) {
          log_nvapi_error(status, "DRS_GetSetting(SMOOTH_MOTION)");
          g_state.original_smooth_motion_value.reset();
        }
        if (status == NVAPI_OK) {
          g_state.original_smooth_motion_value = original_smooth_value;
        }

        NvU32 original_mask_value = 0;
        NvAPI_Status mask_status = get_current_setting(SMOOTH_MOTION_API_MASK_ID, g_state.original_smooth_motion_mask, &g_state.original_smooth_motion_mask_override, &original_mask_value);
        if (mask_status != NVAPI_OK) {
          log_nvapi_error(mask_status, "DRS_GetSetting(SMOOTH_MOTION_MASK)");
          g_state.original_smooth_motion_mask_value.reset();
        }
        NvU32 recorded_mask_value = original_mask_value;
        if (mask_status == NVAPI_OK) {
          if (!g_state.original_smooth_motion_mask_override && !g_state.original_smooth_motion_mask.has_value() && recorded_mask_value == 0) {
            recorded_mask_value = SMOOTH_MOTION_API_MASK_VALUE;
          }
          g_state.original_smooth_motion_mask_value = recorded_mask_value;
        }

        const NvU32 desired = SMOOTH_MOTION_ON;
        const bool already_enabled = g_state.original_smooth_motion && *g_state.original_smooth_motion == desired;

        if (!already_enabled && status == NVAPI_OK) {
          NVDRS_SETTING setting = {};
          setting.version = NVDRS_SETTING_VER;
          setting.settingId = SMOOTH_MOTION_ENABLE_ID;
          setting.settingType = NVDRS_DWORD_TYPE;
          setting.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
          setting.u32CurrentValue = desired;

          status = NvAPI_DRS_SetSetting(g_state.session, g_state.profile, &setting);
          if (status != NVAPI_OK) {
            log_nvapi_error(status, "DRS_SetSetting(SMOOTH_MOTION)");
          } else {
            g_state.smooth_motion_applied = true;
            dirty = true;
            BOOST_LOG(info) << "NVIDIA Smooth Motion enabled for stream";
          }
        } else if (already_enabled) {
          smooth_motion_already_enabled = true;
          BOOST_LOG(info) << "NVIDIA Smooth Motion already enabled globally";
        }

        if (mask_status == NVAPI_OK) {
          const bool mask_matches = recorded_mask_value == SMOOTH_MOTION_API_MASK_VALUE;
          if (!mask_matches) {
            NVDRS_SETTING mask_setting = {};
            mask_setting.version = NVDRS_SETTING_VER;
            mask_setting.settingId = SMOOTH_MOTION_API_MASK_ID;
            mask_setting.settingType = NVDRS_DWORD_TYPE;
            mask_setting.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
            mask_setting.u32CurrentValue = SMOOTH_MOTION_API_MASK_VALUE;

            NvAPI_Status set_mask_status = NvAPI_DRS_SetSetting(g_state.session, g_state.profile, &mask_setting);
            if (set_mask_status != NVAPI_OK) {
              log_nvapi_error(set_mask_status, "DRS_SetSetting(SMOOTH_MOTION_MASK)");
            } else {
            g_state.smooth_motion_mask_applied = true;
            dirty = true;
            BOOST_LOG(info) << "NVIDIA Smooth Motion API mask set to DX12|DX11|Vulkan";
          }
        }

        // Align Ultra Low Latency driver state with Smooth Motion requirement.
        NvU32 ull_cpl_value = 0;
        NvAPI_Status ull_cpl_status = get_current_setting(ULTRA_LOW_LATENCY_CPL_STATE_ID, g_state.original_ull_cpl_state, &g_state.original_ull_cpl_state_override, &ull_cpl_value);
        if (ull_cpl_status != NVAPI_OK) {
          log_nvapi_error(ull_cpl_status, "DRS_GetSetting(ULL_CPL_STATE)");
          g_state.original_ull_cpl_state_value.reset();
        } else {
          g_state.original_ull_cpl_state_value = ull_cpl_value;
          if (ull_cpl_value != ULTRA_LOW_LATENCY_CPL_STATE_ULTRA) {
            NVDRS_SETTING setting = {};
            setting.version = NVDRS_SETTING_VER;
            setting.settingId = ULTRA_LOW_LATENCY_CPL_STATE_ID;
            setting.settingType = NVDRS_DWORD_TYPE;
            setting.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
            setting.u32CurrentValue = ULTRA_LOW_LATENCY_CPL_STATE_ULTRA;

            NvAPI_Status set_status = NvAPI_DRS_SetSetting(g_state.session, g_state.profile, &setting);
            if (set_status != NVAPI_OK) {
              log_nvapi_error(set_status, "DRS_SetSetting(ULL_CPL_STATE)");
            } else {
              g_state.ull_cpl_applied = true;
              dirty = true;
              BOOST_LOG(info) << "NVIDIA Ultra Low Latency CPL state set to Ultra";
            }
          }
        }

        NvU32 ull_enabled_value = 0;
        NvAPI_Status ull_enabled_status = get_current_setting(ULTRA_LOW_LATENCY_ENABLED_ID, g_state.original_ull_enabled, &g_state.original_ull_enabled_override, &ull_enabled_value);
        if (ull_enabled_status != NVAPI_OK) {
          log_nvapi_error(ull_enabled_status, "DRS_GetSetting(ULL_ENABLED)");
          g_state.original_ull_enabled_value.reset();
        } else {
          g_state.original_ull_enabled_value = ull_enabled_value;
          if (ull_enabled_value != ULTRA_LOW_LATENCY_ENABLED_ON) {
            NVDRS_SETTING setting = {};
            setting.version = NVDRS_SETTING_VER;
            setting.settingId = ULTRA_LOW_LATENCY_ENABLED_ID;
            setting.settingType = NVDRS_DWORD_TYPE;
            setting.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
            setting.u32CurrentValue = ULTRA_LOW_LATENCY_ENABLED_ON;

            NvAPI_Status set_status = NvAPI_DRS_SetSetting(g_state.session, g_state.profile, &setting);
            if (set_status != NVAPI_OK) {
              log_nvapi_error(set_status, "DRS_SetSetting(ULL_ENABLED)");
            } else {
              g_state.ull_enabled_applied = true;
              dirty = true;
              BOOST_LOG(info) << "NVIDIA Ultra Low Latency enabled";
            }
          }
        }
      }
    }
    }

    if (dirty) {
      NvAPI_Status status = NvAPI_DRS_SaveSettings(g_state.session);
      if (status != NVAPI_OK) {
        log_nvapi_error(status, "DRS_SaveSettings(stream)");
      } else {
        restore_info_t info {
          g_state.frame_limit_applied,
          g_state.original_frame_limit,
          g_state.vsync_applied,
          g_state.original_vsync,
          g_state.llm_applied,
          g_state.original_prerender_limit,
          g_state.ull_cpl_applied,
          g_state.original_ull_cpl_state,
          g_state.original_ull_cpl_state_value,
          g_state.original_ull_cpl_state_override,
          g_state.ull_enabled_applied,
          g_state.original_ull_enabled,
          g_state.original_ull_enabled_value,
          g_state.original_ull_enabled_override,
          g_state.smooth_motion_applied,
          g_state.original_smooth_motion,
          g_state.original_smooth_motion_value,
          g_state.original_smooth_motion_override,
          g_state.smooth_motion_mask_applied,
          g_state.original_smooth_motion_mask,
          g_state.original_smooth_motion_mask_value,
          g_state.original_smooth_motion_mask_override,
        };
        if (write_overrides_file(info)) {
          g_recovery_file_owned = true;
        }
      }
    }

    return frame_limit_success || g_state.vsync_applied || g_state.llm_applied || g_state.ull_cpl_applied || g_state.ull_enabled_applied || g_state.smooth_motion_applied || g_state.smooth_motion_mask_applied || smooth_motion_already_enabled;
  }

  void streaming_stop() {
    if (!g_state.initialized || !g_state.session || !g_state.profile) {
      cleanup();
      return;
    }

    restore_info_t info {
      g_state.frame_limit_applied,
      g_state.original_frame_limit,
      g_state.vsync_applied,
      g_state.original_vsync,
      g_state.llm_applied,
      g_state.original_prerender_limit,
      g_state.ull_cpl_applied,
      g_state.original_ull_cpl_state,
      g_state.original_ull_cpl_state_value,
      g_state.original_ull_cpl_state_override,
      g_state.ull_enabled_applied,
      g_state.original_ull_enabled,
      g_state.original_ull_enabled_value,
      g_state.original_ull_enabled_override,
      g_state.smooth_motion_applied,
      g_state.original_smooth_motion,
      g_state.original_smooth_motion_value,
      g_state.original_smooth_motion_override,
      g_state.smooth_motion_mask_applied,
      g_state.original_smooth_motion_mask,
      g_state.original_smooth_motion_mask_value,
      g_state.original_smooth_motion_mask_override,
    };

    cleanup();

    if (restore_with_fresh_session(info)) {
      delete_overrides_file();
    } else {
      BOOST_LOG(warning) << "Failed to restore NVIDIA Control Panel overrides";
    }

    g_recovery_file_owned = false;
  }

}  // namespace platf::frame_limiter_nvcp

#endif  // _WIN32
