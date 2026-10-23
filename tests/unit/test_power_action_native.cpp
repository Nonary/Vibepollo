// Include standard headers before selecting the Windows implementation on the
// portable builder. Only the Win32 calls/types are substituted by power_api.
#include <src/power_action.h>
#ifndef _WIN32
  #define _WIN32
  #include <src/power_action_native.cpp>
  #undef _WIN32
#endif
#include <cwchar>
#include <gtest/gtest.h>

namespace {
  struct api_state_t {
    SYSTEM_POWER_CAPABILITIES caps = [] {
      SYSTEM_POWER_CAPABILITIES value {};
      value.SystemS3 = TRUE;
      return value;
    }();
    DWORD error {ERROR_SUCCESS};
    bool has_token {false}, fail_open {false}, fail_impersonation {false}, fail_lookup {false}, deny_privilege {false};
    bool query_success {true}, suspend_success {true}, enabled {false};
    int calls {0}, impersonations {0}, restores {0}, closes {0}, reverts {0};
  } state;
}  // namespace

HANDLE GetCurrentThread() {
  return reinterpret_cast<HANDLE>(1);
}

DWORD GetLastError() {
  return state.error;
}

void SetLastError(DWORD value) {
  state.error = value;
}

BOOL OpenThreadToken(HANDLE, DWORD rights, BOOL, HANDLE *token) {
  EXPECT_EQ(rights, TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY);
  if (!state.has_token || state.fail_open) {
    state.error = state.fail_open ? ERROR_ACCESS_DENIED : ERROR_NO_TOKEN;
    return FALSE;
  }
  *token = reinterpret_cast<HANDLE>(2);
  return TRUE;
}

BOOL ImpersonateSelf(int level) {
  EXPECT_EQ(level, SecurityImpersonation);
  ++state.impersonations;
  if (state.fail_impersonation) {
    return FALSE;
  }
  state.has_token = true;
  return TRUE;
}

BOOL RevertToSelf() {
  ++state.reverts;
  state.has_token = false;
  return TRUE;
}

BOOL CloseHandle(HANDLE) {
  ++state.closes;
  return TRUE;
}

BOOL LookupPrivilegeValueW(const wchar_t *system, const wchar_t *name, LUID *) {
  EXPECT_EQ(system, nullptr);
  EXPECT_EQ(std::wcscmp(name, L"SeShutdownPrivilege"), 0);
  return !state.fail_lookup;
}

BOOL AdjustTokenPrivileges(HANDLE, BOOL, TOKEN_PRIVILEGES *value, DWORD, TOKEN_PRIVILEGES *old, DWORD *) {
  if (!old) {
    state.enabled = value->Privileges[0].Attributes == SE_PRIVILEGE_ENABLED;
    ++state.restores;
    return TRUE;
  }
  if (state.deny_privilege) {
    state.error = ERROR_NOT_ALL_ASSIGNED;
    return TRUE;
  }
  old->PrivilegeCount = 1;
  old->Privileges[0].Attributes = state.enabled ? SE_PRIVILEGE_ENABLED : 0;
  state.enabled = value->Privileges[0].Attributes == SE_PRIVILEGE_ENABLED;
  return TRUE;
}

BOOL GetPwrCapabilities(SYSTEM_POWER_CAPABILITIES *caps) {
  *caps = state.caps;
  return state.query_success;
}

BOOL SetSuspendState(BOOL hibernate, BOOL force, BOOL disable_wake) {
  EXPECT_FALSE(hibernate);
  EXPECT_FALSE(force);
  EXPECT_FALSE(disable_wake);
  EXPECT_TRUE(state.enabled);
  ++state.calls;
  return state.suspend_success;
}

TEST(PowerActionNative, SleepPreservesWakeEventsAndRestoresThreadPrivilege) {
  state = {};
  EXPECT_TRUE(nvhttp::power_action::native_sleep());
  EXPECT_EQ(state.calls, 1);
  EXPECT_EQ(state.impersonations, 1);
  EXPECT_EQ(state.restores, 1);
  EXPECT_EQ(state.closes, 1);
  EXPECT_EQ(state.reverts, 1);
  EXPECT_FALSE(state.enabled);
}

TEST(PowerActionNative, ExistingThreadTokenIsPreservedOnNativeFailure) {
  state = {};
  state.has_token = true;
  state.suspend_success = false;
  EXPECT_FALSE(nvhttp::power_action::native_sleep());
  EXPECT_EQ(state.calls, 1);
  EXPECT_EQ(state.restores, 1);
  EXPECT_EQ(state.reverts, 0);
  EXPECT_FALSE(state.enabled);
}

TEST(PowerActionNative, DeniedPrivilegeNeverCallsSuspend) {
  state = {};
  state.deny_privilege = true;
  EXPECT_FALSE(nvhttp::power_action::native_sleep());
  EXPECT_EQ(state.calls, 0);
  EXPECT_EQ(state.closes, 1);
  EXPECT_EQ(state.reverts, 1);
}

TEST(PowerActionNative, TokenAndLookupFailuresNeverCallSuspend) {
  state = {};
  state.fail_open = true;
  EXPECT_FALSE(nvhttp::power_action::native_sleep());
  EXPECT_EQ(state.calls, 0);
  EXPECT_EQ(state.impersonations, 0);
  state = {};
  state.fail_lookup = true;
  EXPECT_FALSE(nvhttp::power_action::native_sleep());
  EXPECT_EQ(state.calls, 0);
  EXPECT_EQ(state.closes, 1);
  EXPECT_EQ(state.reverts, 1);
}

TEST(PowerActionNative, ModernStandbyIsSupportedAndNoSleepStatesIsNot) {
  state = {};
  state.caps = {};
#ifdef SUNSHINE_TEST_LEGACY_POWER_CAPABILITIES
  state.caps.spare2[2] = TRUE;
#else
  state.caps.AoAc = TRUE;
#endif
  EXPECT_TRUE(nvhttp::power_action::native_sleep_supported());
  state.caps = {};
  EXPECT_FALSE(nvhttp::power_action::native_sleep_supported());
  EXPECT_FALSE(nvhttp::power_action::native_sleep());
  EXPECT_EQ(state.calls, 0);
}

TEST(PowerActionNative, OtherPowerFlagsAreNotSleepStates) {
  state = {};
  state.caps = {};
  state.caps.FastSystemS4 = TRUE;
#ifdef SUNSHINE_TEST_LEGACY_POWER_CAPABILITIES
  state.caps.spare2[0] = TRUE;
  state.caps.spare2[1] = TRUE;
#else
  state.caps.Hiberboot = TRUE;
  state.caps.WakeAlarmPresent = TRUE;
#endif
  EXPECT_FALSE(nvhttp::power_action::native_sleep_supported());
  EXPECT_FALSE(nvhttp::power_action::native_sleep());
  EXPECT_EQ(state.calls, 0);
}

TEST(PowerActionNative, EachTraditionalSleepStateIsSupported) {
  for (auto member : {&SYSTEM_POWER_CAPABILITIES::SystemS1, &SYSTEM_POWER_CAPABILITIES::SystemS2, &SYSTEM_POWER_CAPABILITIES::SystemS3}) {
    state = {};
    state.caps = {};
    state.caps.*member = TRUE;
    EXPECT_TRUE(nvhttp::power_action::native_sleep_supported());
  }
}

TEST(PowerActionNative, FailedCapabilityQueryNeverCallsSuspend) {
  state = {};
  state.query_success = false;
  EXPECT_FALSE(nvhttp::power_action::native_sleep_supported());
  EXPECT_FALSE(nvhttp::power_action::native_sleep());
  EXPECT_EQ(state.calls, 0);
  EXPECT_EQ(state.impersonations, 0);
}
