/** @file src/platform/windows/steam_discovery.h
 * Read Steam registration without changing the host's identity or predefined hives.
 */
#pragma once

// clang-format off
#include <windows.h>
#include <sddl.h>
#include <wtsapi32.h>
// clang-format on

#include <algorithm>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace platf::steam::windows_discovery {
  namespace detail {
    struct close_token {
      void operator()(void *token) const {
        CloseHandle(token);
      }
    };

    struct free_wts {
      void operator()(void *data) const {
        WTSFreeMemory(data);
      }
    };

    struct free_local {
      void operator()(void *data) const {
        LocalFree(data);
      }
    };

    using token_t = std::unique_ptr<void, close_token>;

    struct user_t {
      std::wstring sid;
      bool system;
    };

    inline std::optional<user_t> token_user(HANDLE token) {
      DWORD bytes = 0;
      GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
      if (bytes < sizeof(TOKEN_USER) || bytes > 65536) {
        return std::nullopt;
      }
      std::vector<BYTE> data(bytes);
      if (!GetTokenInformation(token, TokenUser, data.data(), bytes, &bytes)) {
        return std::nullopt;
      }
      const auto sid = reinterpret_cast<const TOKEN_USER *>(data.data())->User.Sid;
      LPWSTR text = nullptr;
      if (!ConvertSidToStringSidW(sid, &text)) {
        return std::nullopt;
      }
      std::unique_ptr<void, free_local> release {text};
      return user_t {text, IsWellKnownSid(sid, WinLocalSystemSid) != FALSE};
    }

    inline std::optional<DWORD> interactive_session() {
      PWTS_SESSION_INFOW sessions = nullptr;
      DWORD count = 0;
      if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) {
        return std::nullopt;
      }
      std::unique_ptr<void, free_wts> release {sessions};
      std::vector<DWORD> eligible;
      for (DWORD i = 0; i < count; ++i) {
        if (sessions[i].SessionId == 0 || sessions[i].State != WTSActive) {
          continue;
        }
        LPWSTR username = nullptr;
        DWORD bytes = 0;
        const bool queried = WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessions[i].SessionId, WTSUserName, &username, &bytes) != FALSE;
        std::unique_ptr<void, free_wts> release_name {username};
        if (queried && username && bytes > sizeof(wchar_t) && username[0] != L'\0') {
          eligible.push_back(sessions[i].SessionId);
        }
      }
      DWORD own_session = 0;
      if (ProcessIdToSessionId(GetCurrentProcessId(), &own_session) && std::find(eligible.begin(), eligible.end(), own_session) != eligible.end()) {
        return own_session;
      }
      const auto console = WTSGetActiveConsoleSessionId();
      if (std::find(eligible.begin(), eligible.end(), console) != eligible.end()) {
        return console;
      }
      // Never select an arbitrary RDP/desktop user by enumeration order.
      return eligible.size() == 1 ? std::optional<DWORD> {eligible.front()} : std::nullopt;
    }

    inline std::optional<std::filesystem::path> registry_path(HKEY hive, const wchar_t *key, const wchar_t *value, DWORD view = 0) {
      const DWORD flags = RRF_RT_REG_SZ | view;
      DWORD bytes = 0;
      if (RegGetValueW(hive, key, value, flags, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes <= sizeof(wchar_t) || bytes > 65536 || bytes % sizeof(wchar_t) != 0) {
        return std::nullopt;
      }
      std::vector<wchar_t> text(bytes / sizeof(wchar_t) + 1, L'\0');
      if (RegGetValueW(hive, key, value, flags, nullptr, text.data(), &bytes) != ERROR_SUCCESS || text[0] == L'\0') {
        return std::nullopt;
      }
      return std::filesystem::path {text.data()};
    }
  }  // namespace detail

  // Keep user-controlled paths and library metadata under that user's access
  // checks, including UNC/reparse targets. Only this thread is impersonated;
  // the service process token and process-wide registry mappings are untouched.
  class discovery_scope {
  public:
    discovery_scope():
        parent_(active_) {
      if (parent_) {
        sid_ = parent_->sid_;
      } else {
        initialize();
      }
      // Publish only after construction's potentially throwing work. Even a
      // failed acquisition is authoritative for every nested scan in this call.
      active_ = this;
    }

    discovery_scope(const discovery_scope &) = delete;
    discovery_scope &operator=(const discovery_scope &) = delete;

    ~discovery_scope() {
      active_ = parent_;
      if (impersonating_ && !(previous_ ? SetThreadToken(nullptr, previous_.get()) : RevertToSelf())) {
        // Continuing with an unknown thread identity could corrupt later
        // access checks. This matches the host's fail-closed impersonation rule.
        std::terminate();
      }
    }

    const std::optional<std::wstring> &user_hive() const {
      return sid_;
    }

  private:
    void initialize() {
      HANDLE raw = nullptr;
      if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY | TOKEN_IMPERSONATE, TRUE, &raw)) {
        previous_.reset(raw);
      } else if (GetLastError() != ERROR_NO_TOKEN) {
        return;
      }
      detail::token_t process_token;
      if (!previous_) {
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) {
          return;
        }
        process_token.reset(raw);
      }
      const auto effective_user = detail::token_user(previous_ ? previous_.get() : process_token.get());
      if (!effective_user) {
        return;
      }
      if (!effective_user->system) {
        // A nested scan must retain the outer scan's chosen user even if
        // console/session ownership changes while metadata is being read.
        sid_ = effective_user->sid;
        return;
      }
      const auto session = detail::interactive_session();
      if (!session || !WTSQueryUserToken(*session, &raw)) {
        return;
      }
      selected_.reset(raw);
      const auto user = detail::token_user(selected_.get());
      if (user && !user->system) {
        // Copy before impersonating: a throwing allocation during construction
        // must never leave a changed thread identity without its destructor.
        sid_ = user->sid;
        if (!ImpersonateLoggedOnUser(selected_.get())) {
          sid_.reset();
          return;
        }
        impersonating_ = true;
      }
    }

    inline static thread_local discovery_scope *active_ = nullptr;
    discovery_scope *parent_;
    detail::token_t previous_;
    detail::token_t selected_;
    std::optional<std::wstring> sid_;
    bool impersonating_ = false;
  };

  template<class Read>
  auto read_user_source(Read read) -> decltype(read()) {
    discovery_scope scope;
    // A logged-out/missing user must not turn a deferred artwork read into
    // SYSTEM access to a path previously supplied by a desktop user.
    return scope.user_hive() ? read() : decltype(read()) {};
  }

  inline std::vector<std::filesystem::path> registry_roots(const discovery_scope &scope) {
    std::vector<std::filesystem::path> roots;
    if (const auto &sid = scope.user_hive()) {
      // An explicit loaded-user hive avoids cached SYSTEM HKCU and process-wide
      // RegOverridePredefKey races. Do not load profiles or fall back to .Default.
      const auto key = *sid + L"\\Software\\Valve\\Steam";
      if (auto path = detail::registry_path(HKEY_USERS, key.c_str(), L"SteamPath")) {
        roots.push_back(std::move(*path));
      }
    }
    // Steam's installer may register only InstallPath in the 32-bit view.
    // Query both views explicitly; filesystem validation/deduplication stays
    // with the shared discovery code, alongside its existing folder fallbacks.
    for (const auto view : {RRF_SUBKEY_WOW6432KEY, RRF_SUBKEY_WOW6464KEY}) {
      if (auto path = detail::registry_path(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", view)) {
        roots.push_back(std::move(*path));
      }
    }
    return roots;
  }
}  // namespace platf::steam::windows_discovery
