/**
 * @file src/platform/windows/frame_limiter_nvcp.cpp
 * @brief NVIDIA Control Panel frame limiter provider implementation.
 */

#ifdef _WIN32

  #include "frame_limiter_nvcp.h"

  #include "src/logging.h"

  #include <algorithm>
  #include <cstdint>
  #include <nvapi.h>
  #include <NvApiDriverSettings.h>
  #include <optional>
  #include <windows.h>

namespace platf::frame_limiter_nvcp {

  namespace {

    struct state_t {
      NvDRSSessionHandle session = nullptr;
      NvDRSProfileHandle profile = nullptr;
      bool initialized = false;
      bool active = false;
      std::optional<NvU32> original_value;
    };

    state_t g_state;

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
      g_state.original_value.reset();
      g_state.active = false;
    }

    bool restore_with_fresh_session(const std::optional<NvU32> &original_value) {
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

        if (original_value) {
          NVDRS_SETTING restore = {};
          restore.version = NVDRS_SETTING_VER1;
          restore.settingId = FRL_FPS_ID;
          restore.settingType = NVDRS_DWORD_TYPE;
          restore.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
          restore.u32CurrentValue = *original_value;

          result = NvAPI_DRS_SetSetting(session, profile, &restore);
          if (result != NVAPI_OK) {
            log_nvapi_error(result, "DRS_SetSetting(FRL_FPS restore)");
            break;
          }
        } else {
          result = NvAPI_DRS_DeleteProfileSetting(session, profile, FRL_FPS_ID);
          if (result != NVAPI_OK && result != NVAPI_SETTING_NOT_FOUND) {
            log_nvapi_error(result, "DRS_DeleteProfileSetting(FRL_FPS restore)");
            break;
          }
          result = NVAPI_OK;
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
        BOOST_LOG(info) << "NVIDIA Control Panel frame limiter restored";
        return true;
      }
      return false;
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

  }  // namespace

  bool is_available() {
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

  bool streaming_start(int fps) {
    g_state.active = false;
    g_state.original_value.reset();

    if (fps <= 0) {
      BOOST_LOG(warning) << "NVIDIA Control Panel limiter requested with non-positive FPS";
      return false;
    }

    if (!ensure_initialized()) {
      return false;
    }

    NVDRS_SETTING existing = {};
    existing.version = NVDRS_SETTING_VER;
    NvAPI_Status status = NvAPI_DRS_GetSetting(g_state.session, g_state.profile, FRL_FPS_ID, &existing);
    if (status == NVAPI_OK && existing.settingLocation == NVDRS_CURRENT_PROFILE_LOCATION) {
      g_state.original_value = existing.u32CurrentValue;
    } else if (status != NVAPI_OK && status != NVAPI_SETTING_NOT_FOUND) {
      log_nvapi_error(status, "DRS_GetSetting(FRL_FPS)");
      cleanup();
      return false;
    }

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
      cleanup();
      return false;
    }

    status = NvAPI_DRS_SaveSettings(g_state.session);
    if (status != NVAPI_OK) {
      log_nvapi_error(status, "DRS_SaveSettings(apply)");
      cleanup();
      return false;
    }

    g_state.active = true;
    BOOST_LOG(info) << "NVIDIA Control Panel frame limiter set to " << clamped_fps;
    return true;
  }

  void streaming_stop() {
    if (!g_state.initialized || !g_state.session || !g_state.profile) {
      cleanup();
      return;
    }

    const bool was_active = g_state.active;
    const auto original_value = g_state.original_value;

    cleanup();

    if (was_active) {
      if (!restore_with_fresh_session(original_value)) {
        BOOST_LOG(warning) << "Failed to restore NVIDIA Control Panel frame limiter";
      }
    }
  }

}  // namespace platf::frame_limiter_nvcp

#endif  // _WIN32
