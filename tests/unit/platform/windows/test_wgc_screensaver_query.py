#!/usr/bin/env python3
"""Compile the production SPI query against strict inert Windows API boundaries.

No Windows settings, window stations or sessions are modified by this test.
"""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2] if len(sys.argv) > 2 else "c++"
source = (root / "src/platform/windows/ipc/misc_utils.cpp").read_text()
start = source.index("  std::optional<bool> query_screen_saver_running()")
opening = source.index("{", start)
depth, end = 1, opening + 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
function = source[start:end]
program = r"""
#include <cassert>
#include <cstdint>
#include <cwchar>
#include <optional>
#include <string>
using DWORD = std::uint32_t;
using BOOL = int;
using UINT = unsigned;
struct HWINSTA__ {};
using HWINSTA = HWINSTA__ *;
constexpr BOOL FALSE = 0, TRUE = 1;
constexpr int UOI_NAME = 2;
constexpr UINT SPI_GETSCREENSAVERRUNNING = 0x72;
DWORD own_session = 1, active_session = 1;
bool session_ok = true, station_ok = true, spi_ok = true, switch_console = false;
BOOL saver = FALSE;
std::wstring station = L"WinSta0";
int queries = 0;
DWORD GetCurrentProcessId() { return 42; }
BOOL ProcessIdToSessionId(DWORD id, DWORD *session) {
  assert(id == 42); *session = own_session; return session_ok;
}
DWORD WTSGetActiveConsoleSessionId() { return active_session; }
HWINSTA GetProcessWindowStation() { return reinterpret_cast<HWINSTA>(1); }
BOOL GetUserObjectInformationW(HWINSTA handle, int index, void *data, DWORD size, DWORD *needed) {
  assert(handle && index == UOI_NAME && size >= (station.size()+1)*sizeof(wchar_t));
  *needed = (station.size()+1)*sizeof(wchar_t);
  std::wcscpy(static_cast<wchar_t *>(data), station.c_str()); return station_ok;
}
int _wcsicmp(const wchar_t *a, const wchar_t *b) { return wcscasecmp(a, b); }
BOOL SystemParametersInfoW(UINT action, UINT value, void *out, UINT flags) {
  assert(action == SPI_GETSCREENSAVERRUNNING && value == 0 && flags == 0);
  ++queries; *static_cast<BOOL *>(out) = saver; if (switch_console) active_session = 2; return spi_ok;
}
""" + function + r"""
int main() {
  assert(query_screen_saver_running() == false);
  saver = TRUE; assert(query_screen_saver_running() == true);
  station = L"winsta0"; assert(query_screen_saver_running() == true);
  spi_ok = false; assert(!query_screen_saver_running().has_value());
  // Failed calls may have written TRUE: never use the output without success.
  spi_ok = true;
  switch_console = true; assert(!query_screen_saver_running().has_value());
  switch_console = false; active_session = 1;
  const int previous = queries;
  station = L"Service-0x0-3e7$"; assert(!query_screen_saver_running().has_value());
  station = L"WinSta0"; station_ok = false; assert(!query_screen_saver_running().has_value());
  station_ok = true; own_session = 0; assert(!query_screen_saver_running().has_value());
  own_session = 2; assert(!query_screen_saver_running().has_value());
  own_session = 1; active_session = 0xffffffff; assert(!query_screen_saver_running().has_value());
  active_session = 1; session_ok = false; assert(!query_screen_saver_running().has_value());
  assert(queries == previous);
}
"""
with tempfile.TemporaryDirectory(prefix="wgc-screensaver-query-") as directory:
    path = pathlib.Path(directory)
    (path / "query.cpp").write_text(program)
    subprocess.run([compiler, "-std=c++23", "-Wall", "-Wextra", "-Werror", str(path / "query.cpp"), "-o", str(path / "query")], check=True)
    subprocess.run([str(path / "query")], check=True)
print("Production SPI query: interactive context, true/false and failure cases passed")
