#pragma once
#include "windows.h"
inline HANDLE const WTS_CURRENT_SERVER_HANDLE = nullptr;

enum WTS_CONNECTSTATE_CLASS {
  WTSActive,
  WTSConnected,
  WTSDisconnected
};

enum WTS_INFO_CLASS {
  WTSUserName
};

struct WTS_SESSION_INFOW {
  DWORD SessionId;
  LPWSTR pWinStationName;
  WTS_CONNECTSTATE_CLASS State;
};

using PWTS_SESSION_INFOW = WTS_SESSION_INFOW *;
BOOL WTSEnumerateSessionsW(HANDLE, DWORD, DWORD, PWTS_SESSION_INFOW *, DWORD *);
BOOL WTSQuerySessionInformationW(HANDLE, DWORD, WTS_INFO_CLASS, LPWSTR *, DWORD *);
BOOL WTSQueryUserToken(DWORD, HANDLE *);
DWORD WTSGetActiveConsoleSessionId();
void WTSFreeMemory(void *);
