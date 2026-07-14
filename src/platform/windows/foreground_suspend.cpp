/**
 * @file src/platform/windows/foreground_suspend.cpp
 */

#include "foreground_suspend.h"

#include "utf_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cwchar>
#include <filesystem>
#include <unordered_set>
#include <utility>
#include <vector>

#include <winsock2.h>
#include <windows.h>
#include <sddl.h>
#include <wtsapi32.h>

namespace platf::foreground_suspend {
  namespace {
    constexpr DWORD REQUIRED_PROCESS_ACCESS = PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SUSPEND_RESUME | SYNCHRONIZE;
    constexpr DWORD PROCESS_PROTECTION_LEVEL_INFORMATION_CLASS = 7;
    constexpr DWORD PROTECTION_LEVEL_NONE_VALUE = 0xFFFFFFFEu;

    using nt_process_fn_t = LONG(NTAPI *)(HANDLE);
    using is_process_critical_fn_t = BOOL(WINAPI *)(HANDLE, PBOOL);
    using get_process_information_fn_t = BOOL(WINAPI *)(HANDLE, int, LPVOID, DWORD);

    struct protection_level_information_t {
      DWORD protection_level;
    };

    class handle_guard_t {
    public:
      explicit handle_guard_t(HANDLE handle = nullptr): handle_(handle) {}
      handle_guard_t(const handle_guard_t &) = delete;
      handle_guard_t &operator=(const handle_guard_t &) = delete;
      ~handle_guard_t() {
        if (handle_) {
          CloseHandle(handle_);
        }
      }
      [[nodiscard]] HANDLE get() const noexcept { return handle_; }
      [[nodiscard]] HANDLE release() noexcept {
        const auto handle = handle_;
        handle_ = nullptr;
        return handle;
      }

    private:
      HANDLE handle_;
    };

    std::string lower_ascii(std::string value) {
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
      });
      return value;
    }

    std::string normalized_path(std::string_view value) {
      if (value.empty()) {
        return {};
      }
      std::string text(value);
      std::replace(text.begin(), text.end(), '/', '\\');
      try {
        const auto wide = utf_utils::from_utf8(text);
        text = utf_utils::to_utf8(std::filesystem::path(wide).lexically_normal().wstring());
      } catch (...) {
      }
      std::replace(text.begin(), text.end(), '/', '\\');
      while (text.size() > 3 && text.back() == '\\') {
        text.pop_back();
      }
      return lower_ascii(std::move(text));
    }

    std::string normalized_basename(std::string_view value) {
      if (value.empty()) {
        return {};
      }
      try {
        return lower_ascii(utf_utils::to_utf8(std::filesystem::path(utf_utils::from_utf8(std::string(value))).filename().wstring()));
      } catch (...) {
      }
      std::string text(value);
      std::replace(text.begin(), text.end(), '/', '\\');
      const auto separator = text.find_last_of('\\');
      if (separator != std::string::npos) {
        text.erase(0, separator + 1);
      }
      return lower_ascii(std::move(text));
    }

    const std::unordered_set<std::string> &launcher_names() {
      static const std::unordered_set<std::string> names {
        "steam.exe", "steamwebhelper.exe", "steamservice.exe", "steamerrorreporter.exe",
        "playnite.desktopapp.exe", "playnite.fullscreenapp.exe", "playnite-launcher.exe",
        "battle.net.exe", "agent.exe", "blizzarderror.exe", "blizzardbrowser.exe",
        "eadesktop.exe", "ealauncher.exe", "eabackgroundservice.exe", "ealocalhostsvc.exe", "ealink.exe",
        "ubisoftconnect.exe", "upc.exe", "ubisoftgamelauncher.exe", "ubisoftgamelauncher64.exe", "ubisoftconnectinstaller.exe",
        "xboxpcapp.exe", "gamingservices.exe", "gamingservicesnet.exe", "gamebar.exe", "gamebarftserver.exe", "gamebarpresencewriter.exe",
        "epicgameslauncher.exe", "epicwebhelper.exe", "epicgameslauncherhelper.exe",
        "galaxyclient.exe", "galaxyclientservice.exe", "galaxycommunication.exe", "galaxyclienthelper.exe", "galaxyupdater.exe",
        "riotclientservices.exe", "riotclientux.exe", "riotclientuxrender.exe", "riotclientcrashhandler.exe",
        "amazongames.exe", "amazongamesui.exe", "amazongameservice.exe", "amazon games.exe", "amazon games ui.exe",
      };
      return names;
    }

    const std::unordered_set<std::string> &system_names() {
      static const std::unordered_set<std::string> names {
        "explorer.exe", "runtimebroker.exe", "shellexperiencehost.exe", "startmenuexperiencehost.exe",
        "searchhost.exe", "searchapp.exe", "textinputhost.exe", "applicationframehost.exe", "systemsettings.exe",
        "taskhostw.exe", "dwm.exe", "sihost.exe", "ctfmon.exe", "lockapp.exe", "securityhealthsystray.exe",
      };
      return names;
    }

    bool shell_window_class(HWND window, bool &query_succeeded) {
      wchar_t class_name[256] {};
      if (!GetClassNameW(window, class_name, static_cast<int>(std::size(class_name)))) {
        query_succeeded = false;
        return false;
      }
      query_succeeded = true;
      static constexpr std::array classes {
        L"Progman", L"WorkerW", L"SHELLDLL_DefView", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
      };
      return std::any_of(classes.begin(), classes.end(), [&](const wchar_t *candidate) {
        return _wcsicmp(class_name, candidate) == 0;
      });
    }

    std::optional<std::string> query_process_path(HANDLE process) {
      std::wstring buffer(32768, L'\0');
      DWORD size = static_cast<DWORD>(buffer.size());
      if (!QueryFullProcessImageNameW(process, 0, buffer.data(), &size) || size == 0) {
        return std::nullopt;
      }
      buffer.resize(size);
      try {
        return utf_utils::to_utf8(buffer);
      } catch (...) {
        return std::nullopt;
      }
    }

    std::optional<std::uint64_t> query_creation_time(HANDLE process) {
      FILETIME creation {}, exit {}, kernel {}, user {};
      if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
        return std::nullopt;
      }
      ULARGE_INTEGER value {};
      value.LowPart = creation.dwLowDateTime;
      value.HighPart = creation.dwHighDateTime;
      return value.QuadPart;
    }

    std::optional<bool> query_critical(HANDLE process) {
      const auto kernel = GetModuleHandleW(L"kernel32.dll");
      const auto function = kernel ? reinterpret_cast<is_process_critical_fn_t>(GetProcAddress(kernel, "IsProcessCritical")) : nullptr;
      BOOL critical = FALSE;
      if (!function || !function(process, &critical)) {
        return std::nullopt;
      }
      return critical != FALSE;
    }

    std::optional<bool> query_protected(HANDLE process) {
      const auto kernel = GetModuleHandleW(L"kernel32.dll");
      const auto function = kernel ? reinterpret_cast<get_process_information_fn_t>(GetProcAddress(kernel, "GetProcessInformation")) : nullptr;
      protection_level_information_t information {};
      if (!function || !function(process, static_cast<int>(PROCESS_PROTECTION_LEVEL_INFORMATION_CLASS), &information, sizeof(information))) {
        return std::nullopt;
      }
      return information.protection_level != PROTECTION_LEVEL_NONE_VALUE;
    }

    std::optional<std::string> query_token_sid(HANDLE token) {
      DWORD required = 0;
      GetTokenInformation(token, TokenUser, nullptr, 0, &required);
      if (required == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return std::nullopt;
      }
      std::vector<unsigned char> buffer(required);
      if (!GetTokenInformation(token, TokenUser, buffer.data(), required, &required)) {
        return std::nullopt;
      }
      const auto token_user = reinterpret_cast<const TOKEN_USER *>(buffer.data());
      LPWSTR sid_text = nullptr;
      if (!ConvertSidToStringSidW(token_user->User.Sid, &sid_text) || !sid_text) {
        return std::nullopt;
      }
      std::string result;
      try {
        result = utf_utils::to_utf8(sid_text);
      } catch (...) {
      }
      LocalFree(sid_text);
      if (result.empty()) {
        return std::nullopt;
      }
      return result;
    }

    std::optional<std::string> query_process_owner_sid(HANDLE process) {
      HANDLE raw_token = nullptr;
      if (!OpenProcessToken(process, TOKEN_QUERY, &raw_token)) {
        return std::nullopt;
      }
      handle_guard_t token(raw_token);
      return query_token_sid(token.get());
    }

    std::optional<std::string> query_active_user_sid(DWORD active_session) {
      HANDLE raw_token = nullptr;
      if (!WTSQueryUserToken(active_session, &raw_token)) {
        DWORD current_session = 0;
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &current_session) || current_session != active_session ||
            !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
          return std::nullopt;
        }
      }
      handle_guard_t token(raw_token);
      return query_token_sid(token.get());
    }

    std::string query_system_root() {
      std::wstring buffer(MAX_PATH, L'\0');
      UINT length = GetWindowsDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
      if (length == 0) {
        return {};
      }
      if (length >= buffer.size()) {
        buffer.resize(length + 1);
        length = GetWindowsDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
        if (length == 0 || length >= buffer.size()) {
          return {};
        }
      }
      buffer.resize(length);
      try {
        return utf_utils::to_utf8(buffer);
      } catch (...) {
        return {};
      }
    }

    nt_process_fn_t nt_function(const char *name) {
      const auto ntdll = GetModuleHandleW(L"ntdll.dll");
      return ntdll ? reinterpret_cast<nt_process_fn_t>(GetProcAddress(ntdll, name)) : nullptr;
    }

    resume_result_e resume_native_process(HANDLE process) noexcept {
      if (!process) {
        return resume_result_e::failed;
      }
      if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
        return resume_result_e::exited;
      }
      const auto resume = nt_function("NtResumeProcess");
      return resume && resume(process) >= 0 ? resume_result_e::resumed : resume_result_e::failed;
    }
  }  // namespace

  bool path_is_under_directory(std::string_view path, std::string_view directory) {
    const auto child = normalized_path(path);
    const auto parent = normalized_path(directory);
    if (child.empty() || parent.empty() || child.size() <= parent.size() || child.compare(0, parent.size(), parent) != 0) {
      return false;
    }
    return child[parent.size()] == '\\';
  }

  bool is_known_launcher_basename(std::string_view basename) {
    return launcher_names().contains(normalized_basename(basename));
  }

  bool is_known_system_basename(std::string_view basename) {
    return system_names().contains(normalized_basename(basename));
  }

  policy_result_t evaluate_policy(const identity_t &identity) {
    if (identity.pid == 0 || identity.pid == 4) {
      return {false, rejection_reason_e::invalid_pid};
    }
    if (identity.process_path.empty() || identity.executable_basename.empty() || !identity.creation_time ||
        !identity.session_id || !identity.active_session_id || identity.owner_sid.empty() || identity.active_user_sid.empty() ||
        identity.system_root.empty() || !identity.critical || !identity.protected_process) {
      return {false, rejection_reason_e::missing_identity};
    }
    if (*identity.critical) {
      return {false, rejection_reason_e::critical_process};
    }
    if (*identity.protected_process) {
      return {false, rejection_reason_e::protected_process};
    }
    if (*identity.session_id != *identity.active_session_id) {
      return {false, rejection_reason_e::wrong_session};
    }
    if (lower_ascii(identity.owner_sid) != lower_ascii(identity.active_user_sid)) {
      return {false, rejection_reason_e::wrong_user};
    }
    if (path_is_under_directory(identity.process_path, identity.system_root)) {
      return {false, rejection_reason_e::system_path};
    }
    if (is_known_system_basename(identity.executable_basename)) {
      return {false, rejection_reason_e::system_process};
    }
    if (is_known_launcher_basename(identity.executable_basename)) {
      return {false, rejection_reason_e::known_launcher};
    }
    return {true, rejection_reason_e::none};
  }

  std::string_view describe_rejection(rejection_reason_e reason) {
    switch (reason) {
      case rejection_reason_e::none: return "foreground process accepted";
      case rejection_reason_e::missing_foreground_window: return "foreground window is missing";
      case rejection_reason_e::invalid_foreground_window: return "foreground window is hidden, minimized, or could not be validated";
      case rejection_reason_e::shell_window: return "foreground window belongs to the Windows shell";
      case rejection_reason_e::invalid_pid: return "foreground PID is invalid or reserved";
      case rejection_reason_e::own_process: return "foreground process is Vibepollo";
      case rejection_reason_e::open_denied: return "foreground process denied the required limited suspend access";
      case rejection_reason_e::missing_identity: return "foreground process identity or safety information is incomplete";
      case rejection_reason_e::critical_process: return "target is a system/critical process";
      case rejection_reason_e::protected_process: return "target is a protected process";
      case rejection_reason_e::wrong_session: return "foreground process is outside the active interactive session";
      case rejection_reason_e::wrong_user: return "foreground process belongs to another user";
      case rejection_reason_e::system_path: return "foreground executable is under the Windows system directory";
      case rejection_reason_e::system_process: return "foreground process is a known Windows shell/UI process";
      case rejection_reason_e::known_launcher: return "foreground process is a known launcher";
      case rejection_reason_e::suspend_failed: return "Windows refused to suspend the foreground process";
    }
    return "foreground suspension was rejected";
  }

  target_selection_e select_target(const selection_input_t &input) {
    if (!input.placeholder && input.owned_group_live && !input.owned_group_is_launcher) {
      return target_selection_e::owned_process_group;
    }
    return input.foreground_allowed ? target_selection_e::foreground_process : target_selection_e::none;
  }

  owned_group_scan_e scan_owned_group(std::uintptr_t native_job_handle) {
    if (native_job_handle == 0) {
      return owned_group_scan_e::unknown;
    }
    DWORD buffer_length = sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST);
    std::vector<unsigned char> buffer(buffer_length);
    while (true) {
      auto list = reinterpret_cast<PJOBOBJECT_BASIC_PROCESS_ID_LIST>(buffer.data());
      DWORD required = 0;
      if (QueryInformationJobObject(reinterpret_cast<HANDLE>(native_job_handle), JobObjectBasicProcessIdList, list, buffer_length, &required)) {
        bool unknown = false;
        for (DWORD index = 0; index < list->NumberOfProcessIdsInList; ++index) {
          handle_guard_t process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(list->ProcessIdList[index])));
          if (!process.get()) {
            unknown = true;
            continue;
          }
          const auto path = query_process_path(process.get());
          if (!path) {
            unknown = true;
            continue;
          }
          if (is_known_launcher_basename(*path)) {
            return owned_group_scan_e::contains_launcher;
          }
        }
        return unknown ? owned_group_scan_e::unknown : owned_group_scan_e::clear;
      }
      if (GetLastError() != ERROR_MORE_DATA || required <= buffer_length) {
        return owned_group_scan_e::unknown;
      }
      buffer_length = required;
      buffer.resize(buffer_length);
    }
  }

  target_t::target_t(void *process_handle, identity_t identity, bool suspended):
      process_handle_(process_handle),
      identity_(std::move(identity)),
      suspended_(suspended) {}

  target_t::target_t(target_t &&other) noexcept:
      process_handle_(std::exchange(other.process_handle_, nullptr)),
      identity_(std::move(other.identity_)),
      suspended_(std::exchange(other.suspended_, false))
#ifdef SUNSHINE_TESTS
      , resume_override_(std::move(other.resume_override_))
#endif
  {}

  target_t &target_t::operator=(target_t &&other) noexcept {
    if (this != &other) {
      release();
      process_handle_ = std::exchange(other.process_handle_, nullptr);
      identity_ = std::move(other.identity_);
      suspended_ = std::exchange(other.suspended_, false);
#ifdef SUNSHINE_TESTS
      resume_override_ = std::move(other.resume_override_);
#endif
    }
    return *this;
  }

  target_t::~target_t() {
    release();
  }

  void target_t::release() noexcept {
    if (suspended_) {
      (void) resume();
    }
    if (process_handle_) {
      CloseHandle(reinterpret_cast<HANDLE>(process_handle_));
      process_handle_ = nullptr;
    }
  }

  std::uint32_t target_t::pid() const noexcept { return identity_.pid; }
  const std::string &target_t::process_path() const noexcept { return identity_.process_path; }
  const std::string &target_t::executable_basename() const noexcept { return identity_.executable_basename; }
  std::uint64_t target_t::creation_time() const noexcept { return identity_.creation_time.value_or(0); }
  std::uint32_t target_t::session_id() const noexcept { return identity_.session_id.value_or(0); }
  const std::string &target_t::owner_sid() const noexcept { return identity_.owner_sid; }
  bool target_t::suspended() const noexcept { return suspended_; }

  resume_result_e target_t::resume() noexcept {
    if (!suspended_) {
      return resume_result_e::resumed;
    }
#ifdef SUNSHINE_TESTS
    const auto result = resume_override_ ? resume_override_() : resume_native_process(reinterpret_cast<HANDLE>(process_handle_));
#else
    const auto result = resume_native_process(reinterpret_cast<HANDLE>(process_handle_));
#endif
    if (result != resume_result_e::failed) {
      suspended_ = false;
    }
    return result;
  }

#ifdef SUNSHINE_TESTS
  target_t target_t::for_tests(std::uint32_t pid, std::string basename, std::function<resume_result_e()> resume_override) {
    identity_t identity;
    identity.pid = pid;
    identity.executable_basename = std::move(basename);
    target_t target(nullptr, std::move(identity), true);
    target.resume_override_ = std::move(resume_override);
    return target;
  }
#endif

  acquire_result_t acquire_and_suspend_foreground() {
    acquire_result_t result;
    const auto window = GetForegroundWindow();
    if (!window) {
      result.reason = rejection_reason_e::missing_foreground_window;
      return result;
    }
    if (window == GetDesktopWindow() || window == GetShellWindow()) {
      result.reason = rejection_reason_e::shell_window;
      return result;
    }
    bool class_query_succeeded = false;
    if (shell_window_class(window, class_query_succeeded)) {
      result.reason = rejection_reason_e::shell_window;
      return result;
    }
    if (!class_query_succeeded || !IsWindowVisible(window) || IsIconic(window)) {
      result.reason = rejection_reason_e::invalid_foreground_window;
      return result;
    }

    DWORD pid = 0;
    if (!GetWindowThreadProcessId(window, &pid) || pid == 0 || pid == 4) {
      result.reason = rejection_reason_e::invalid_pid;
      return result;
    }
    result.pid = pid;
    if (pid == GetCurrentProcessId()) {
      result.reason = rejection_reason_e::own_process;
      return result;
    }

    handle_guard_t process(OpenProcess(REQUIRED_PROCESS_ACCESS, FALSE, pid));
    if (!process.get()) {
      result.reason = rejection_reason_e::open_denied;
      return result;
    }

    identity_t identity;
    identity.pid = pid;
    if (const auto path = query_process_path(process.get())) {
      identity.process_path = *path;
      identity.executable_basename = normalized_basename(*path);
      result.executable_basename = identity.executable_basename;
    }
    identity.creation_time = query_creation_time(process.get());
    DWORD session_id = 0;
    if (ProcessIdToSessionId(pid, &session_id)) {
      identity.session_id = session_id;
    }
    const DWORD active_session = WTSGetActiveConsoleSessionId();
    if (active_session != 0xFFFFFFFFu) {
      identity.active_session_id = active_session;
      identity.active_user_sid = query_active_user_sid(active_session).value_or("");
    }
    identity.owner_sid = query_process_owner_sid(process.get()).value_or("");
    identity.system_root = query_system_root();
    identity.critical = query_critical(process.get());
    identity.protected_process = query_protected(process.get());

    const auto policy = evaluate_policy(identity);
    if (!policy.allowed) {
      result.reason = policy.reason;
      return result;
    }

    const auto suspend = nt_function("NtSuspendProcess");
    if (!suspend || suspend(process.get()) < 0) {
      result.reason = rejection_reason_e::suspend_failed;
      return result;
    }

    result.target.emplace(process.release(), std::move(identity), true);
    result.reason = rejection_reason_e::none;
    return result;
  }

  bool foreground_target_slot_t::active() const noexcept {
    return target_.has_value() && target_->suspended();
  }

  const target_t *foreground_target_slot_t::target() const noexcept {
    return target_ ? &*target_ : nullptr;
  }

  bool foreground_target_slot_t::attach(target_t target) {
    if (active()) {
      return false;
    }
    target_.emplace(std::move(target));
    return true;
  }

  resume_result_e foreground_target_slot_t::recover() noexcept {
    if (!target_) {
      return resume_result_e::resumed;
    }
    const auto result = target_->resume();
    if (result != resume_result_e::failed) {
      target_.reset();
    }
    return result;
  }

}  // namespace platf::foreground_suspend
