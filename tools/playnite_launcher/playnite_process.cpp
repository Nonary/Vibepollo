#include "tools/playnite_launcher/playnite_process.h"

#include "src/logging.h"
#include "src/platform/windows/ipc/misc_utils.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <Psapi.h>
#include <shellapi.h>
#include <ShlObj.h>
#include <Shlwapi.h>
#include <string>
#include <system_error>
#include <utility>
#include <wincrypt.h>
#include <winsock2.h>
#include <windows.h>

namespace playnite_launcher::playnite {
  namespace {
    constexpr wchar_t cleanup_watchdog_filename[] = L"playnite-cleanup-watchdog.exe";
    constexpr wchar_t cleanup_watchdog_cache_directory[] = L"playnite-watchdogs";
    constexpr wchar_t cleanup_watchdog_cache_mutex[] = L"Local\\VibepolloPlayniteWatchdogCache-v1";
    constexpr wchar_t cleanup_lease_mutex_prefix[] = L"Local\\VibepolloPlayniteCleanupLease-v1-";

    std::optional<std::wstring> cleanup_lease_mutex_name(const std::string &install_dir_utf8) {
      if (install_dir_utf8.empty()) {
        return std::nullopt;
      }

      try {
        auto normalized = platf::dxgi::utf8_to_wide(install_dir_utf8);
        if (normalized.empty()) {
          return std::nullopt;
        }

        for (auto &ch : normalized) {
          if (ch == L'/') {
            ch = L'\\';
          } else if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
          }
        }
        while (
          normalized.size() > 1 &&
          normalized.back() == L'\\' &&
          !(normalized.size() == 3 && normalized[1] == L':')
        ) {
          normalized.pop_back();
        }

        std::uint64_t hash = 1469598103934665603ull;
        for (const auto ch : normalized) {
          hash ^= static_cast<std::uint16_t>(ch);
          hash *= 1099511628211ull;
        }
        return std::wstring(cleanup_lease_mutex_prefix) + std::to_wstring(hash);
      } catch (...) {
        return std::nullopt;
      }
    }

    struct staged_watchdog_t {
      std::filesystem::path path;
      std::wstring sha256;
    };

    class watchdog_cache_lock_t {
    public:
      watchdog_cache_lock_t() {
        handle_ = CreateMutexW(nullptr, FALSE, cleanup_watchdog_cache_mutex);
        if (!handle_) {
          return;
        }

        const DWORD wait_result = WaitForSingleObject(handle_, 10000);
        acquired_ = wait_result == WAIT_OBJECT_0 || wait_result == WAIT_ABANDONED;
        if (!acquired_) {
          CloseHandle(handle_);
          handle_ = nullptr;
        }
      }

      ~watchdog_cache_lock_t() {
        if (acquired_) {
          ReleaseMutex(handle_);
        }
        if (handle_) {
          CloseHandle(handle_);
        }
      }

      watchdog_cache_lock_t(const watchdog_cache_lock_t &) = delete;
      watchdog_cache_lock_t &operator=(const watchdog_cache_lock_t &) = delete;

      explicit operator bool() const {
        return acquired_;
      }

    private:
      HANDLE handle_ = nullptr;
      bool acquired_ = false;
    };

    std::optional<std::wstring> sha256_file(const std::filesystem::path &path) {
      HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr
      );
      if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
      }

      HCRYPTPROV provider = 0;
      HCRYPTHASH hash = 0;
      if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) ||
          !CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
        if (hash) {
          CryptDestroyHash(hash);
        }
        if (provider) {
          CryptReleaseContext(provider, 0);
        }
        CloseHandle(file);
        return std::nullopt;
      }

      bool succeeded = true;
      std::array<BYTE, 64 * 1024> buffer {};
      while (true) {
        DWORD bytes_read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr)) {
          succeeded = false;
          break;
        }
        if (bytes_read == 0) {
          break;
        }
        if (!CryptHashData(hash, buffer.data(), bytes_read, 0)) {
          succeeded = false;
          break;
        }
      }

      std::array<BYTE, 32> digest {};
      DWORD digest_size = static_cast<DWORD>(digest.size());
      if (succeeded && !CryptGetHashParam(hash, HP_HASHVAL, digest.data(), &digest_size, 0)) {
        succeeded = false;
      }

      CryptDestroyHash(hash);
      CryptReleaseContext(provider, 0);
      CloseHandle(file);
      if (!succeeded || digest_size != digest.size()) {
        return std::nullopt;
      }

      static constexpr wchar_t hex_digits[] = L"0123456789abcdef";
      std::wstring hex;
      hex.reserve(digest.size() * 2);
      for (BYTE byte : digest) {
        hex.push_back(hex_digits[byte >> 4]);
        hex.push_back(hex_digits[byte & 0x0F]);
      }
      return hex;
    }

    std::optional<std::filesystem::path> cleanup_watchdog_cache_root() {
      PWSTR local_app_data = nullptr;
      if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local_app_data)) ||
          !local_app_data) {
        return std::nullopt;
      }

      std::filesystem::path root = std::filesystem::path(local_app_data) / L"Vibepollo" / cleanup_watchdog_cache_directory;
      CoTaskMemFree(local_app_data);
      return root;
    }

    bool is_sha256_directory_name(const std::wstring &name) {
      if (name.size() != 64) {
        return false;
      }
      return std::all_of(name.begin(), name.end(), [](wchar_t ch) {
        return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f');
      });
    }

    void cleanup_stale_watchdog_versions(const std::filesystem::path &cache_root, const std::wstring &active_sha256) {
      std::error_code ec;
      std::filesystem::directory_iterator entries(cache_root, ec);
      if (ec) {
        return;
      }

      for (const auto &entry : entries) {
        const std::wstring name = entry.path().filename().wstring();
        if (name == active_sha256 || !is_sha256_directory_name(name)) {
          continue;
        }

        std::error_code remove_ec;
        const auto removed = std::filesystem::remove_all(entry.path(), remove_ec);
        if (!remove_ec && removed > 0) {
          BOOST_LOG(debug) << "Removed stale Playnite cleanup watchdog cache version '" << platf::dxgi::wide_to_utf8(name) << "'";
        }
      }
    }

    std::optional<staged_watchdog_t> stage_cleanup_watchdog(const std::filesystem::path &source_path) {
      auto cache_root = cleanup_watchdog_cache_root();
      if (!cache_root) {
        BOOST_LOG(warning) << "Cleanup watcher staging failed: unable to resolve LocalAppData";
        return std::nullopt;
      }

      for (int attempt = 0; attempt < 2; ++attempt) {
        auto source_sha256 = sha256_file(source_path);
        if (!source_sha256) {
          BOOST_LOG(warning) << "Cleanup watcher staging failed: unable to hash source executable";
          return std::nullopt;
        }

        const std::filesystem::path version_directory = *cache_root / *source_sha256;
        const std::filesystem::path staged_path = version_directory / cleanup_watchdog_filename;
        std::error_code ec;
        std::filesystem::create_directories(version_directory, ec);
        if (ec) {
          BOOST_LOG(warning) << "Cleanup watcher staging failed: unable to create cache directory: " << ec.message();
          return std::nullopt;
        }

        if (std::filesystem::exists(staged_path, ec) && !ec) {
          auto staged_sha256 = sha256_file(staged_path);
          if (staged_sha256 && *staged_sha256 == *source_sha256) {
            cleanup_stale_watchdog_versions(*cache_root, *source_sha256);
            return staged_watchdog_t {staged_path, *source_sha256};
          }

          std::filesystem::remove(staged_path, ec);
          if (ec) {
            BOOST_LOG(warning) << "Cleanup watcher staging failed: cached executable is invalid and cannot be replaced: " << ec.message();
            return std::nullopt;
          }
        }

        std::filesystem::path temporary_path = staged_path;
        temporary_path += L".staging-" + std::to_wstring(GetCurrentProcessId()) +
                          L"-" + std::to_wstring(GetCurrentThreadId()) +
                          L"-" + std::to_wstring(GetTickCount64());

        std::filesystem::copy_file(
          source_path,
          temporary_path,
          std::filesystem::copy_options::overwrite_existing,
          ec
        );
        if (ec) {
          BOOST_LOG(warning) << "Cleanup watcher staging failed: unable to copy executable: " << ec.message();
          return std::nullopt;
        }

        auto temporary_sha256 = sha256_file(temporary_path);
        if (!temporary_sha256 || *temporary_sha256 != *source_sha256) {
          std::filesystem::remove(temporary_path, ec);
          if (attempt == 0) {
            continue;
          }
          BOOST_LOG(warning) << "Cleanup watcher staging failed: copied executable hash did not match source";
          return std::nullopt;
        }

        if (!MoveFileExW(
              temporary_path.c_str(),
              staged_path.c_str(),
              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
            )) {
          const DWORD move_error = GetLastError();
          auto raced_sha256 = sha256_file(staged_path);
          std::filesystem::remove(temporary_path, ec);
          if (!raced_sha256 || *raced_sha256 != *source_sha256) {
            BOOST_LOG(warning) << "Cleanup watcher staging failed: atomic publish failed, error=" << move_error;
            return std::nullopt;
          }
        }

        auto staged_sha256 = sha256_file(staged_path);
        if (!staged_sha256 || *staged_sha256 != *source_sha256) {
          BOOST_LOG(warning) << "Cleanup watcher staging failed: published executable hash did not match source";
          return std::nullopt;
        }

        cleanup_stale_watchdog_versions(*cache_root, *source_sha256);
        return staged_watchdog_t {staged_path, *source_sha256};
      }

      return std::nullopt;
    }

    // Quote an argument for CommandLineToArgvW/CreateProcessW. Backslashes before
    // a closing quote must be doubled, including the trailing slash common in
    // Playnite install directories.
    std::wstring quote_command_line_argument(const std::wstring &argument) {
      std::wstring quoted;
      quoted.push_back(L'"');
      for (auto it = argument.begin();;) {
        std::size_t backslash_count = 0;
        while (it != argument.end() && *it == L'\\') {
          ++it;
          ++backslash_count;
        }

        if (it == argument.end()) {
          quoted.append(backslash_count * 2, L'\\');
          break;
        }
        if (*it == L'"') {
          quoted.append(backslash_count * 2 + 1, L'\\');
        } else {
          quoted.append(backslash_count, L'\\');
        }
        quoted.push_back(*it);
        ++it;
      }
      quoted.push_back(L'"');
      return quoted;
    }
  }  // namespace

  cleanup_lease_t::cleanup_lease_t(cleanup_lease_t &&other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

  cleanup_lease_t &cleanup_lease_t::operator=(cleanup_lease_t &&other) noexcept {
    if (this != &other) {
      if (handle_) {
        ReleaseMutex(handle_);
        CloseHandle(handle_);
      }
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }

  cleanup_lease_t::~cleanup_lease_t() {
    if (handle_) {
      ReleaseMutex(handle_);
      CloseHandle(handle_);
    }
  }

  std::optional<cleanup_lease_t> cleanup_lease_t::acquire(const std::string &install_dir_utf8) {
    const auto mutex_name = cleanup_lease_mutex_name(install_dir_utf8);
    if (!mutex_name) {
      return std::nullopt;
    }

    HANDLE handle = CreateMutexW(nullptr, FALSE, mutex_name->c_str());
    if (!handle) {
      BOOST_LOG(warning) << "Cleanup lease: failed to create mutex for install directory";
      return std::nullopt;
    }

    const DWORD wait_result = WaitForSingleObject(handle, 0);
    if (wait_result == WAIT_OBJECT_0 || wait_result == WAIT_ABANDONED) {
      return cleanup_lease_t {handle};
    }

    if (wait_result != WAIT_TIMEOUT) {
      BOOST_LOG(warning) << "Cleanup lease: failed to acquire mutex, error=" << wait_result;
    }
    CloseHandle(handle);
    return std::nullopt;
  }

  bool cleanup_lease_t::held_by_another_launcher(const std::string &install_dir_utf8) {
    const auto mutex_name = cleanup_lease_mutex_name(install_dir_utf8);
    if (!mutex_name) {
      return false;
    }

    HANDLE handle = CreateMutexW(nullptr, FALSE, mutex_name->c_str());
    if (!handle) {
      BOOST_LOG(warning) << "Cleanup lease: failed to inspect mutex for install directory";
      return false;
    }

    const DWORD wait_result = WaitForSingleObject(handle, 0);
    if (wait_result == WAIT_TIMEOUT) {
      CloseHandle(handle);
      return true;
    }
    if (wait_result == WAIT_OBJECT_0 || wait_result == WAIT_ABANDONED) {
      ReleaseMutex(handle);
    } else {
      BOOST_LOG(warning) << "Cleanup lease: failed to inspect mutex, error=" << wait_result;
    }
    CloseHandle(handle);
    return false;
  }

  bool is_playnite_running() {
    try {
      auto d = platf::dxgi::find_process_ids_by_name(L"Playnite.DesktopApp.exe");
      if (!d.empty()) {
        return true;
      }
      auto f = platf::dxgi::find_process_ids_by_name(L"Playnite.FullscreenApp.exe");
      if (!f.empty()) {
        return true;
      }
    } catch (...) {
    }
    return false;
  }

  std::wstring get_explorer_path() {
    WCHAR winDir[MAX_PATH] = {};
    if (GetWindowsDirectoryW(winDir, ARRAYSIZE(winDir)) > 0) {
      std::filesystem::path p(winDir);
      p /= L"explorer.exe";
      if (std::filesystem::exists(p)) {
        return p.wstring();
      }
    }
    WCHAR out[MAX_PATH] = {};
    DWORD rc = SearchPathW(nullptr, L"explorer.exe", nullptr, ARRAYSIZE(out), out, nullptr);
    if (rc > 0 && rc < ARRAYSIZE(out)) {
      return std::wstring(out);
    }
    return L"explorer.exe";
  }

  DWORD explorer_pid_from_windows() {
    DWORD pid = 0;
    HWND shell = GetShellWindow();
    if (shell) {
      GetWindowThreadProcessId(shell, &pid);
      if (pid) {
        return pid;
      }
    }
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray) {
      GetWindowThreadProcessId(tray, &pid);
    }
    return pid;
  }

  DWORD explorer_pid_from_process_list() {
    DWORD current_session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &current_session);
    try {
      auto pids = platf::dxgi::find_process_ids_by_name(L"explorer.exe");
      for (DWORD candidate : pids) {
        DWORD session = 0;
        ProcessIdToSessionId(candidate, &session);
        if (session == current_session) {
          return candidate;
        }
      }
      if (!pids.empty()) {
        return pids.front();
      }
    } catch (...) {
    }
    return 0;
  }

  HANDLE open_explorer_parent_handle() {
    DWORD pid = explorer_pid_from_windows();
    if (!pid) {
      pid = explorer_pid_from_process_list();
    }
    if (!pid) {
      return nullptr;
    }
    return OpenProcess(PROCESS_CREATE_PROCESS | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_DUP_HANDLE, FALSE, pid);
  }

  bool assign_parent_attributes(HANDLE parent, STARTUPINFOEXW &si, LPPROC_THREAD_ATTRIBUTE_LIST &attrList) {
    if (!parent) {
      return true;
    }
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    attrList = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, size));
    if (!attrList) {
      BOOST_LOG(warning) << "assign_parent_attributes: HeapAlloc failed";
      SetLastError(ERROR_OUTOFMEMORY);
      return false;
    }
    if (!InitializeProcThreadAttributeList(attrList, 1, 0, &size)) {
      DWORD err = GetLastError();
      BOOST_LOG(warning) << "assign_parent_attributes: InitializeProcThreadAttributeList failed: " << err;
      HeapFree(GetProcessHeap(), 0, attrList);
      attrList = nullptr;
      SetLastError(err);
      return false;
    }
    if (!UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, &parent, sizeof(parent), nullptr, nullptr)) {
      DWORD err = GetLastError();
      BOOST_LOG(warning) << "assign_parent_attributes: UpdateProcThreadAttribute failed: " << err;
      DeleteProcThreadAttributeList(attrList);
      HeapFree(GetProcessHeap(), 0, attrList);
      attrList = nullptr;
      SetLastError(err);
      return false;
    }
    si.lpAttributeList = attrList;
    return true;
  }

  void free_parent_attributes(LPPROC_THREAD_ATTRIBUTE_LIST attrList) {
    if (attrList) {
      DeleteProcThreadAttributeList(attrList);
      HeapFree(GetProcessHeap(), 0, attrList);
    }
  }

  bool launch_detached_command(const wchar_t *application, const std::wstring &cmd, STARTUPINFOEXW &si, PROCESS_INFORMATION &pi, DWORD flags) {
    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(0);
    return CreateProcessW(application, cmdline.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si.StartupInfo, &pi);
  }

  void close_process_info(PROCESS_INFORMATION &pi) {
    if (pi.hThread) {
      CloseHandle(pi.hThread);
      pi.hThread = nullptr;
    }
    if (pi.hProcess) {
      CloseHandle(pi.hProcess);
      pi.hProcess = nullptr;
    }
  }

  bool launch_uri_detached_parented(const std::wstring &uri) {
    // Try to parent the explorer-launched process to the shell; fall back to a vanilla launch if that fails.
    HANDLE parent = open_explorer_parent_handle();
    if (!parent) {
      BOOST_LOG(warning) << "Unable to open explorer.exe as parent; proceeding without parent override";
    }

    auto attempt_launch = [&](bool use_parent) -> std::pair<bool, DWORD> {
      STARTUPINFOEXW si {};
      si.StartupInfo.cb = sizeof(si);
      LPPROC_THREAD_ATTRIBUTE_LIST attrList = nullptr;

      if (use_parent) {
        if (!assign_parent_attributes(parent, si, attrList)) {
          // Attribute wiring failed; drop back to a non-parented launch.
          use_parent = false;
        }
      }

      std::wstring exe = get_explorer_path();
      std::wstring cmd = L"\"" + exe + L"\"";
      if (!uri.empty()) {
        cmd += L" ";
        cmd += uri;
      }

      DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW | DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB;
      if (use_parent) {
        flags |= EXTENDED_STARTUPINFO_PRESENT;
      }

      PROCESS_INFORMATION pi {};
      BOOL ok = launch_detached_command(exe.c_str(), cmd, si, pi, flags);
      DWORD err = ok ? ERROR_SUCCESS : GetLastError();

      free_parent_attributes(attrList);
      if (ok) {
        close_process_info(pi);
      }
      return {ok != FALSE, err};
    };

    auto result = attempt_launch(parent != nullptr);
    if (!result.first && result.second == ERROR_INVALID_HANDLE) {
      BOOST_LOG(warning) << "CreateProcessW(explorer uri) failed with ERROR_INVALID_HANDLE; retrying without explicit parent";
      result = attempt_launch(false);
    }

    if (parent) {
      CloseHandle(parent);
    }

    if (!result.first) {
      BOOST_LOG(warning) << "CreateProcessW(explorer uri) failed: " << result.second;
      return false;
    }
    return true;
  }

  std::wstring query_assoc_string(ASSOCSTR str, LPCWSTR extra) {
    std::array<wchar_t, 4096> buf {};
    DWORD sz = static_cast<DWORD>(buf.size());
    if (AssocQueryStringW(ASSOCF_NOTRUNCATE, str, L"playnite", extra, buf.data(), &sz) == S_OK && buf[0] != 0) {
      return std::wstring(buf.data());
    }
    return std::wstring();
  }

  std::wstring parse_command_executable(const std::wstring &command) {
    if (command.empty()) {
      return std::wstring();
    }
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(command.c_str(), &argc);
    std::wstring exe;
    if (argv && argc >= 1) {
      exe.assign(argv[0]);
    } else if (!command.empty() && command.front() == L'"') {
      auto pos = command.find(L'"', 1);
      if (pos != std::wstring::npos) {
        exe = command.substr(1, pos - 1);
      }
    }
    if (argv) {
      LocalFree(argv);
    }
    if (exe.empty()) {
      auto space = command.find(L' ');
      exe = (space == std::wstring::npos) ? command : command.substr(0, space);
    }
    return exe;
  }

  std::wstring query_playnite_executable_from_assoc() {
    auto exe = query_assoc_string(ASSOCSTR_EXECUTABLE, nullptr);
    if (!exe.empty()) {
      return exe;
    }
    auto command = query_assoc_string(ASSOCSTR_COMMAND, L"open");
    return parse_command_executable(command);
  }

  bool launch_executable_detached_parented_with_args(const std::wstring &exe_full_path, const std::wstring &args) {
    // Build command line: "<exe>" <args>
    std::wstring cmd = L"\"" + exe_full_path + L"\"";
    if (!args.empty()) {
      cmd += L" ";
      cmd += args;
    }
    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(0);

    std::wstring working_dir;
    try {
      std::filesystem::path p(exe_full_path);
      working_dir = p.parent_path().wstring();
    } catch (...) {
    }
    const wchar_t *cwd_ptr = working_dir.empty() ? nullptr : working_dir.c_str();

    auto attempt_launch = [&](bool use_parent) -> std::pair<bool, DWORD> {
      HANDLE parent = nullptr;
      LPPROC_THREAD_ATTRIBUTE_LIST attrList = nullptr;
      STARTUPINFOEXW si_ex {};
      STARTUPINFOW si_basic {};
      STARTUPINFOW *si_ptr = nullptr;
      DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
                    CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW | DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB;

      if (use_parent) {
        parent = open_explorer_parent_handle();
        if (!parent) {
          use_parent = false;
        } else if (!assign_parent_attributes(parent, si_ex, attrList)) {
          CloseHandle(parent);
          parent = nullptr;
          use_parent = false;
        } else {
          si_ex.StartupInfo.cb = sizeof(si_ex);
          si_ptr = &si_ex.StartupInfo;
        }
      }

      if (!use_parent) {
        flags &= ~EXTENDED_STARTUPINFO_PRESENT;
      }
      if (!si_ptr) {
        si_basic.cb = sizeof(si_basic);
        si_ptr = &si_basic;
      }

      PROCESS_INFORMATION pi {};
      BOOL ok = CreateProcessW(exe_full_path.c_str(), cmdline.data(), nullptr, nullptr, FALSE, flags, nullptr, cwd_ptr, si_ptr, &pi);
      DWORD err = ok ? ERROR_SUCCESS : GetLastError();
      if (pi.hThread) {
        CloseHandle(pi.hThread);
      }
      if (pi.hProcess) {
        CloseHandle(pi.hProcess);
      }
      if (attrList) {
        free_parent_attributes(attrList);
      }
      if (parent) {
        CloseHandle(parent);
      }
      return {ok != FALSE, err};
    };

    auto result = attempt_launch(true);
    if (!result.first && result.second == ERROR_INVALID_HANDLE) {
      BOOST_LOG(warning) << "CreateProcessW(executable with args) failed with ERROR_INVALID_HANDLE when using explorer parent; retrying without explicit parent";
      result = attempt_launch(false);
    }
    if (result.first) {
      return true;
    }
    BOOST_LOG(warning) << "CreateProcessW(executable with args) failed: " << result.second;
    return false;
  }

  bool launch_executable_detached_parented(const std::wstring &exe_full_path) {
    return launch_executable_detached_parented_with_args(exe_full_path, std::wstring());
  }

  bool spawn_cleanup_watchdog_process(const std::wstring &self_path, const std::string &install_dir_utf8, int exit_timeout_secs, bool fullscreen_flag, std::optional<DWORD> wait_for_pid) {
    try {
      const watchdog_cache_lock_t cache_lock;
      const auto staged_watchdog = cache_lock ? stage_cleanup_watchdog(self_path) : std::nullopt;
      const std::wstring watchdog_path = staged_watchdog ? staged_watchdog->path.wstring() : self_path;
      if (staged_watchdog) {
        BOOST_LOG(info) << "Cleanup watcher staged at '" << platf::dxgi::wide_to_utf8(watchdog_path)
                        << "' sha256=" << platf::dxgi::wide_to_utf8(staged_watchdog->sha256);
      } else if (!cache_lock) {
        BOOST_LOG(warning) << "Cleanup watcher staging skipped: unable to acquire the cache lock; upgrade survival is unavailable";
      } else {
        BOOST_LOG(warning) << "Cleanup watcher will run from the installed executable; upgrade survival is unavailable";
      }

      std::wstring wcmd = quote_command_line_argument(watchdog_path) + L" --do-cleanup";
      if (!install_dir_utf8.empty()) {
        wcmd += L" --install-dir " + quote_command_line_argument(platf::dxgi::utf8_to_wide(install_dir_utf8));
      }
      if (exit_timeout_secs > 0) {
        wcmd += L" --exit-timeout " + std::to_wstring(exit_timeout_secs);
      }
      if (fullscreen_flag) {
        wcmd += L" --fullscreen";
      }
      if (wait_for_pid) {
        wcmd += L" --wait-for-pid " + std::to_wstring(*wait_for_pid);
      }

      BOOST_LOG(info) << "Spawning cleanup watcher (fullscreen=" << fullscreen_flag << ", installDir='" << install_dir_utf8
                      << "' waitPid=" << (wait_for_pid ? std::to_string(*wait_for_pid) : std::string("none")) << ")";

      STARTUPINFOW si {};
      si.cb = sizeof(si);
      si.dwFlags |= STARTF_USESHOWWINDOW;
      si.wShowWindow = SW_HIDE;
      PROCESS_INFORMATION pi {};
      std::wstring cmdline = wcmd;
      DWORD flags_base = CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW | DETACHED_PROCESS;
      DWORD flags_try = flags_base | CREATE_BREAKAWAY_FROM_JOB;
      BOOL ok = CreateProcessW(watchdog_path.c_str(), cmdline.data(), nullptr, nullptr, FALSE, flags_try, nullptr, nullptr, &si, &pi);
      bool created_with_breakaway = ok != FALSE;
      if (!ok) {
        const DWORD breakaway_error = GetLastError();
        BOOST_LOG(warning) << "Cleanup watcher spawn with job breakaway failed, error=" << breakaway_error
                           << "; retrying without breakaway";
        ok = CreateProcessW(watchdog_path.c_str(), cmdline.data(), nullptr, nullptr, FALSE, flags_base, nullptr, nullptr, &si, &pi);
      }
      if (!ok) {
        BOOST_LOG(warning) << "Cleanup watcher spawn failed (fullscreen=" << fullscreen_flag << ") error=" << GetLastError();
        return false;
      }

      BOOL in_job = FALSE;
      const BOOL job_query_succeeded = IsProcessInJob(pi.hProcess, nullptr, &in_job);
      BOOST_LOG(info) << "Cleanup watcher spawned (fullscreen=" << fullscreen_flag << ", pid=" << pi.dwProcessId
                      << ", breakaway_create=" << (created_with_breakaway ? "true" : "false")
                      << ", in_job=" << (job_query_succeeded ? (in_job ? "true" : "false") : "unknown") << ")";
      if (pi.hThread) {
        CloseHandle(pi.hThread);
      }
      if (pi.hProcess) {
        CloseHandle(pi.hProcess);
      }
      return true;
    } catch (...) {
      BOOST_LOG(warning) << "Exception launching cleanup watcher";
      return false;
    }
  }

}  // namespace playnite_launcher::playnite
