#pragma once
// Minimal SCM boundary for testing the production startup helper on non-Windows hosts.
#include <cstdint>
using DWORD = std::uint32_t;
using BOOL = int;
using SC_HANDLE = void *;
using LPCWSTR = const wchar_t *;
using LPWSTR = wchar_t *;
using LPDWORD = DWORD *;
inline constexpr DWORD ERROR_SUCCESS = 0;
inline constexpr DWORD ERROR_ACCESS_DENIED = 5;
inline constexpr DWORD ERROR_INVALID_DATA = 13;
inline constexpr DWORD ERROR_WRITE_FAULT = 29;
inline constexpr DWORD ERROR_INSUFFICIENT_BUFFER = 122;
inline constexpr DWORD ERROR_SERVICE_DOES_NOT_EXIST = 1060;
inline constexpr DWORD SC_MANAGER_CONNECT = 1;
inline constexpr DWORD SERVICE_QUERY_CONFIG = 1;
inline constexpr DWORD SERVICE_CHANGE_CONFIG = 2;
inline constexpr DWORD SERVICE_AUTO_START = 2;
inline constexpr DWORD SERVICE_DEMAND_START = 3;
inline constexpr DWORD SERVICE_DISABLED = 4;
inline constexpr DWORD SERVICE_NO_CHANGE = 0xffffffff;

struct QUERY_SERVICE_CONFIGW {
  DWORD dwServiceType;
  DWORD dwStartType;
  DWORD dwErrorControl;
  LPWSTR lpBinaryPathName;
  LPWSTR lpLoadOrderGroup;
  DWORD dwTagId;
  LPWSTR lpDependencies;
  LPWSTR lpServiceStartName;
  LPWSTR lpDisplayName;
};

SC_HANDLE OpenSCManagerW(LPCWSTR machine, LPCWSTR database, DWORD access);
SC_HANDLE OpenServiceW(SC_HANDLE manager, LPCWSTR name, DWORD access);
BOOL CloseServiceHandle(SC_HANDLE handle);
DWORD GetLastError();
BOOL QueryServiceConfigW(SC_HANDLE handle, QUERY_SERVICE_CONFIGW *config, DWORD bytes, LPDWORD needed);
BOOL ChangeServiceConfigW(SC_HANDLE handle, DWORD type, DWORD start, DWORD error, LPCWSTR binary, LPCWSTR group, LPDWORD tag, LPCWSTR dependencies, LPCWSTR account, LPCWSTR password, LPCWSTR display);
