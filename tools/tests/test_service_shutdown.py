#!/usr/bin/env python3
"""Run the real service lifecycle against deterministic Windows API fakes.

No Windows SDK is needed: startup failures, graceful exit, and delayed forced
termination must all release the exclusive log before publishing STOPPED.
Usage: python tools/tests/test_service_shutdown.py [path/to/sunshinesvc.cpp]
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

source_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / "sunshinesvc.cpp"
source = source_path.read_text()
# Keep the production ServiceMain (and its cleanup helper when present) intact.
start = source.find("void ReportServiceStopped(")
if start < 0:
    start = source.index("VOID WINAPI ServiceMain(")
production = source[start:source.index("// This will run in a child process")]

fakes = r'''
#include <cassert>
#include <cstdint>
#include <iostream>
#include <set>
#define WINAPI
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
using DWORD = unsigned long;
using ULONGLONG = unsigned long long;
using HANDLE = void *;
using LPPROC_THREAD_ATTRIBUTE_LIST = void *;
using LPWSTR = wchar_t *;
using LPTSTR = char *;
using VOID = void;
const auto INVALID_HANDLE_VALUE = reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1));
constexpr DWORD NO_ERROR=0, TRUE=1, FALSE=0, INFINITE=0xffffffff,
 WAIT_OBJECT_0=0, WAIT_TIMEOUT=258, SERVICE_STOPPED=1, SERVICE_START_PENDING=2,
 SERVICE_RUNNING=4, SERVICE_WIN32_OWN_PROCESS=16, SERVICE_ACCEPT_STOP=1,
 SERVICE_ACCEPT_PRESHUTDOWN=256, SERVICE_ACCEPT_SESSIONCHANGE=128,
 STARTF_USESTDHANDLES=256, PROC_THREAD_ATTRIBUTE_HANDLE_LIST=1,
 PROC_THREAD_ATTRIBUTE_JOB_LIST=2, CREATE_UNICODE_ENVIRONMENT=1024,
 CREATE_NO_WINDOW=0x08000000, EXTENDED_STARTUPINFO_PRESENT=0x80000,
 ERROR_PROCESS_ABORTED=1067, ERROR_SHUTDOWN_IN_PROGRESS=1115,
 FAST_EXIT_WINDOW_MS=60000, CRASH_LOOP_RESTART_DELAY_MS=30000,
 CRASH_LOOP_FAST_EXIT_THRESHOLD=3;
constexpr auto SERVICE_NAME="test";
namespace platf::service_launch { constexpr auto launched_by_service_env_var=L"test"; }
struct SERVICE_STATUS {
 DWORD dwServiceType=0, dwServiceSpecificExitCode=0, dwWin32ExitCode=0,
 dwWaitHint=0, dwControlsAccepted=0, dwCheckPoint=0, dwCurrentState=0;
} service_status;
struct STARTUPINFOW { DWORD cb; LPWSTR lpDesktop; DWORD dwFlags; HANDLE hStdInput, hStdOutput, hStdError; };
using LPSTARTUPINFOW = STARTUPINFOW *;
struct STARTUPINFOEXW { STARTUPINFOW StartupInfo; LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList; };
struct PROCESS_INFORMATION { HANDLE hProcess, hThread; DWORD dwProcessId; };
HANDLE service_status_handle, stop_event, session_change_event;
std::set<HANDLE> handles;
HANDLE handle(int n) { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(n)); }
HANDLE acquire(int n) { auto h=handle(n); assert(handles.insert(h).second); return h; }
int scenario, event_count, last_error, stopped_count;
bool stopping, child_alive, killed, attributes_live, attributes_deleted;
void HandlerEx() {}
HANDLE RegisterServiceCtrlHandlerEx(...) { return handle(100); }
void ExitProcess(DWORD) { assert(false); }
DWORD GetLastError() { return last_error; }
bool CloseHandle(HANDLE h) { assert(handles.erase(h)==1); last_error=999; return true; }
bool SetServiceStatus(HANDLE, SERVICE_STATUS *s) {
 if (s->dwCurrentState==SERVICE_STOPPED) {
  // Model a replacement opening the log at the instant SCM can restart it.
  assert(!handles.count(handle(3)) && "old service still owns the exclusive log");
  assert(!child_alive && "child still owns its inherited log handle");
  assert(handles.empty() && !attributes_live);
  assert(s->dwWin32ExitCode==(scenario<=4 ? 123 : 0));
  assert(s->dwWaitHint==0 && s->dwCheckPoint==0 && s->dwControlsAccepted==0);
  assert(++stopped_count==1);
 }
 return true;
}
HANDLE CreateEventA(...) {
 ++event_count;
 if (scenario==event_count) { last_error=123; return nullptr; }
 return acquire(event_count);
}
HANDLE OpenLogFileHandle() {
 if (scenario==3) { last_error=123; return INVALID_HANDLE_VALUE; }
 return acquire(3);
}
LPPROC_THREAD_ATTRIBUTE_LIST AllocateProcThreadAttributeList(DWORD) {
 if (scenario==4) { last_error=123; return nullptr; }
 attributes_live=true; return handle(4);
}
void DeleteProcThreadAttributeList(void *) { assert(attributes_live); attributes_deleted=true; }
HANDLE GetProcessHeap() { return handle(101); }
bool HeapFree(HANDLE, DWORD, void *) { assert(attributes_deleted); attributes_live=false; return true; }
bool UpdateProcThreadAttribute(...) { return true; }
bool SetEnvironmentVariableW(...) { return true; }
DWORD WTSGetActiveConsoleSessionId() { return 1; }
HANDLE DuplicateTokenForSession(DWORD) { return acquire(5); }
HANDLE CreateJobObjectForChildProcess() { return acquire(6); }
bool CreateProcessAsUserW(HANDLE, const wchar_t *, void *, void *, void *, bool,
 DWORD, void *, void *, LPSTARTUPINFOW, PROCESS_INFORMATION *p) {
 p->hProcess=acquire(7); p->hThread=acquire(8); p->dwProcessId=1;
 child_alive=true; return true;
}
ULONGLONG GetTickCount64() { return 1; }
DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
 if (h==stop_event) {
  if (scenario==5) stopping=true; // stopped before a child was launched
  return stopping ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
 }
 assert(h==handle(7));
 if (ms==20000 && scenario==7) return WAIT_TIMEOUT;
 if (ms==INFINITE) assert(killed);
 child_alive=false; return WAIT_OBJECT_0;
}
DWORD WaitForMultipleObjects(...) { stopping=true; return WAIT_OBJECT_0; }
bool RunTerminationHelper(HANDLE, DWORD) { return scenario!=8; }
bool TerminateProcess(HANDLE, DWORD) { killed=true; return true; } // asynchronous
bool GetExitCodeProcess(HANDLE, DWORD *code) { *code=0; return true; }
bool SetEvent(HANDLE) { stopping=true; return true; }
'''
main = r'''
int main(int argc, char **argv) {
 scenario=std::stoi(argv[1]);
 ServiceMain(0, nullptr);
 assert(stopped_count==1);
 if (scenario==7 || scenario==8) assert(killed);
 std::cout << "scenario " << scenario << " passed\n";
}
'''
with tempfile.TemporaryDirectory(prefix="service-shutdown-test-") as tmp:
    cpp = Path(tmp) / "lifecycle.cpp"
    exe = Path(tmp) / "lifecycle"
    cpp.write_text(fakes + production + main)
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", str(cpp), "-o", str(exe)], check=True)
    results = [subprocess.run([str(exe), str(i)]).returncode for i in range(1, 9)]
    if any(results):
        sys.exit(1)
print("All 8 service lifecycle scenarios passed")
