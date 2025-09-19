/**
 * @file src/platform/windows/frame_limiter_nvcp.cpp
 * @brief NVIDIA Control Panel frame limiter provider implementation.
 */

#ifdef _WIN32

  #include "frame_limiter_nvcp.h"

  #include <algorithm>
  #include <cstdint>
  #include <filesystem>
  #include <fstream>
  #include <optional>
  #include <system_error>

  #include <windows.h>
  #include <nvapi.h>
  #include <NvApiDriverSettings.h>

  #include <nlohmann/json.hpp>

  #include "src/logging.h"

namespace platf::frame_limiter_nvcp {

  namespace {

    struct state_t {
      NvDRSSessionHandle session = nullptr;
      NvDRSProfileHandle profile = nullptr;
      bool initialized = false;
      bool frame_limit_applied = false;
      bool vsync_applied = false;
      bool llm_applied = false;
      std::optional<NvU32> original_frame_limit;
      std::optional<NvU32> original_vsync;
      std::optional<NvU32> original_prerender_limit;
    };

    state_t g_state;
    bool g_recovery_file_owned = false;

    void log_nvapi_error(NvAPI_Status status, const char *label) {
      NvAPI_ShortString message = {};
      NvAPI_GetErrorMessage(status, message);
      BOOST_LOG(warning) << "NvAPI " << label << " failed: " << message;
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
      g_state.frame_limit_applied = false;
      g_state.vsync_applied = false;
      g_state.llm_applied = false;
      g_state.original_frame_limit.reset();
      g_state.original_vsync.reset();
      g_state.original_prerender_limit.reset();
    }

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

      status = NvAPI_DRS_GetBaseProfile(g_state.session, &g_state.profile);
      if (status != NVAPI_OK) {
        log_nvapi_error(status, "DRS_GetBaseProfile");
        cleanup();
        return false;
      }

      return true;
    }

    NvAPI_Status get_current_setting(NvU32 setting_id, std::optional<NvU32> &storage) {
      NVDRS_SETTING existing = {};
      existing.version = NVDRS_SETTING_VER;
      NvAPI_Status status = NvAPI_DRS_GetSetting(g_state.session, g_state.profile, setting_id, &existing);
      if (status == NVAPI_OK && existing.settingLocation == NVDRS_CURRENT_PROFILE_LOCATION) {
        storage = existing.u32CurrentValue;
      } else if (status == NVAPI_SETTING_NOT_FOUND) {
        storage.reset();
        status = NVAPI_OK;
      }
      return status;
    }

    struct restore_info_t {
      bool frame_limit_applied = false;
      std::optional<NvU32> frame_limit_value;
      bool vsync_applied = false;
      std::optional<NvU32> vsync_value;
      bool llm_applied = false;
      std::optional<NvU32> prerender_value;
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
      if (!info.frame_limit_applied && !info.vsync_applied && !info.llm_applied) {
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
      auto encode_section = [&](const char *key, bool applied, const std::optional<NvU32> &value) {
        nlohmann::json node;
        node["applied"] = applied;
        if (value.has_value()) {
          node["value"] = *value;
        } else {
          node["value"] = nullptr;
        }
        j[key] = node;
      };

      encode_section("frame_limit", info.frame_limit_applied, info.frame_limit_value);
      encode_section("vsync", info.vsync_applied, info.vsync_value);
      encode_section("low_latency", info.llm_applied, info.prerender_value);

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

      auto decode_section = [&](const char *key, bool &applied, std::optional<NvU32> &value) {
        applied = false;
        value.reset();
        if (!j.contains(key)) {
          return;
        }
        const auto &node = j[key];
        if (!node.is_object()) {
          return;
        }
        applied = node.value("applied", false);
        if (node.contains("value") && !node["value"].is_null()) {
          try {
            auto v = node["value"].get<std::uint32_t>();
            value = static_cast<NvU32>(v);
          } catch (...) {
            value.reset();
          }
        }
      };

      restore_info_t info;
      decode_section("frame_limit", info.frame_limit_applied, info.frame_limit_value);
      decode_section("vsync", info.vsync_applied, info.vsync_value);
      decode_section("low_latency", info.llm_applied, info.prerender_value);

      if (!info.frame_limit_applied && !info.vsync_applied && !info.llm_applied) {
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
      if (!restore_data.frame_limit_applied && !restore_data.vsync_applied && !restore_data.llm_applied) {
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

        NvDRSProfileHandle profile = nullptr;
        result = NvAPI_DRS_GetBaseProfile(session, &profile);
        if (result != NVAPI_OK) {
          log_nvapi_error(result, "DRS_GetBaseProfile(restore)");
          break;
        }

        auto restore_setting = [&](NvU32 setting_id, std::optional<NvU32> value,
                                   const char *label) -> bool {
          NVDRS_SETTING setting = {};
          setting.version = NVDRS_SETTING_VER1;
          setting.settingId = setting_id;
          setting.settingType = NVDRS_DWORD_TYPE;
          setting.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;

          if (value) {
            setting.u32CurrentValue = *value;
            NvAPI_Status s = NvAPI_DRS_SetSetting(session, profile, &setting);
            if (s != NVAPI_OK) {
              log_nvapi_error(s, label);
              return false;
            }
          } else {
            NvAPI_Status s = NvAPI_DRS_DeleteProfileSetting(session, profile, setting_id);
            if (s != NVAPI_OK && s != NVAPI_SETTING_NOT_FOUND) {
              log_nvapi_error(s, label);
              return false;
            }
          }
          return true;
        };

        if (restore_data.frame_limit_applied) {
          if (!restore_setting(FRL_FPS_ID, restore_data.frame_limit_value, "DRS_SetSetting(FRL_FPS restore)")) {
            break;
          }
        }

        if (restore_data.vsync_applied) {
          if (!restore_setting(VSYNCMODE_ID, restore_data.vsync_value, "DRS_SetSetting(VSYNCMODE restore)")) {
            break;
          }
        }

        if (restore_data.llm_applied) {
          if (!restore_setting(PRERENDERLIMIT_ID, restore_data.prerender_value, "DRS_SetSetting(PRERENDERLIMIT restore)")) {
            break;
          }
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

  bool streaming_start(int fps, bool apply_frame_limit, bool force_vsync_off, bool force_low_latency_off) {
    maybe_restore_from_overrides_file();

    g_state.frame_limit_applied = false;
    g_state.vsync_applied = false;
    g_state.llm_applied = false;
    g_state.original_frame_limit.reset();
    g_state.original_vsync.reset();
    g_state.original_prerender_limit.reset();

    if (!apply_frame_limit && !force_vsync_off && !force_low_latency_off) {
      return false;
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

    if (apply_frame_limit) {
      NvAPI_Status status = get_current_setting(FRL_FPS_ID, g_state.original_frame_limit);
      if (status != NVAPI_OK) {
        log_nvapi_error(status, "DRS_GetSetting(FRL_FPS)");
      } else {
        NVDRS_SETTING setting = {};
        setting.version = NVDRS_SETTING_VER1;
        setting.settingId = FRL_FPS_ID;
        setting.settingType = NVDRS_DWORD_TYPE;
        setting.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
        int clamped_fps = std::clamp(fps, (int) (FRL_FPS_MIN + 1), (int) FRL_FPS_MAX);
        setting.u32CurrentValue = (NvU32) clamped_fps;

        status = NvAPI_DRS_SetSetting(g_state.session, g_state.profile, &setting);
        if (status != NVAPI_OK) {
          log_nvapi_error(status, "DRS_SetSetting(FRL_FPS)");
        } else {
          g_state.frame_limit_applied = true;
          dirty = true;
          frame_limit_success = true;
          BOOST_LOG(info) << "NVIDIA Control Panel frame limiter set to " << clamped_fps;
        }
      }
    }

    if (force_vsync_off) {
      NvAPI_Status status = get_current_setting(VSYNCMODE_ID, g_state.original_vsync);
      if (status != NVAPI_OK) {
        log_nvapi_error(status, "DRS_GetSetting(VSYNCMODE)");
      } else {
        NVDRS_SETTING setting = {};
        setting.version = NVDRS_SETTING_VER1;
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
        setting.version = NVDRS_SETTING_VER1;
        setting.settingId = PRERENDERLIMIT_ID;
        setting.settingType = NVDRS_DWORD_TYPE;
        setting.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
        setting.u32CurrentValue = PRERENDERLIMIT_APP_CONTROLLED;

        status = NvAPI_DRS_SetSetting(g_state.session, g_state.profile, &setting);
        if (status != NVAPI_OK) {
          log_nvapi_error(status, "DRS_SetSetting(PRERENDERLIMIT)");
        } else {
          g_state.llm_applied = true;
          dirty = true;
          BOOST_LOG(info) << "NVIDIA Control Panel low latency mode forced to Off for stream";
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
        };
        if (write_overrides_file(info)) {
          g_recovery_file_owned = true;
        }
      }
    }

    return frame_limit_success;
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
