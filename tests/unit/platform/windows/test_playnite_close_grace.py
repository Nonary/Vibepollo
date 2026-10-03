"""Compile the actual close helper against deterministic Win32 process seams."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[4]
source = (repo / "src/platform/windows/playnite_integration.cpp").read_text()
begin = source.index("  static bool close_then_kill_by_name(")
end = source.index("  static bool resolve_playnite_launch_exe(", begin)
helper = source[begin:end].replace("std::chrono::steady_clock::now()", "Clock::now()")
compiler = shutil.which(os.environ.get("CXX", "g++"))
if not compiler:
    print("SKIP: C++ compiler unavailable")
    raise SystemExit(77)
prefix = r'''
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>
using DWORD = unsigned long;
using BOOL = int;
using HWND = int;
using LPARAM = std::intptr_t;
constexpr int TRUE=1, FALSE=0, WM_CLOSE=16;
constexpr DWORD PROCESS_TERMINATE=1, PROCESS_QUERY_LIMITED_INFORMATION=2, SYNCHRONIZE=4;
constexpr DWORD ERROR_INVALID_PARAMETER=87, WAIT_OBJECT_0=0, WAIT_TIMEOUT=258, WAIT_FAILED=0xffffffff;
int scenario, scans=0, kills=0, closes=0;
std::int64_t elapsed=0;
std::vector<DWORD> waits;
struct Clock {
  static std::chrono::steady_clock::time_point now() {
    return std::chrono::steady_clock::time_point{std::chrono::milliseconds{elapsed}};
  }
};
namespace winrt {
struct handle {
  int value;
  explicit handle(int value):value(value) {}
  explicit operator bool() const {return value!=0;}
  int get() const {return value;}
};
}
namespace platf {
std::string to_utf8(const std::wstring &) {return "Playnite";}
namespace dxgi {
std::vector<DWORD> find_process_ids_by_name(const wchar_t *) {
  ++scans;
  if (scenario==0 || (scans>1 && scenario!=4 && scenario!=7 && scenario!=8)) return {};
  return scenario==3 ? std::vector<DWORD>{42,43} : std::vector<DWORD>{42};
}
}
}
#define BOOST_LOG(level) std::cout
int OpenProcess(DWORD permissions, BOOL, DWORD pid) {
  if (permissions!=(PROCESS_TERMINATE|PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE)) std::abort();
  if (scenario==5 || scenario==6) return 0;
  return 100+pid; // Handle identifies original process, distinct from a reused PID.
}
DWORD GetLastError() {return scenario==6 ? ERROR_INVALID_PARAMETER : 5;}
void GetWindowThreadProcessId(HWND hwnd, DWORD *pid) {*pid=hwnd;}
void PostMessageW(HWND, int, int, int) {++closes;}
void EnumWindows(BOOL (*callback)(HWND,LPARAM), LPARAM context) {callback(42,context);callback(43,context);}
DWORD WaitForSingleObject(int, DWORD timeout) {
  waits.push_back(timeout);
  if (scenario==8) return WAIT_FAILED;
  if (kills) return WAIT_OBJECT_0;
  if (scenario==1) {elapsed+=1501;return WAIT_OBJECT_0;}
  elapsed+=timeout;
  return WAIT_TIMEOUT;
}
BOOL TerminateProcess(int handle, int) {
  if (handle!=142 && handle!=143) std::abort();
  ++kills;
  return scenario!=7;
}
'''
suffix = r'''
int main(int argc, char **argv) {
  scenario=std::stoi(argv[1]);
  bool result=close_then_kill_by_name(L"Playnite.DesktopApp.exe");
  bool expected=scenario<4 || scenario==6;
  if (result!=expected) return 1;
  if (scenario==0 && (!waits.empty() || kills || elapsed)) return 2;
  if (scenario==1 && (kills || waits.at(0)<1501 || closes!=1)) return 3;
  if (scenario==2 && (kills!=1 || elapsed!=15000 || waits.at(1)!=5000)) return 4;
  if (scenario==3 && (elapsed>15000 || waits.at(2)!=0)) return 5;
  if (scenario==4 && kills!=1) return 6; // Newly discovered replacement blocks installation.
  if ((scenario==5 || scenario==6 || scenario==8) && kills) return 7;
  std::cout << "PASS: graceful close scenario " << scenario << '\n';
}
'''
with tempfile.TemporaryDirectory(prefix="playnite-close-") as temporary:
    directory = Path(temporary)
    cpp = directory / "close.cpp"
    executable = directory / ("close.exe" if os.name == "nt" else "close")
    cpp.write_text(prefix + helper + suffix)
    subprocess.run([compiler, "-std=c++20", str(cpp), "-o", str(executable)], check=True)
    for scenario in range(9):
        subprocess.run([str(executable), str(scenario)], check=True, timeout=5)
