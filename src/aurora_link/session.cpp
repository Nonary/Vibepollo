/**
 * @file src/aurora_link/session.cpp
 * @brief The whole per-stream session, as two verbs the host can call directly.
 *
 * WHY THIS EXISTS: the host's global_prep_cmd pointed at a hand-built tree in
 * a local xlink-build folder -- a .cmd that called a .vbs that launched an .exe and
 * two Python scripts, plus a second .cmd for the audio cable. Every one of those is
 * an absolute path with a username in it, none of them are removed when the host is
 * uninstalled, and a second host needs the tree copied and its paths rewritten. That
 * is the entire reason aurora-link exists (see aurora_link.h), but the goal was not
 * actually reached while the prep-cmd still named those scripts.
 *
 * `session-start` and `session-end` are what the prep-cmd names instead. They do what
 * xlink-session.cmd did, with nothing outside this executable except dispwake.
 *
 * WHAT THE OLD SCRIPT DID THAT A NAIVE SWAP WOULD HAVE SILENTLY DROPPED -- both of
 * these were found by reading it rather than by anything failing loudly:
 *   - it SAVED AND RESTORED the default playback device around the session (the host
 *     parks the machine on its own virtual sink and does not put it back). That is
 *     audio_default.cpp now.
 *   - its stop path, and ONLY its stop path, ran the display sessionend hook. The host
 *     does not run the pause command when a session TERMINATES, so this is the sole
 *     thing that relights the physical panel after a quit. Dropping it would have
 *     left the monitor dark with no error anywhere.
 *
 * ORDERING IS NOT ARBITRARY. rename-audio runs BEFORE the cable is used because the
 * cable is hidden between sessions, so a device that was absent at install time still
 * ends up named. The pad/mic hosts start LAST, once the gate is open and the cable is
 * up, so their first packet has somewhere to go.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <shlwapi.h>

#include <cstdio>
#include <cerrno>
#include <string>
#include <vector>

#include "aurora_link.h"

namespace aurora_link {

  namespace {

    /**
     * Per-session scratch: the saved audio endpoint and the child pids.
     *
     * LOCALAPPDATA, not the install directory: the prep-cmd's session entry runs
     * UNELEVATED as the interactive user (the cable entry is the elevated one), so
     * The Program Files installation (including the retained compatibility path
     * C:\Program Files\Apollo) is not writable from here. Both verbs run as the same
     * user, so both see the same directory.
     */
    std::wstring state_dir() {
      wchar_t buf[MAX_PATH] = {0};
      DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
      std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring(L"C:\\Windows\\Temp");
      dir += L"\\Aurora Link";
      CreateDirectoryW(dir.c_str(), nullptr);
      return dir;
    }

    std::wstring state_file(const wchar_t *leaf) {
      return state_dir() + L"\\" + leaf;
    }

    /** Full path of this executable, so children are spawned from the same binary. */
    std::wstring self_path() {
      wchar_t buf[MAX_PATH] = {0};
      const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
      return (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring();
    }

    std::wstring self_dir() {
      std::wstring p = self_path();
      const size_t cut = p.find_last_of(L'\\');
      return cut == std::wstring::npos ? std::wstring() : p.substr(0, cut);
    }

    /**
     * Spawn `aurora-link <verb>` detached, with no window, logging to `log`.
     *
     * INHERIT EXACTLY ONE HANDLE, VIA PROC_THREAD_ATTRIBUTE_HANDLE_LIST. The obvious
     * version -- STARTF_USESTDHANDLES with bInheritHandles=TRUE -- HANGS THE CALLER,
     * and it hangs it in a way that would have stalled every stream start.
     * bInheritHandles=TRUE does not mean "inherit the handles I named in STARTUPINFO",
     * it means "inherit EVERY inheritable handle I hold". session-start is launched by
     * The host with its stdout on a pipe, so each long-running child ended up holding a
     * duplicate of that pipe. The pipe then has no EOF until the children exit -- and
     * they are the pad and mic hosts, which run for the whole session. The host WAITS for
     * a prep-cmd to finish, so the stream would sit there until the session it was
     * trying to start had already ended.
     *
     * Measured, not theorised: the first build did exactly this and a plain
     * `aurora-link session-start` from a shell never returned, while both children were
     * running correctly. Redirection has to be surgical for the same reason a daemon
     * closes its inherited descriptors.
     *
     * CREATE_NO_WINDOW rather than a minimised console: `start /min` used to leave a
     * visible minimised console per launch, and they piled up across sessions.
     *
     * @return the child pid, or 0.
     */
    DWORD spawn_child(const wchar_t *verb, const std::wstring &log) {
      SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
      HANDLE h = CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (h == INVALID_HANDLE_VALUE) {
        h = nullptr;
      }

      // CreateProcessW may modify the command line buffer, so it cannot be a literal.
      std::wstring cmd = L"\"" + self_path() + L"\" " + verb;
      std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
      mutable_cmd.push_back(L'\0');

      STARTUPINFOEXW six = {};
      six.StartupInfo.cb = sizeof(six);
      PROCESS_INFORMATION pi = {};
      std::vector<char> attr_buf;
      bool inherit = false;

      if (h != nullptr) {
        SIZE_T need = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &need);
        attr_buf.resize(need);
        auto *attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
        HANDLE only = h;
        if (need > 0 && InitializeProcThreadAttributeList(attrs, 1, 0, &need) &&
            UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      &only, sizeof(only), nullptr, nullptr)) {
          six.lpAttributeList = attrs;
          six.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
          six.StartupInfo.hStdOutput = h;
          six.StartupInfo.hStdError = h;
          six.StartupInfo.hStdInput = nullptr;
          inherit = true;
        }
      }

      // If the attribute list could not be built, run with NO inherited handles at all
      // and lose the log rather than risk the hang above. A missing log is a nuisance;
      // a stalled stream start is a broken product.
      const DWORD flags = CREATE_NO_WINDOW | (inherit ? EXTENDED_STARTUPINFO_PRESENT : 0u);
      const BOOL ok = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr,
                                     inherit, flags, nullptr, nullptr, &six.StartupInfo, &pi);

      if (six.lpAttributeList != nullptr) {
        DeleteProcThreadAttributeList(six.lpAttributeList);
      }
      if (h != nullptr) {
        CloseHandle(h);
      }
      if (!ok) {
        fwprintf(stderr, L"aurora-link: could not start '%ls' (err=%lu)\n", verb, GetLastError());
        return 0;
      }
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      return pi.dwProcessId;
    }

    /**
     * Kill the children recorded by the last session-start.
     *
     * PIDS ARE VERIFIED BY FULL IMAGE PATH BEFORE ANYTHING IS TERMINATED. A pid file can
     * outlive its processes (a crash, a reboot, a session that never ended cleanly)
     * and Windows reuses pids freely, so terminating a recorded pid blind is a real
     * risk of killing an unrelated process. Checking the image path costs one call
     * and removes that failure mode entirely.
     */
    bool kill_recorded_children() {
      const std::wstring pidfile = state_file(L"session.pids");
      FILE *f = _wfopen(pidfile.c_str(), L"r");
      if (f == nullptr) {
        return errno == ENOENT;
      }
      unsigned long pid = 0;
      std::vector<unsigned long> pids;
      while (fwscanf(f, L"%lu", &pid) == 1) {
        pids.push_back(pid);
      }
      fclose(f);
      std::vector<unsigned long> remaining;

      for (const unsigned long p : pids) {
        if (p == 0 || p == GetCurrentProcessId()) {
          continue;
        }
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(p));
        if (h == nullptr) {
          if (GetLastError() != ERROR_INVALID_PARAMETER) remaining.push_back(p);
          continue;  // gone, or retain the record if access was denied
        }
        wchar_t img[MAX_PATH] = {0};
        DWORD len = MAX_PATH;
        const bool named = QueryFullProcessImageNameW(h, 0, img, &len) != 0;
        if (!named) remaining.push_back(p);
        if (named && _wcsicmp(img, self_path().c_str()) == 0) {
          const auto event_name = L"Local\\EclipseLinkStop-" + std::to_wstring(p);
          HANDLE stop = OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name.c_str());
          if (stop) { SetEvent(stop); CloseHandle(stop); }
          if (WaitForSingleObject(h, 5000) != WAIT_OBJECT_0 &&
              (!TerminateProcess(h, 1) || WaitForSingleObject(h, 5000) != WAIT_OBJECT_0)) {
            remaining.push_back(p);
            fwprintf(stderr, L"aurora-link: child %lu did not confirm termination\n", p);
          }
        }
        CloseHandle(h);
      }
      if (remaining.empty()) {
        _wremove(pidfile.c_str());
      } else {
        FILE *pending = _wfopen(pidfile.c_str(), L"w");
        if (pending) {
          for (auto p : remaining) fwprintf(pending, L"%lu\n", p);
          fclose(pending);
        }
      }
      return remaining.empty();
    }

    /**
     * Relight the physical panel after a quit, via dispwake.
     *
     * Deliberately NOT reimplemented here: dispwake classifies displays by Windows
     * output technology and is the single owner of that decision, so nothing in this
     * stack names a monitor, an index, a resolution or a refresh rate.
     */
    void display_sessionend() {
      const std::wstring dispwake = self_dir() + L"\\dispwake.exe";
      if (GetFileAttributesW(dispwake.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"aurora-link: no dispwake.exe next to me - leaving the display alone\n");
        return;
      }
      // Let the host finish its own display revert first, or we race it and get undone.
      Sleep(3000);

      std::wstring cmd = L"\"" + dispwake + L"\" physical-wake";
      std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
      mutable_cmd.push_back(L'\0');
      STARTUPINFOW si = {};
      si.cb = sizeof(si);
      PROCESS_INFORMATION pi = {};
      if (CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
                         CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        if (WaitForSingleObject(pi.hProcess, 15000) != WAIT_OBJECT_0) {
          if (TerminateProcess(pi.hProcess, 1)) WaitForSingleObject(pi.hProcess, 5000);
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
      }
    }

    const std::vector<std::pair<std::wstring, std::wstring>> &cable_names() {
      static const std::vector<std::pair<std::wstring, std::wstring>> rules = {
        {L"CABLE Input", L"Aurora Mic Bridge"},
        {L"CABLE In 16ch", L"Aurora Mic Bridge 16ch"},
        {L"CABLE Output", L"Aurora Mic"},
      };
      return rules;
    }

  }  // namespace

  std::wstring session_audio_snapshot() {
    return state_file(L"audio-prev.txt");
  }

  std::wstring widen(const char *s) {
    if (s == nullptr || *s == '\0') {
      return {};
    }
    // argv from a narrow main() is in the ANSI code page, not UTF-8.
    const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 0) {
      return {};
    }
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, out.data(), n);
    return out;
  }

  int session_start() {
    // Clear anything a previous session left behind before starting new children --
    // the old script did the same, because an interrupted session leaves a pad host
    // holding the UDP port and the next one then silently receives nothing.
    if (!kill_recorded_children()) {
      fprintf(stderr, "aurora-link: previous children did not stop; refusing duplicate session helpers\n");
      return 0;
    }

    rename_endpoints(cable_names());

    save_default_endpoint(state_file(L"audio-prev.txt"));

    const DWORD pad = spawn_child(L"run", state_file(L"pad.log"));
    const DWORD mic = spawn_child(L"mic", state_file(L"mic.log"));

    FILE *f = _wfopen(state_file(L"session.pids").c_str(), L"w");
    if (f != nullptr) {
      fwprintf(f, L"%lu\n%lu\n", static_cast<unsigned long>(pad), static_cast<unsigned long>(mic));
      fclose(f);
    }
    wprintf(L"aurora-link: session started (pad pid %lu, mic pid %lu)\n",
            static_cast<unsigned long>(pad), static_cast<unsigned long>(mic));
    // Never fail a session on our account: the host aborts the stream on a non-zero
    // prep-cmd, and a missing microphone is not a reason to refuse to play a game.
    return 0;
  }

  int session_end() {
    const bool children_stopped = kill_recorded_children();

    restore_default_endpoint(state_file(L"audio-prev.txt"));
    display_sessionend();

    wprintf(children_stopped ? L"aurora-link: session ended\n" :
                              L"aurora-link: session cleanup incomplete; child records retained\n");
    return 0;
  }

}  // namespace aurora_link
