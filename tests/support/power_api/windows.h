#pragma once
// The OS boundary only. Admission/authorization are tested separately against
// the production service and certificate store.
using BOOL = int;
using DWORD = unsigned long;
using HANDLE = void *;

struct LUID {
  unsigned long LowPart;
  long HighPart;
};

struct LUID_AND_ATTRIBUTES {
  LUID Luid;
  DWORD Attributes;
};

struct TOKEN_PRIVILEGES {
  DWORD PrivilegeCount;
  LUID_AND_ATTRIBUTES Privileges[1];
};

inline constexpr BOOL TRUE = 1, FALSE = 0;
inline constexpr DWORD TOKEN_ADJUST_PRIVILEGES = 0x20, TOKEN_QUERY = 8;
inline constexpr DWORD ERROR_SUCCESS = 0, ERROR_ACCESS_DENIED = 5, ERROR_NO_TOKEN = 1008, ERROR_NOT_ALL_ASSIGNED = 1300;
inline constexpr DWORD SE_PRIVILEGE_ENABLED = 2;
inline constexpr auto SE_SHUTDOWN_NAME = L"SeShutdownPrivilege";
inline constexpr int SecurityImpersonation = 2;
HANDLE GetCurrentThread();
BOOL OpenThreadToken(HANDLE, DWORD, BOOL, HANDLE *);
DWORD GetLastError();
void SetLastError(DWORD);
BOOL ImpersonateSelf(int);
BOOL RevertToSelf();
BOOL CloseHandle(HANDLE);
BOOL LookupPrivilegeValueW(const wchar_t *, const wchar_t *, LUID *);
BOOL AdjustTokenPrivileges(HANDLE, BOOL, TOKEN_PRIVILEGES *, DWORD, TOKEN_PRIVILEGES *, DWORD *);
