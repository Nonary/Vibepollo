#pragma once
// API seam for executing the production helper on a non-Windows test host.
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>
using DWORD = std::uint32_t;
using ULONG = std::uint32_t;
using UINT = unsigned int;
using UINT_PTR = std::uintptr_t;
using HANDLE = void *;
using HDESK = void *;
using HWINSTA = void *;
using WPARAM = std::uintptr_t;
using LPARAM = std::intptr_t;

struct MSG {
  UINT message = 0;
  WPARAM wParam = 0;
  LPARAM lParam = 0;
};

#define FALSE 0
#define TRUE 1
#define INVALID_HANDLE_VALUE reinterpret_cast<HANDLE>(-1)
#define GENERIC_WRITE 0x40000000
#define OPEN_EXISTING 3
#define UOI_NAME 2
#define DESKTOP_READOBJECTS 1
#define DESKTOP_WRITEOBJECTS 128
#define PM_NOREMOVE 0
#define MOD_NOREPEAT 0x4000
#define WM_TIMER 0x113
#define WM_HOTKEY 0x312

namespace hotkey_test_api {
  struct state_t {
    DWORD session = 2;
    DWORD console = 2;
    ULONG server_pid = 99;
    std::wstring station = L"WinSta0";
    bool registration_succeeds = true;
    int registrations = 0;
    int unregistrations = 0;
    UINT modifiers = 0;
    UINT key = 0;
    std::vector<MSG> messages;
    std::vector<std::uint8_t> writes;
    std::size_t cursor = 0;
    bool switch_console_on_message = false;
  };

  inline state_t state;
}  // namespace hotkey_test_api

inline DWORD GetCurrentThreadId() {
  return 7;
}

inline HDESK GetThreadDesktop(DWORD) {
  return reinterpret_cast<void *>(4);
}

inline DWORD GetCurrentProcessId() {
  return 123;
}

inline int ProcessIdToSessionId(DWORD, DWORD *session) {
  *session = hotkey_test_api::state.session;
  return TRUE;
}

inline HWINSTA GetProcessWindowStation() {
  return reinterpret_cast<void *>(1);
}

inline int GetUserObjectInformationW(HANDLE, int, void *name, DWORD size, DWORD *) {
  const auto &station = hotkey_test_api::state.station;
  if ((station.size() + 1) * sizeof(wchar_t) > size) {
    return FALSE;
  }
  std::memcpy(name, station.c_str(), (station.size() + 1) * sizeof(wchar_t));
  return TRUE;
}

inline int _wcsicmp(const wchar_t *a, const wchar_t *b) {
  return wcscasecmp(a, b);
}

inline HDESK OpenDesktopW(const wchar_t *, DWORD, int, DWORD) {
  return reinterpret_cast<void *>(2);
}

inline int SetThreadDesktop(HDESK) {
  return TRUE;
}

inline int CloseDesktop(HDESK) {
  return TRUE;
}

inline HANDLE CreateFileA(const char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE) {
  return reinterpret_cast<void *>(3);
}

inline int GetNamedPipeServerProcessId(HANDLE, ULONG *pid) {
  *pid = hotkey_test_api::state.server_pid;
  return TRUE;
}

inline int CloseHandle(HANDLE) {
  return TRUE;
}

inline int WriteFile(HANDLE, const void *buffer, DWORD size, DWORD *written, void *) {
  const auto bytes = static_cast<const std::uint8_t *>(buffer);
  hotkey_test_api::state.writes.insert(hotkey_test_api::state.writes.end(), bytes, bytes + size);
  *written = size;
  return TRUE;
}

inline int PeekMessageW(MSG *, HANDLE, UINT, UINT, UINT) {
  return FALSE;
}

inline int RegisterHotKey(HANDLE, int, UINT modifiers, UINT key) {
  auto &s = hotkey_test_api::state;
  ++s.registrations;
  s.modifiers = modifiers;
  s.key = key;
  return s.registration_succeeds;
}

inline int UnregisterHotKey(HANDLE, int) {
  ++hotkey_test_api::state.unregistrations;
  return TRUE;
}

inline UINT_PTR SetTimer(HANDLE, UINT_PTR, UINT, void *) {
  return 10;
}

inline int KillTimer(HANDLE, UINT_PTR) {
  return TRUE;
}

inline int GetMessageW(MSG *message, HANDLE, UINT, UINT) {
  auto &s = hotkey_test_api::state;
  if (s.cursor == s.messages.size()) {
    return 0;
  }
  if (s.switch_console_on_message) {
    s.console = 3;
  }
  *message = s.messages[s.cursor++];
  return 1;
}
