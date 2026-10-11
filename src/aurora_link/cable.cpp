/**
 * @file src/aurora_link/cable.cpp
 * @brief Show/hide the virtual audio cable by enabling/disabling its PnP device.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <setupapi.h>
#include <cfgmgr32.h>

#include <cstdio>
#include <string>

#include "aurora_link.h"

#pragma comment(lib, "setupapi.lib")

namespace aurora_link {

  namespace {

    /**
     * The cable presents one PnP device that publishes several audio endpoints.
     * Matching on the hardware ID rather than a hard-coded instance path
     * (ROOT\MEDIA\0001) matters: that instance number is assigned at install time
     * and differs between machines, so pinning it would work on the PC it was
     * written on and quietly no-op on the second one.
     */
    constexpr wchar_t cable_hwid_fragment[] = L"VBAudioVACWDM";

    bool for_each_matching_device(bool enable, int &changed) {
      HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
      if (set == INVALID_HANDLE_VALUE) {
        return false;
      }

      SP_DEVINFO_DATA dev {};
      dev.cbSize = sizeof(dev);
      bool ok = true;

      for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        wchar_t ids[4096] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, nullptr,
                                               reinterpret_cast<PBYTE>(ids), sizeof(ids), nullptr)) {
          continue;
        }
        // REG_MULTI_SZ: walk the null-separated list.
        bool match = false;
        for (const wchar_t *p = ids; *p; p += wcslen(p) + 1) {
          if (wcsstr(p, cable_hwid_fragment) != nullptr) {
            match = true;
            break;
          }
        }
        if (!match) {
          continue;
        }

        SP_PROPCHANGE_PARAMS pc {};
        pc.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        pc.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        pc.StateChange = enable ? DICS_ENABLE : DICS_DISABLE;
        pc.Scope = DICS_FLAG_GLOBAL;  // persist across reboots, not just this hardware profile
        pc.HwProfile = 0;

        if (!SetupDiSetClassInstallParamsW(set, &dev, &pc.ClassInstallHeader, sizeof(pc)) ||
            !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &dev)) {
          fprintf(stderr, "aurora-link: %s failed (err %lu)\n", enable ? "enable" : "disable", GetLastError());
          ok = false;
          continue;
        }
        ++changed;
      }

      SetupDiDestroyDeviceInfoList(set);
      return ok;
    }

  }  // namespace

  bool set_cable_enabled(bool on) {
    int changed = 0;
    bool ok = for_each_matching_device(on, changed);
    if (changed == 0) {
      // Not an error worth failing the session over: a host without the cable
      // installed still streams fine, it just has no mic bridge.
      fprintf(stderr, "aurora-link: no virtual audio cable found (nothing to %s)\n", on ? "show" : "hide");
      return true;
    }
    printf("aurora-link: cable %s (%d device%s)\n", on ? "shown" : "hidden", changed, changed == 1 ? "" : "s");
    return ok;
  }

}  // namespace aurora_link
