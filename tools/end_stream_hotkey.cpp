/** @file tools/end_stream_hotkey.cpp
 * @brief Optional end-stream shortcut on the console user's desktop.
 */
#include "src/platform/windows/end_stream_hotkey_protocol.h"

#include <cerrno>
#include <cstdlib>
#include <windows.h>
#include <wtsapi32.h>

namespace {
  bool parse_number(const char *value, unsigned long &result) {
    if (!value || !*value || *value == '-') {
      return false;
    }
    char *end = nullptr;
    errno = 0;
    result = std::strtoul(value, &end, 10);
    return errno == 0 && end && *end == '\0';
  }
}  // namespace

int main(int argc, char **argv) {
  using namespace platf::hotkey::end_stream;
  unsigned long host_pid, key, modifiers;
  if (argc != 5 || !parse_number(argv[2], host_pid) || host_pid == 0 || !parse_number(argv[3], key) || key > 0xFF || !parse_number(argv[4], modifiers) || !valid_binding(static_cast<int>(key), modifiers)) {
    return 1;
  }
  DWORD session = 0;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &session) || session == 0 || session != WTSGetActiveConsoleSessionId()) {
    return 2;
  }
  // Secure desktop input is intentionally outside this feature's contract.
  wchar_t station_name[256] {};
  DWORD needed = 0;
  if (!GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, station_name, sizeof(station_name), &needed) || _wcsicmp(station_name, L"WinSta0") != 0) {
    return 3;
  }
  HDESK desktop = OpenDesktopW(L"Default", 0, FALSE, DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS);
  if (!desktop) {
    return 3;
  }
  HDESK previous_desktop = GetThreadDesktop(GetCurrentThreadId());
  if (!SetThreadDesktop(desktop)) {
    CloseDesktop(desktop);
    return 3;
  }
  const auto release_desktop = [&] {
    SetThreadDesktop(previous_desktop);
    CloseDesktop(desktop);
  };
  HANDLE pipe = CreateFileA(argv[1], GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) {
    release_desktop();
    return 4;
  }
  ULONG server_pid = 0;
  if (!GetNamedPipeServerProcessId(pipe, &server_pid) || server_pid != host_pid) {
    CloseHandle(pipe);
    release_desktop();
    return 4;
  }
  const auto send = [&](message type) {
    const auto byte = static_cast<std::uint8_t>(type);
    DWORD written = 0;
    return WriteFile(pipe, &byte, 1, &written, nullptr) && written == 1;
  };
  MSG msg {};
  PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
  constexpr int hotkey_id = 1;
  if (!RegisterHotKey(nullptr, hotkey_id, modifiers | MOD_NOREPEAT, static_cast<UINT>(key))) {
    send(message::registration_failed);
    CloseHandle(pipe);
    release_desktop();
    return 5;
  }
  UINT_PTR timer = SetTimer(nullptr, 0, 1000, nullptr);
  bool healthy = timer != 0 && send(message::ready);
  while (healthy && GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (msg.message == WM_TIMER && msg.wParam == timer) {
      healthy = session == WTSGetActiveConsoleSessionId() && send(message::heartbeat);
    } else if (msg.message == WM_HOTKEY && msg.wParam == hotkey_id) {
      healthy = session == WTSGetActiveConsoleSessionId() && send(message::terminate);
    }
  }
  if (timer) {
    KillTimer(nullptr, timer);
  }
  UnregisterHotKey(nullptr, hotkey_id);
  CloseHandle(pipe);
  release_desktop();
  return 0;
}
