/**
 * @file src/aurora_link/main.cpp
 * @brief Aurora Link entry point — subcommand dispatch.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <thread>
#include <atomic>

#include <cstdio>
#include <cstring>
#include <string>

#include "aurora_link.h"

namespace aurora_link {
  static std::atomic<bool> stop_requested{false};
  bool helper_stopping() { return stop_requested.load(); }
  void request_helper_stop() { stop_requested.store(true); }
}

namespace {

  // The compatibility pad/mic helpers belong to this installation's host. Hold
  // its process handle, not just a PID, so a crash/forced service stop cannot
  // leave them bound to their UDP ports (and PID reuse cannot adopt another app).
  bool monitor_host_lifetime() {
    wchar_t self[MAX_PATH] = {};
    DWORD length = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (!length || length == MAX_PATH) return false;
    std::wstring host(self);
    auto slash = host.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    host.resize(slash + 1);
    host += L"sunshine.exe";
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    HANDLE owner = nullptr;
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) {
      CloseHandle(snapshot);
      return false;
    }
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
      if (_wcsicmp(entry.szExeFile, L"sunshine.exe") != 0) continue;
      DWORD candidate_session = 0;
      if (!ProcessIdToSessionId(entry.th32ProcessID, &candidate_session) || candidate_session != session) continue;
      HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
      if (!process) continue;
      wchar_t path[MAX_PATH] = {};
      DWORD size = MAX_PATH;
      if (QueryFullProcessImageNameW(process, 0, path, &size) && _wcsicmp(path, host.c_str()) == 0 &&
          WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
        if (owner) {
          CloseHandle(process);
          CloseHandle(owner);
          CloseHandle(snapshot);
          return false; // Two owners in this session is ambiguous; never adopt one arbitrarily.
        }
        owner = process;
        continue;
      }
      CloseHandle(process);
    }
    CloseHandle(snapshot);
    if (!owner) return false;
    auto event_name = L"Local\\EclipseLinkStop-" + std::to_wstring(GetCurrentProcessId());
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, event_name.c_str());
    if (!stop) { CloseHandle(owner); return false; }
    std::thread([owner, stop]() {
      HANDLE handles[] = {owner, stop};
      WaitForMultipleObjects(2, handles, FALSE, INFINITE);
      CloseHandle(owner);
      CloseHandle(stop);
      aurora_link::request_helper_stop();
      // Normal loops leave within one receive timeout and release ViGEm/audio.
      // Only a stuck helper needs the bounded last resort.
      Sleep(5000);
      ExitProcess(1);
    }).detach();
    return true;
  }

  int usage() {
    fprintf(stderr,
            "Eclipse Link — host companion\n"
            "\n"
            "  aurora-link cable on|off      show/hide the virtual audio cable (admin)\n"
            "  aurora-link rename-audio      give the virtual cable meaningful names\n"
            "  aurora-link run               run the pad host (UDP -> ViGEm)\n"
            "  aurora-link mic               run the mic host (Opus/RTP -> virtual cable)\n"
            "  aurora-link audio-default save|restore   remember/put back the default sink\n"
            "  aurora-link session-start     everything a stream needs, as one verb\n"
            "  aurora-link session-end       ...and put it all back\n");
    return 2;
  }

  bool is_on(const char *s) {
    return s != nullptr && (_stricmp(s, "on") == 0 || _stricmp(s, "start") == 0 || _stricmp(s, "1") == 0);
  }

}  // namespace

int main(int argc, char *argv[]) {
  if (argc < 2) {
    return usage();
  }

  const char *verb = argv[1];

  if (_stricmp(verb, "cable") == 0) {
    if (argc < 3) {
      return usage();
    }
    return aurora_link::set_cable_enabled(is_on(argv[2])) ? 0 : 1;
  }

  if (_stricmp(verb, "rename-audio") == 0) {
    // Applied at install time and on every session start, so a device that was
    // absent (the cable is hidden between sessions) still ends up named correctly.
    return aurora_link::rename_endpoints({
             {L"CABLE Input", L"Aurora Mic Bridge"},
             {L"CABLE In 16ch", L"Aurora Mic Bridge 16ch"},
             {L"CABLE Output", L"Aurora Mic"},
           }) ?
             0 :
             1;
  }

  if ((_stricmp(verb, "run") == 0 || _stricmp(verb, "mic") == 0) && !monitor_host_lifetime()) {
    fprintf(stderr, "Eclipse Link: no running host from this installation; refusing an orphan helper\n");
    return 1;
  }

  if (_stricmp(verb, "run") == 0) {
    return aurora_link::run_pad_host();
  }

  if (_stricmp(verb, "mic") == 0) {
    return aurora_link::run_mic_host();
  }

  if (_stricmp(verb, "audio-default") == 0) {
    if (argc < 3) {
      return usage();
    }
    // The path is optional: with none given, use the same per-session scratch file
    // session-start/-end use, which is the only case that matters in production.
    const std::wstring path = argc > 3 ? aurora_link::widen(argv[3]) : aurora_link::session_audio_snapshot();
    if (_stricmp(argv[2], "save") == 0) {
      return aurora_link::save_default_endpoint(path) ? 0 : 1;
    }
    if (_stricmp(argv[2], "restore") == 0) {
      return aurora_link::restore_default_endpoint(path) ? 0 : 1;
    }
    return usage();
  }

  // session-start / session-end are what the host's global_prep_cmd names. They must
  // never return non-zero: the host aborts the stream on a failed prep-cmd, and none
  // of this is worth refusing to play a game over.
  if (_stricmp(verb, "session-start") == 0) {
    return aurora_link::session_start();
  }

  if (_stricmp(verb, "session-end") == 0) {
    return aurora_link::session_end();
  }

  return usage();
}
