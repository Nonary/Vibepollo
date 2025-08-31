/**
 * @file src/platform/windows/playnite_ipc.cpp
 * @brief Playnite plugin IPC server using Windows named pipes with anonymous handshake.
 */

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include "playnite_ipc.h"

#ifndef SUNSHINE_PLAYNITE_LAUNCHER
  #include "src/platform/windows/misc.h"
#endif
#include "src/utility.h"

#include <AclAPI.h>
#include <chrono>
#include <sddl.h>
#include <span>
#include <string>
#include <vector>
#include <windows.h>
#include <winsock2.h>
#include <WtsApi32.h>

using namespace std::chrono_literals;

#ifdef SUNSHINE_PLAYNITE_LAUNCHER
// The launcher never runs as SYSTEM; provide a local stub to avoid linking heavy deps.
namespace platf {
  inline bool is_running_as_system() {
    return false;
  }
}  // namespace platf
#endif

namespace platf::playnite {
  namespace {
    // Local UTF-16 to UTF-8 converter to avoid linking extra objects into tools
    static std::string to_utf8_local(const std::wstring &ws) {
      if (ws.empty()) {
        return std::string();
      }
      int size = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
      if (size <= 0) {
        return std::string();
      }
      std::string out;
      out.resize(static_cast<size_t>(size - 1));
      if (size > 1) {
        WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, out.data(), size, nullptr, nullptr);
      }
      return out;
    }

    static bool open_process_query_token(DWORD pid, HANDLE &out_token) {
      out_token = nullptr;
      winrt::handle hProc {OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
      if (!hProc) {
        return false;
      }
      HANDLE tok = nullptr;
      if (!OpenProcessToken(hProc.get(), TOKEN_QUERY, &tok)) {
        return false;
      }
      out_token = tok;
      return true;
    }

    static bool get_token_user_sid_string(HANDLE tok, std::wstring &sid_str) {
      sid_str.clear();
      DWORD len = 0;
      GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
      if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0) {
        return false;
      }
      auto buf = std::make_unique<uint8_t[]>(len);
      auto tu = reinterpret_cast<TOKEN_USER *>(buf.get());
      if (!GetTokenInformation(tok, TokenUser, tu, len, &len)) {
        return false;
      }
      LPWSTR sidW = nullptr;
      if (!ConvertSidToStringSidW(tu->User.Sid, &sidW)) {
        return false;
      }
      sid_str.assign(sidW);
      LocalFree(sidW);
      return true;
    }

    static bool get_token_session_id(HANDLE tok, DWORD &session_id) {
      DWORD len = sizeof(DWORD);
      return GetTokenInformation(tok, TokenSessionId, &session_id, len, &len) == TRUE;
    }

    static bool get_token_integrity_rid(HANDLE tok, DWORD &il_rid) {
      il_rid = 0;
      DWORD len = 0;
      GetTokenInformation(tok, TokenIntegrityLevel, nullptr, 0, &len);
      if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0) {
        return false;
      }
      auto buf = std::make_unique<uint8_t[]>(len);
      auto tml = reinterpret_cast<TOKEN_MANDATORY_LABEL *>(buf.get());
      if (!GetTokenInformation(tok, TokenIntegrityLevel, tml, len, &len)) {
        return false;
      }
      if (!IsValidSid(tml->Label.Sid)) {
        return false;
      }
      DWORD subAuthCount = *GetSidSubAuthorityCount(tml->Label.Sid);
      il_rid = *GetSidSubAuthority(tml->Label.Sid, subAuthCount - 1);
      return true;
    }

    static std::vector<DWORD> get_active_wts_session_ids() {
      std::vector<DWORD> ids;
      WTS_SESSION_INFO *pInfos = nullptr;
      DWORD count = 0;
      if (!WTSEnumerateSessions(WTS_CURRENT_SERVER_HANDLE, 0, 1, &pInfos, &count)) {
        return ids;
      }
      for (DWORD i = 0; i < count; ++i) {
        if (pInfos[i].State == WTSActive) {
          ids.push_back(pInfos[i].SessionId);
        }
      }
      if (pInfos) {
        WTSFreeMemory(pInfos);
      }
      return ids;
    }
  }  // namespace

  // Well-known public control pipe name that the Playnite plugin connects to when using
  // the legacy static mode. Newer flows should pass a dynamic control name.
  inline constexpr char kControlPipeName[] = "sunshine_playnite_connector";

  IpcServer::IpcServer():
      control_name_(kControlPipeName) {}

  IpcServer::IpcServer(const std::string &control_name):
      control_name_(control_name.empty() ? kControlPipeName : control_name) {}

  IpcServer::~IpcServer() {
    stop();
  }

  void IpcServer::start() {
    if (running_.exchange(true)) {
      return;
    }
    BOOST_LOG(info) << "Playnite IPC: starting server thread";
    worker_ = std::thread([this]() {
      run();
    });
  }

  void IpcServer::stop() {
    if (!running_.exchange(false)) {
      return;
    }
    BOOST_LOG(info) << "Playnite IPC: stopping server thread";
    if (pipe_) {
      pipe_->stop();
      pipe_.reset();
    }
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  void IpcServer::run() {
    while (running_.load()) {
      broken_.store(false);
      // Previously we deferred pipe startup until an "interactive" desktop session existed.
      // That prevented connections from RDP sessions and while the screen was locked.
      // Remove that dependency so IPC is available in those scenarios as well.
      if (no_session_logged_) {
        // If we logged a prior missing-session note, clear it once on first loop after start
        no_session_logged_ = false;
      }

      // Reduce log spam by only attempting to create/listen when Playnite is actually running
      if (!is_playnite_running()) {
        if (!no_playnite_logged_) {
          BOOST_LOG(info) << "Playnite IPC: Playnite not running; deferring pipe startup";
          no_playnite_logged_ = true;
        }
        std::this_thread::sleep_for(2s);
        continue;
      } else if (no_playnite_logged_) {
        BOOST_LOG(info) << "Playnite IPC: Playnite detected; enabling pipe handshake";
        no_playnite_logged_ = false;
      }
      auto data_pipe = wait_for_handshake_and_get_data_pipe();
      if (!data_pipe) {
        std::this_thread::sleep_for(500ms);
        continue;
      }
      auto on_msg = [this](std::span<const uint8_t> b) {
        accumulate_and_dispatch_lines(b);
      };
      auto on_err = [](const std::string &e) {
        BOOST_LOG(error) << "Playnite IPC error: " << e;
      };
      auto on_broken = [this]() {
        BOOST_LOG(warning) << "Playnite IPC: broken pipe";
        broken_.store(true);
      };
      pipe_ = std::make_unique<platf::dxgi::AsyncNamedPipe>(std::move(data_pipe));
      pipe_->start(on_msg, on_err, on_broken);
      active_.store(true);
      serve_connected_loop();
      if (pipe_) {
        pipe_->stop();
        pipe_.reset();
      }
      active_.store(false);
      if (!running_.load()) {
        break;
      }
      std::this_thread::sleep_for(300ms);
    }
  }

  void IpcServer::accumulate_and_dispatch_lines(std::span<const uint8_t> bytes) {
    if (!bytes.empty()) {
      recv_buffer_.append((const char *) bytes.data(), bytes.size());
    }
    size_t start = 0;
    while (true) {
      auto pos = recv_buffer_.find('\n', start);
      if (pos == std::string::npos) {
        break;
      }
      std::string line = recv_buffer_.substr(start, pos - start);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      start = pos + 1;
      auto non_ws = line.find_first_not_of(" \t\r\n");
      if (non_ws == std::string::npos) {
        continue;
      }
      if (handler_) {
        handler_(std::span<const uint8_t>((const uint8_t *) line.data(), line.size()));
      }
    }
    if (start > 0) {
      recv_buffer_.erase(0, start);
    }
  }

  std::unique_ptr<platf::dxgi::INamedPipe> IpcServer::wait_for_handshake_and_get_data_pipe() {
    platf::dxgi::AnonymousPipeFactory factory;
    // Tighten pre-connection security: use LocalSystem + INTERACTIVE DACL when running as SYSTEM,
    // otherwise LocalSystem + current user. Keeps WGC unaffected via factory seam.
    factory.set_security_descriptor_builder([](SECURITY_DESCRIPTOR &desc, PACL *out_pacl) -> bool {
      if (!InitializeSecurityDescriptor(&desc, SECURITY_DESCRIPTOR_REVISION)) {
        return false;
      }

      // Build well-known SIDs
      DWORD sid_size = SECURITY_MAX_SID_SIZE;
      BYTE sys_sid_buf[SECURITY_MAX_SID_SIZE] {};
      BYTE iu_sid_buf[SECURITY_MAX_SID_SIZE] {};
      PSID system_sid = sys_sid_buf;
      PSID interactive_sid = iu_sid_buf;

      if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system_sid, &sid_size)) {
        return false;
      }
      sid_size = SECURITY_MAX_SID_SIZE;
      if (!CreateWellKnownSid(WinInteractiveSid, nullptr, interactive_sid, &sid_size)) {
        return false;
      }

      // Optionally current user SID for user-mode
      PSID current_user_sid = nullptr;
      HANDLE tok = nullptr;
      if (!platf::is_running_as_system()) {
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
          return false;
        }
        DWORD len = 0;
        GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0) {
          CloseHandle(tok);
          return false;
        }
        auto buf = std::make_unique<uint8_t[]>(len);
        auto tu = reinterpret_cast<TOKEN_USER *>(buf.get());
        if (!GetTokenInformation(tok, TokenUser, tu, len, &len)) {
          CloseHandle(tok);
          return false;
        }
        current_user_sid = tu->User.Sid;
        // We'll keep buf alive until after SetSecurityDescriptorDacl by capturing it in lambda scope
        // but we only need the raw SID pointer for SetEntriesInAcl below (it duplicates SIDs inside ACL)
        CloseHandle(tok);
      }

      std::vector<EXPLICIT_ACCESS> eaList;
      auto allow_all = GENERIC_ALL;

      EXPLICIT_ACCESS ea_sys {};
      ea_sys.grfAccessPermissions = allow_all;
      ea_sys.grfAccessMode = SET_ACCESS;
      ea_sys.grfInheritance = NO_INHERITANCE;
      ea_sys.Trustee.TrusteeForm = TRUSTEE_IS_SID;
      ea_sys.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
      ea_sys.Trustee.ptstrName = reinterpret_cast<LPTSTR>(system_sid);
      eaList.push_back(ea_sys);

      if (platf::is_running_as_system()) {
        EXPLICIT_ACCESS ea_interactive = ea_sys;
        ea_interactive.Trustee.ptstrName = reinterpret_cast<LPTSTR>(interactive_sid);
        eaList.push_back(ea_interactive);
      } else {
        if (!current_user_sid || !IsValidSid(current_user_sid)) {
          return false;
        }
        EXPLICIT_ACCESS ea_user = ea_sys;
        ea_user.Trustee.TrusteeType = TRUSTEE_IS_USER;
        ea_user.Trustee.ptstrName = reinterpret_cast<LPTSTR>(current_user_sid);
        eaList.push_back(ea_user);
      }

      PACL rawDacl = nullptr;
      if (DWORD err = SetEntriesInAcl(static_cast<ULONG>(eaList.size()), eaList.data(), nullptr, &rawDacl); err != ERROR_SUCCESS) {
        return false;
      }
      if (!SetSecurityDescriptorDacl(&desc, TRUE, rawDacl, FALSE)) {
        LocalFree(rawDacl);
        return false;
      }
      *out_pacl = rawDacl;  // caller frees with LocalFree
      return true;
    });
    const char *ctl = control_name_.empty() ? kControlPipeName : control_name_.c_str();
    // Frequent poll; keep at debug to avoid spam
    BOOST_LOG(debug) << "Playnite IPC: waiting for handshake on control pipe '" << ctl << "'";
    auto pipe = factory.create_server(ctl);
    if (!pipe) {
      return nullptr;
    }
    pipe->wait_for_client_connection(15000);
    auto wp = dynamic_cast<platf::dxgi::WinPipe *>(pipe.get());
    if (!wp || !pipe->is_connected() || !validate_client_and_get_expected_sid(wp)) {
      return nullptr;
    }
    return pipe;
  }

  bool IpcServer::get_client_pid(platf::dxgi::WinPipe *wp, DWORD &pid) {
    if (!wp->get_client_process_id(pid)) {
      return false;
    }
    BOOST_LOG(info) << "Playnite IPC: client connected, PID=" << pid;
    return true;
  }

  bool IpcServer::is_playnite_process(DWORD pid) {
    auto v1 = platf::dxgi::find_process_ids_by_name(L"Playnite.DesktopApp.exe");
    auto v2 = platf::dxgi::find_process_ids_by_name(L"Playnite.FullscreenApp.exe");
    return std::find(v1.begin(), v1.end(), pid) != v1.end() || std::find(v2.begin(), v2.end(), pid) != v2.end();
  }

  bool IpcServer::is_playnite_running() {
    try {
      auto v1 = platf::dxgi::find_process_ids_by_name(L"Playnite.DesktopApp.exe");
      if (!v1.empty()) {
        return true;
      }
      auto v2 = platf::dxgi::find_process_ids_by_name(L"Playnite.FullscreenApp.exe");
      return !v2.empty();
    } catch (...) {
      return false;
    }
  }

  bool IpcServer::get_client_sid(platf::dxgi::WinPipe *wp, std::wstring &sid) {
    return wp->get_client_user_sid_string(sid);
  }

  bool IpcServer::get_expected_user_sid(std::wstring &sid) {
#ifdef SUNSHINE_PLAYNITE_LAUNCHER
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
      return false;
    }
    DWORD len = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
    auto buf = std::make_unique<uint8_t[]>(len);
    auto tu = reinterpret_cast<TOKEN_USER *>(buf.get());
    if (!GetTokenInformation(tok, TokenUser, tu, len, &len)) {
      CloseHandle(tok);
      return false;
    }
    LPWSTR sidW = nullptr;
    if (!ConvertSidToStringSidW(tu->User.Sid, &sidW)) {
      CloseHandle(tok);
      return false;
    }
    sid.assign(sidW);
    LocalFree(sidW);
    CloseHandle(tok);
    return true;
#else
    // Primary path: expected SID is the active user's token (works when running as SYSTEM)
    platf::dxgi::safe_token user_token;
    user_token.reset(platf::dxgi::retrieve_users_token(false));
    HANDLE tok = user_token.get();
    if (!tok) {
      // Fallback: when running as a regular user, WTSQueryUserToken often fails.
      // In that case, use our own process token as the expected SID.
      BOOST_LOG(debug) << "Playnite IPC: retrieve_users_token failed; falling back to current process token";
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        BOOST_LOG(warning) << "Playnite IPC: OpenProcessToken fallback failed: " << GetLastError();
        return false;
      }
    }
    auto close_fallback = util::fail_guard([&]() {
      // Only close the handle if it did not come from safe_token (fallback path)
      if (!user_token && tok) {
        CloseHandle(tok);
      }
    });

    DWORD len = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
    if (len == 0) {
      BOOST_LOG(warning) << "Playnite IPC: GetTokenInformation length query failed: " << GetLastError();
      return false;
    }
    auto buf = std::make_unique<uint8_t[]>(len);
    auto tu = reinterpret_cast<TOKEN_USER *>(buf.get());
    if (!GetTokenInformation(tok, TokenUser, tu, len, &len)) {
      BOOST_LOG(warning) << "Playnite IPC: GetTokenInformation(TokenUser) failed: " << GetLastError();
      return false;
    }
    LPWSTR sidW = nullptr;
    if (!ConvertSidToStringSidW(tu->User.Sid, &sidW)) {
      BOOST_LOG(warning) << "Playnite IPC: ConvertSidToStringSidW failed: " << GetLastError();
      return false;
    }
    sid.assign(sidW);
    LocalFree(sidW);
    return true;
#endif
  }

  bool IpcServer::validate_client_and_get_expected_sid(platf::dxgi::WinPipe *wp) {
    DWORD pid = 0;
    if (!get_client_pid(wp, pid)) {
      BOOST_LOG(warning) << "Playnite IPC: failed to obtain client PID; disconnecting";
      wp->disconnect();
      return false;
    }

    // Optional sanity: only allow Playnite processes, but do not rely on PID for auth
    if (!is_playnite_process(pid)) {
      BOOST_LOG(warning) << "Playnite IPC: rejecting non-Playnite client PID=" << pid;
      wp->disconnect();
      return false;
    }

    HANDLE tok = nullptr;
    if (!open_process_query_token(pid, tok)) {
      BOOST_LOG(warning) << "Playnite IPC: OpenProcess/OpenProcessToken failed for PID=" << pid << "; disconnecting";
      wp->disconnect();
      return false;
    }
    auto close_tok = util::fail_guard([&]() {
      if (tok) {
        CloseHandle(tok);
      }
    });

    // Reject tokens at System IL (guard talking to ourselves when running as SYSTEM)
    DWORD il_rid = 0;
    if (!get_token_integrity_rid(tok, il_rid)) {
      BOOST_LOG(warning) << "Playnite IPC: failed to read client token integrity; disconnecting";
      wp->disconnect();
      return false;
    }
    if (il_rid >= SECURITY_MANDATORY_SYSTEM_RID) {
      BOOST_LOG(warning) << "Playnite IPC: rejecting client with System IL";
      wp->disconnect();
      return false;
    }

    DWORD client_session = 0;
    if (!get_token_session_id(tok, client_session)) {
      BOOST_LOG(warning) << "Playnite IPC: failed to read client session ID; disconnecting";
      wp->disconnect();
      return false;
    }

    bool as_system = platf::is_running_as_system();
    if (as_system) {
      // Accept only if the caller is in an active interactive session (console or RDP)
      auto active_ids = get_active_wts_session_ids();
      bool ok = std::find(active_ids.begin(), active_ids.end(), client_session) != active_ids.end();
      if (!ok) {
        BOOST_LOG(warning) << "Playnite IPC: SYSTEM-mode reject: client session=" << client_session << " not active";
        wp->disconnect();
        return false;
      }
      return true;
    } else {
      // User-mode: accept only if same user SID
      std::wstring client_sid;
      if (!get_token_user_sid_string(tok, client_sid)) {
        BOOST_LOG(warning) << "Playnite IPC: failed to get client TokenUser; disconnecting";
        wp->disconnect();
        return false;
      }

      HANDLE myTok = nullptr;
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &myTok)) {
        BOOST_LOG(warning) << "Playnite IPC: OpenProcessToken(self) failed; disconnecting";
        wp->disconnect();
        return false;
      }
      auto close_my = util::fail_guard([&]() {
        if (myTok) {
          CloseHandle(myTok);
        }
      });
      std::wstring my_sid;
      if (!get_token_user_sid_string(myTok, my_sid)) {
        BOOST_LOG(warning) << "Playnite IPC: failed to get self TokenUser; disconnecting";
        wp->disconnect();
        return false;
      }
      if (client_sid != my_sid) {
        BOOST_LOG(warning) << "Playnite IPC: user-mode SID mismatch; client=" << to_utf8_local(client_sid)
                           << " expected=" << to_utf8_local(my_sid) << "; disconnecting";
        wp->disconnect();
        return false;
      }
      return true;
    }
  }

  void IpcServer::serve_connected_loop() {
    BOOST_LOG(info) << "Playnite IPC: data pipe established";
    while (running_.load() && pipe_->is_connected() && !broken_.load()) {
      std::this_thread::sleep_for(200ms);
    }
    BOOST_LOG(info) << "Playnite IPC: disconnected";
  }

  bool IpcServer::is_user_session_available() {
    // IPC availability should not depend on an "active desktop". Always report available
    // so callers do not gate pipe creation on console-desktop presence.
    return true;
  }

  bool IpcServer::send_json_line(const std::string &json) {
    if (!pipe_ || !pipe_->is_connected()) {
      BOOST_LOG(warning) << "Playnite IPC: send_json_line called but pipe not connected";
      return false;
    }
    // Append a newline delimiter for the plugin's line-based reader
    std::string payload = json;
    payload.push_back('\n');
    auto bytes = std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(payload.data()), payload.size());
    BOOST_LOG(debug) << "Playnite IPC: sending command (" << payload.size() << " bytes)";
    pipe_->send(bytes);
    return true;
  }

}  // namespace platf::playnite
