/**
 * @file src/platform/windows/nvprefs/nvapi_opensource_wrapper.cpp
 * @brief Definitions for the NVAPI wrapper.
 */
// standard includes
#include <cstring>
#include <map>

// local includes
#include "driver_settings.h"
#include "nvprefs_common.h"

// special nvapi header that should be the last include
#include <nvapi_interface.h>

namespace {

  std::map<const char *, void *> interfaces;
  HMODULE dll = nullptr;

  constexpr NvU32 NVAPI_DRS_SETSETTING_NEW_ID = 0x8A2CF5F5;
  constexpr NvU32 NVAPI_DRS_GETSETTING_NEW_ID = 0xEA99498D;
  constexpr NvU32 NVAPI_DRS_ENUMSETTINGS_NEW_ID = 0xCFD6983E;

  using drs_set_setting_new_fn = NvAPI_Status(__cdecl *) (NvDRSSessionHandle, NvDRSProfileHandle, NVDRS_SETTING *, NvU32, NvU32);
  using drs_get_setting_new_fn = NvAPI_Status(__cdecl *) (NvDRSSessionHandle, NvDRSProfileHandle, NvU32, NVDRS_SETTING *, NvU32 *);
  using drs_enum_settings_fn = NvAPI_Status(__cdecl *) (NvDRSSessionHandle, NvDRSProfileHandle, NvU32, NvU32 *, NVDRS_SETTING *);

  drs_set_setting_new_fn drs_set_setting_new = nullptr;
  drs_get_setting_new_fn drs_get_setting_new = nullptr;
  drs_enum_settings_fn drs_enum_settings_new = nullptr;

  template<typename Func, typename... Args>
  NvAPI_Status call_interface(const char *name, Args... args) {
    auto func = (Func *) interfaces[name];

    if (!func) {
      return interfaces.empty() ? NVAPI_API_NOT_INITIALIZED : NVAPI_NOT_SUPPORTED;
    }

    return func(args...);
  }

}  // namespace

#undef NVAPI_INTERFACE
#define NVAPI_INTERFACE NvAPI_Status __cdecl

extern void *__cdecl nvapi_QueryInterface(NvU32 id);

NVAPI_INTERFACE
NvAPI_Initialize() {
  if (dll) {
    return NVAPI_OK;
  }

#ifdef _WIN64
  auto dll_name = "nvapi64.dll";
#else
  auto dll_name = "nvapi.dll";
#endif

  if ((dll = LoadLibraryEx(dll_name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))) {
    auto query_interface = (decltype(nvapi_QueryInterface) *) GetProcAddress(dll, "nvapi_QueryInterface");
    if (!query_interface) {
      query_interface = (decltype(nvapi_QueryInterface) *) GetProcAddress(dll, "NvAPI_QueryInterface");
    }
    if (query_interface) {
      for (const auto &item : nvapi_interface_table) {
        void *resolved = nullptr;

        if (std::strcmp(item.func, "NvAPI_DRS_SetSetting") == 0) {
          drs_set_setting_new = reinterpret_cast<drs_set_setting_new_fn>(query_interface(NVAPI_DRS_SETSETTING_NEW_ID));
          resolved = drs_set_setting_new ? reinterpret_cast<void *>(drs_set_setting_new) : query_interface(item.id);
        } else if (std::strcmp(item.func, "NvAPI_DRS_GetSetting") == 0) {
          drs_get_setting_new = reinterpret_cast<drs_get_setting_new_fn>(query_interface(NVAPI_DRS_GETSETTING_NEW_ID));
          resolved = drs_get_setting_new ? reinterpret_cast<void *>(drs_get_setting_new) : query_interface(item.id);
        } else if (std::strcmp(item.func, "NvAPI_DRS_EnumSettings") == 0) {
          drs_enum_settings_new = reinterpret_cast<drs_enum_settings_fn>(query_interface(NVAPI_DRS_ENUMSETTINGS_NEW_ID));
          resolved = drs_enum_settings_new ? reinterpret_cast<void *>(drs_enum_settings_new) : query_interface(item.id);
        } else {
          resolved = query_interface(item.id);
        }

        interfaces[item.func] = resolved;
      }
      return NVAPI_OK;
    }
  }

  NvAPI_Unload();
  return NVAPI_LIBRARY_NOT_FOUND;
}

NVAPI_INTERFACE NvAPI_Unload() {
  if (dll) {
    interfaces.clear();
    drs_set_setting_new = nullptr;
    drs_get_setting_new = nullptr;
    drs_enum_settings_new = nullptr;
    FreeLibrary(dll);
    dll = nullptr;
  }
  return NVAPI_OK;
}

NVAPI_INTERFACE NvAPI_GetErrorMessage(NvAPI_Status nr, NvAPI_ShortString szDesc) {
  return call_interface<decltype(NvAPI_GetErrorMessage)>("NvAPI_GetErrorMessage", nr, szDesc);
}

// This is only a subset of NvAPI_DRS_* functions, more can be added if needed

NVAPI_INTERFACE NvAPI_DRS_CreateSession(NvDRSSessionHandle *phSession) {
  return call_interface<decltype(NvAPI_DRS_CreateSession)>("NvAPI_DRS_CreateSession", phSession);
}

NVAPI_INTERFACE NvAPI_DRS_DestroySession(NvDRSSessionHandle hSession) {
  return call_interface<decltype(NvAPI_DRS_DestroySession)>("NvAPI_DRS_DestroySession", hSession);
}

NVAPI_INTERFACE NvAPI_DRS_LoadSettings(NvDRSSessionHandle hSession) {
  return call_interface<decltype(NvAPI_DRS_LoadSettings)>("NvAPI_DRS_LoadSettings", hSession);
}

NVAPI_INTERFACE NvAPI_DRS_SaveSettings(NvDRSSessionHandle hSession) {
  return call_interface<decltype(NvAPI_DRS_SaveSettings)>("NvAPI_DRS_SaveSettings", hSession);
}

NVAPI_INTERFACE NvAPI_DRS_CreateProfile(NvDRSSessionHandle hSession, NVDRS_PROFILE *pProfileInfo, NvDRSProfileHandle *phProfile) {
  return call_interface<decltype(NvAPI_DRS_CreateProfile)>("NvAPI_DRS_CreateProfile", hSession, pProfileInfo, phProfile);
}

NVAPI_INTERFACE NvAPI_DRS_FindProfileByName(NvDRSSessionHandle hSession, NvAPI_UnicodeString profileName, NvDRSProfileHandle *phProfile) {
  return call_interface<decltype(NvAPI_DRS_FindProfileByName)>("NvAPI_DRS_FindProfileByName", hSession, profileName, phProfile);
}

NVAPI_INTERFACE NvAPI_DRS_GetProfileInfo(NvDRSSessionHandle hSession, NvDRSProfileHandle hProfile, NVDRS_PROFILE *pProfileInfo) {
  return call_interface<decltype(NvAPI_DRS_GetProfileInfo)>("NvAPI_DRS_GetProfileInfo", hSession, hProfile, pProfileInfo);
}

NVAPI_INTERFACE NvAPI_DRS_EnumProfiles(NvDRSSessionHandle hSession, NvU32 index, NvDRSProfileHandle *phProfile) {
  return call_interface<decltype(NvAPI_DRS_EnumProfiles)>("NvAPI_DRS_EnumProfiles", hSession, index, phProfile);
}

NVAPI_INTERFACE NvAPI_DRS_CreateApplication(NvDRSSessionHandle hSession, NvDRSProfileHandle hProfile, NVDRS_APPLICATION *pApplication) {
  return call_interface<decltype(NvAPI_DRS_CreateApplication)>("NvAPI_DRS_CreateApplication", hSession, hProfile, pApplication);
}

NVAPI_INTERFACE NvAPI_DRS_GetApplicationInfo(NvDRSSessionHandle hSession, NvDRSProfileHandle hProfile, NvAPI_UnicodeString appName, NVDRS_APPLICATION *pApplication) {
  return call_interface<decltype(NvAPI_DRS_GetApplicationInfo)>("NvAPI_DRS_GetApplicationInfo", hSession, hProfile, appName, pApplication);
}

NVAPI_INTERFACE NvAPI_DRS_EnumApplications(NvDRSSessionHandle hSession, NvDRSProfileHandle hProfile, NvU32 startIndex, NvU32 *appCount, NVDRS_APPLICATION *pApplication) {
  return call_interface<decltype(NvAPI_DRS_EnumApplications)>("NvAPI_DRS_EnumApplications", hSession, hProfile, startIndex, appCount, pApplication);
}

NVAPI_INTERFACE NvAPI_DRS_FindApplicationByName(NvDRSSessionHandle hSession, NvAPI_UnicodeString appName, NvDRSProfileHandle *phProfile, NVDRS_APPLICATION *pApplication) {
  return call_interface<decltype(NvAPI_DRS_FindApplicationByName)>("NvAPI_DRS_FindApplicationByName", hSession, appName, phProfile, pApplication);
}

NVAPI_INTERFACE NvAPI_DRS_SetSetting(NvDRSSessionHandle hSession, NvDRSProfileHandle hProfile, NVDRS_SETTING *pSetting) {
  if (drs_set_setting_new) {
    return drs_set_setting_new(hSession, hProfile, pSetting, 0, 0);
  }
  return call_interface<decltype(NvAPI_DRS_SetSetting)>("NvAPI_DRS_SetSetting", hSession, hProfile, pSetting);
}

NVAPI_INTERFACE NvAPI_DRS_GetSetting(NvDRSSessionHandle hSession, NvDRSProfileHandle hProfile, NvU32 settingId, NVDRS_SETTING *pSetting) {
  if (drs_get_setting_new) {
    NvU32 flags = 0;
    return drs_get_setting_new(hSession, hProfile, settingId, pSetting, &flags);
  }
  return call_interface<decltype(NvAPI_DRS_GetSetting)>("NvAPI_DRS_GetSetting", hSession, hProfile, settingId, pSetting);
}

NVAPI_INTERFACE NvAPI_DRS_EnumSettings(NvDRSSessionHandle hSession, NvDRSProfileHandle hProfile, NvU32 startIndex, NvU32 *settingsCount, NVDRS_SETTING *pSetting) {
  if (drs_enum_settings_new) {
    return drs_enum_settings_new(hSession, hProfile, startIndex, settingsCount, pSetting);
  }
  return call_interface<decltype(NvAPI_DRS_EnumSettings)>("NvAPI_DRS_EnumSettings", hSession, hProfile, startIndex, settingsCount, pSetting);
}

NVAPI_INTERFACE NvAPI_DRS_DeleteProfileSetting(NvDRSSessionHandle hSession, NvDRSProfileHandle hProfile, NvU32 settingId) {
  return call_interface<decltype(NvAPI_DRS_DeleteProfileSetting)>("NvAPI_DRS_DeleteProfileSetting", hSession, hProfile, settingId);
}

NVAPI_INTERFACE NvAPI_DRS_GetBaseProfile(NvDRSSessionHandle hSession, NvDRSProfileHandle *phProfile) {
  return call_interface<decltype(NvAPI_DRS_GetBaseProfile)>("NvAPI_DRS_GetBaseProfile", hSession, phProfile);
}
