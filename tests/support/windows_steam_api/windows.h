#pragma once
// Narrow Windows boundary for exercising the production Steam discovery helper.
#include <cstdint>
using DWORD = std::uint32_t;
using BYTE = unsigned char;
using BOOL = int;
using HANDLE = void *;
using HKEY = void *;
using HLOCAL = void *;
using PSID = void *;
using LPWSTR = wchar_t *;
using LPCWSTR = const wchar_t *;
using LSTATUS = long;
inline constexpr BOOL FALSE = 0;
inline constexpr BOOL TRUE = 1;
inline constexpr DWORD ERROR_NO_TOKEN = 1008;
inline constexpr DWORD TOKEN_IMPERSONATE = 4;
inline constexpr DWORD ERROR_SUCCESS = 0, ERROR_FILE_NOT_FOUND = 2, ERROR_MORE_DATA = 234;
inline constexpr DWORD TOKEN_QUERY = 8;
inline constexpr DWORD RRF_RT_REG_SZ = 2, RRF_SUBKEY_WOW6432KEY = 0x00020000, RRF_SUBKEY_WOW6464KEY = 0x00010000;
inline HKEY const HKEY_USERS = reinterpret_cast<HKEY>(1);
inline HKEY const HKEY_LOCAL_MACHINE = reinterpret_cast<HKEY>(2);

enum TOKEN_INFORMATION_CLASS {
  TokenUser = 1
};

enum WELL_KNOWN_SID_TYPE {
  WinLocalSystemSid = 22
};

struct SID_AND_ATTRIBUTES {
  PSID Sid;
  DWORD Attributes;
};

struct TOKEN_USER {
  SID_AND_ATTRIBUTES User;
};

HANDLE GetCurrentProcess();
HANDLE GetCurrentThread();
DWORD GetLastError();
BOOL OpenThreadToken(HANDLE, DWORD, BOOL, HANDLE *);
BOOL ImpersonateLoggedOnUser(HANDLE);
BOOL SetThreadToken(HANDLE *, HANDLE);
BOOL RevertToSelf();
DWORD GetCurrentProcessId();
BOOL ProcessIdToSessionId(DWORD, DWORD *);
BOOL OpenProcessToken(HANDLE, DWORD, HANDLE *);
BOOL CloseHandle(HANDLE);
BOOL GetTokenInformation(HANDLE, TOKEN_INFORMATION_CLASS, void *, DWORD, DWORD *);
BOOL IsWellKnownSid(PSID, WELL_KNOWN_SID_TYPE);
HLOCAL LocalFree(HLOCAL);
LSTATUS RegGetValueW(HKEY, LPCWSTR, LPCWSTR, DWORD, DWORD *, void *, DWORD *);
