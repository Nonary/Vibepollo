#include "power_action.h"

#ifdef _WIN32
  // Winsock must precede Windows headers, and PowrProf needs Windows types.
  // clang-format off
  #include <winsock2.h>
  #include <windows.h>
  #include <powrprof.h>
// clang-format on
#endif

namespace nvhttp::power_action {
#ifdef _WIN32
  namespace {
    template<class T>
    bool modern_standby_supported(const T &caps) {
      if constexpr (requires { caps.AoAc; }) {
        return caps.AoAc != 0;
      } else {
        // Older MinGW headers reserve Hiberboot, WakeAlarmPresent and AoAc
        // as three bytes following FastSystemS4.
        static_assert(sizeof(caps.spare2) == 3);
        return caps.spare2[2] != 0;
      }
    }
  }  // namespace
#endif

  bool native_sleep_supported() {
#ifdef _WIN32
    SYSTEM_POWER_CAPABILITIES caps {};
    return GetPwrCapabilities(&caps) && (caps.SystemS1 || caps.SystemS2 || caps.SystemS3 || modern_standby_supported(caps));
#else
    // Linux's machine host needs a dedicated privileged broker operation.
    // Never substitute a shell command or an interactive elevation prompt.
    return false;
#endif
  }

  bool native_sleep() {
#ifdef _WIN32
    if (!native_sleep_supported()) {
      return false;
    }
    // Enable shutdown privilege on this worker's impersonation token only.
    // Do not expand the token of the entire multithreaded server process.
    HANDLE token = nullptr;
    bool impersonated = false;
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, TRUE, &token)) {
      if (GetLastError() != ERROR_NO_TOKEN || !ImpersonateSelf(SecurityImpersonation)) {
        return false;
      }
      impersonated = true;
      if (!OpenThreadToken(GetCurrentThread(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, TRUE, &token)) {
        RevertToSelf();
        return false;
      }
    }

    struct cleanup_t {
      HANDLE token;
      bool impersonated;
      TOKEN_PRIVILEGES previous {};
      bool adjusted {false};

      ~cleanup_t() {
        if (adjusted) {
          AdjustTokenPrivileges(token, FALSE, &previous, 0, nullptr, nullptr);
        }
        CloseHandle(token);
        if (impersonated) {
          RevertToSelf();
        }
      }
    } cleanup {token, impersonated};

    TOKEN_PRIVILEGES privileges {};
    privileges.PrivilegeCount = 1;
    if (!LookupPrivilegeValueW(nullptr, L"SeShutdownPrivilege", &privileges.Privileges[0].Luid)) {
      return false;
    }
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    DWORD size = sizeof(cleanup.previous);
    SetLastError(ERROR_SUCCESS);
    if (!AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(cleanup.previous), &cleanup.previous, &size) || GetLastError() != ERROR_SUCCESS) {
      return false;
    }
    cleanup.adjusted = true;
    // Sleep, never hibernate; preserve all wake events. The OS may still deny
    // the transition. This call can return after resume.
    return SetSuspendState(FALSE, FALSE, FALSE) != FALSE;
#else
    return false;
#endif
  }
}  // namespace nvhttp::power_action
