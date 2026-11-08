#pragma once

/**
 * @file tools/display_helper_paths.h
 * @brief Shared path/singleton helpers for the display helper engines.
 *
 * Both the legacy engine and the v2 engine must agree on snapshot file locations,
 * the search order for state files, and the single-instance mutex so that either
 * engine can pick up the other's persisted state (drop-in engine switching).
 * Bodies are ported verbatim from tools/display_settings_helper.cpp.
 */

#ifdef _WIN32

  #include <algorithm>
  #include <cwchar>
  #include <filesystem>
  #include <mutex>
  #include <optional>
  #include <string>
  #include <string_view>
  #include <vector>

  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <windows.h>
  #include <AclAPI.h>
  #include <combaseapi.h>
  #include <sddl.h>
  #include <shlobj.h>
  #include <shellapi.h>

  #include "src/platform/windows/display_restore_policy.h"

  #include "src/platform/windows/ipc/misc_utils.h"

namespace display_helper_paths {
  inline bool is_private_system_dacl(
    PACL dacl,
    PSID system_sid,
    PSID administrators_sid,
    const DWORD required_access,
    const bool require_inheritance
  ) {
    if (!dacl || !IsValidAcl(dacl)) {
      return false;
    }
    ACL_SIZE_INFORMATION info {};
    if (!GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation)) {
      return false;
    }
    bool system_full = false;
    bool administrators_full = false;
    for (DWORD index = 0; index < info.AceCount; ++index) {
      void *raw_ace = nullptr;
      if (!GetAce(dacl, index, &raw_ace) || !raw_ace) {
        return false;
      }
      const auto *header = static_cast<const ACE_HEADER *>(raw_ace);
      if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
        return false;
      }
      const auto *ace = static_cast<const ACCESS_ALLOWED_ACE *>(raw_ace);
      PSID sid = const_cast<DWORD *>(&ace->SidStart);
      const bool full_access = (ace->Mask & required_access) == required_access;
      const bool inherited_by_children = !require_inheritance ||
                                         (header->AceFlags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)) ==
                                           (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE);
      if (!full_access || !inherited_by_children) {
        return false;
      }
      if (EqualSid(sid, system_sid)) {
        system_full = true;
      } else if (EqualSid(sid, administrators_sid)) {
        administrators_full = true;
      } else {
        return false;
      }
    }
    return system_full && administrators_full;
  }

  inline bool is_trusted_system_object(const std::filesystem::path &path, const bool require_protected_dacl) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      return false;
    }
    const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT |
                        ((attributes & FILE_ATTRIBUTE_DIRECTORY) ? FILE_FLAG_BACKUP_SEMANTICS : 0);
    HANDLE handle = CreateFileW(
      path.c_str(),
      READ_CONTROL,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      flags,
      nullptr
    );
    if (handle == INVALID_HANDLE_VALUE) {
      return false;
    }

    FILE_ATTRIBUTE_TAG_INFO tag_info {};
    const bool is_reparse = !GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag_info, sizeof(tag_info)) ||
                            (tag_info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD security_status = GetSecurityInfo(
      handle,
      SE_FILE_OBJECT,
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
      &owner,
      nullptr,
      &dacl,
      nullptr,
      &descriptor
    );
    CloseHandle(handle);

    bool trusted = !is_reparse && security_status == ERROR_SUCCESS && owner && descriptor;
    BYTE system_sid_storage[SECURITY_MAX_SID_SIZE] {};
    BYTE administrators_sid_storage[SECURITY_MAX_SID_SIZE] {};
    DWORD system_sid_size = sizeof(system_sid_storage);
    DWORD administrators_sid_size = sizeof(administrators_sid_storage);
    trusted = trusted &&
              CreateWellKnownSid(WinLocalSystemSid, nullptr, system_sid_storage, &system_sid_size) &&
              CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators_sid_storage, &administrators_sid_size) &&
              (EqualSid(owner, system_sid_storage) || EqualSid(owner, administrators_sid_storage));
    if (trusted && require_protected_dacl) {
      SECURITY_DESCRIPTOR_CONTROL control = 0;
      DWORD revision = 0;
      trusted = GetSecurityDescriptorControl(descriptor, &control, &revision) &&
                (control & SE_DACL_PROTECTED) != 0;
    }
    if (trusted) {
      trusted = is_private_system_dacl(
        dacl,
        system_sid_storage,
        administrators_sid_storage,
        FILE_ALL_ACCESS,
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
      );
    }
    if (descriptor) {
      LocalFree(descriptor);
    }
    return trusted;
  }

  inline bool create_private_system_directory(const std::filesystem::path &path, DWORD *create_error_out = nullptr) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"O:SYG:SYD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)",
          SDDL_REVISION_1,
          &descriptor,
          nullptr)) {
      return false;
    }
    SECURITY_ATTRIBUTES attributes {sizeof(attributes), descriptor, FALSE};
    const BOOL created = CreateDirectoryW(path.c_str(), &attributes);
    const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
    LocalFree(descriptor);
    if (create_error_out) {
      *create_error_out = create_error;
    }
    return created && is_trusted_system_object(path, true);
  }

  inline bool remove_untrusted_snapshot_objects(const std::filesystem::path &root) {
    constexpr std::wstring_view names[] = {
      L"display_golden_restore.json",
      L"display_golden_restore_status.json",
      L"display_session_current.json",
      L"display_session_previous.json",
      L"vibeshine_state.json",
    };
    for (const auto name : names) {
      const auto path = root / name;
      const DWORD attributes = GetFileAttributesW(path.c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) {
          continue;
        }
        return false;
      }
      if (!is_trusted_system_object(path, false)) {
        if (!DeleteFileW(path.c_str())) {
          return false;
        }
      }
    }
    return true;
  }

  inline bool is_trusted_snapshot_locator_key(HKEY key) {
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD security_status = GetSecurityInfo(
      reinterpret_cast<HANDLE>(key),
      SE_REGISTRY_KEY,
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
      &owner,
      nullptr,
      &dacl,
      nullptr,
      &descriptor
    );

    BYTE system_sid_storage[SECURITY_MAX_SID_SIZE] {};
    BYTE administrators_sid_storage[SECURITY_MAX_SID_SIZE] {};
    DWORD system_sid_size = sizeof(system_sid_storage);
    DWORD administrators_sid_size = sizeof(administrators_sid_storage);
    bool trusted = security_status == ERROR_SUCCESS && owner && descriptor &&
                   CreateWellKnownSid(WinLocalSystemSid, nullptr, system_sid_storage, &system_sid_size) &&
                   CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators_sid_storage, &administrators_sid_size) &&
                   (EqualSid(owner, system_sid_storage) || EqualSid(owner, administrators_sid_storage));
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    trusted = trusted && GetSecurityDescriptorControl(descriptor, &control, &revision) &&
              (control & SE_DACL_PROTECTED) != 0 &&
              is_private_system_dacl(
                dacl,
                system_sid_storage,
                administrators_sid_storage,
                KEY_ALL_ACCESS,
                false
              );
    if (descriptor) {
      LocalFree(descriptor);
    }
    return trusted;
  }

  inline HKEY open_private_snapshot_locator_key() {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"O:SYG:SYD:P(A;;KA;;;SY)(A;;KA;;;BA)",
          SDDL_REVISION_1,
          &descriptor,
          nullptr)) {
      return nullptr;
    }
    SECURITY_ATTRIBUTES attributes {sizeof(attributes), descriptor, FALSE};
    HKEY key = nullptr;
    const LSTATUS status = RegCreateKeyExW(
      HKEY_LOCAL_MACHINE,
      L"SOFTWARE\\Nonary\\Vibepollo\\DisplayHelper",
      0,
      nullptr,
      REG_OPTION_NON_VOLATILE,
      KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_CREATE_SUB_KEY | READ_CONTROL | KEY_WOW64_64KEY,
      &attributes,
      &key,
      nullptr
    );
    LocalFree(descriptor);
    if (status != ERROR_SUCCESS || !key) {
      return nullptr;
    }
    if (!is_trusted_snapshot_locator_key(key)) {
      RegCloseKey(key);
      return nullptr;
    }
    return key;
  }

  inline HKEY try_acquire_snapshot_initialization_lock(HKEY locator_key) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"O:SYG:SYD:P(A;;KA;;;SY)(A;;KA;;;BA)",
          SDDL_REVISION_1,
          &descriptor,
          nullptr)) {
      return nullptr;
    }
    SECURITY_ATTRIBUTES attributes {sizeof(attributes), descriptor, FALSE};
    HKEY lock_key = nullptr;
    DWORD disposition = 0;
    const LSTATUS status = RegCreateKeyExW(
      locator_key,
      L"Initialization",
      0,
      nullptr,
      REG_OPTION_VOLATILE,
      KEY_QUERY_VALUE | KEY_SET_VALUE | READ_CONTROL | DELETE,
      &attributes,
      &lock_key,
      &disposition
    );
    LocalFree(descriptor);
    if (status != ERROR_SUCCESS || !lock_key) {
      return nullptr;
    }
    if (disposition != REG_CREATED_NEW_KEY || !is_trusted_snapshot_locator_key(lock_key)) {
      RegCloseKey(lock_key);
      return nullptr;
    }
    const DWORD owner_pid = GetCurrentProcessId();
    FILETIME created {}, exited {}, kernel {}, user {};
    const bool have_creation_time = GetProcessTimes(
      GetCurrentProcess(),
      &created,
      &exited,
      &kernel,
      &user
    ) != FALSE;
    const ULONGLONG owner_creation_time =
      (static_cast<ULONGLONG>(created.dwHighDateTime) << 32U) | created.dwLowDateTime;
    if (RegSetValueExW(
          lock_key,
          L"OwnerPid",
          0,
          REG_DWORD,
          reinterpret_cast<const BYTE *>(&owner_pid),
          sizeof(owner_pid)) != ERROR_SUCCESS ||
        !have_creation_time ||
        RegSetValueExW(
          lock_key,
          L"OwnerCreationTime",
          0,
          REG_QWORD,
          reinterpret_cast<const BYTE *>(&owner_creation_time),
          sizeof(owner_creation_time)) != ERROR_SUCCESS) {
      RegCloseKey(lock_key);
      RegDeleteKeyW(locator_key, L"Initialization");
      return nullptr;
    }
    return lock_key;
  }

  struct SnapshotInitializerState {
    HKEY stale_key = nullptr;
    bool active = false;
  };

  inline SnapshotInitializerState inspect_snapshot_initializer(HKEY locator_key) {
    HKEY lock_key = nullptr;
    if (RegOpenKeyExW(
          locator_key,
          L"Initialization",
          0,
          KEY_QUERY_VALUE | READ_CONTROL,
          &lock_key) != ERROR_SUCCESS || !lock_key) {
      return {};
    }
    if (!is_trusted_snapshot_locator_key(lock_key)) {
      return {.stale_key = lock_key, .active = true};
    }
    DWORD owner_pid = 0;
    DWORD type = 0;
    DWORD bytes = sizeof(owner_pid);
    bool valid_owner = RegQueryValueExW(
                         lock_key,
                         L"OwnerPid",
                         nullptr,
                         &type,
                         reinterpret_cast<BYTE *>(&owner_pid),
                         &bytes) == ERROR_SUCCESS &&
                       type == REG_DWORD && bytes == sizeof(owner_pid) && owner_pid != 0;
    ULONGLONG owner_creation_time = 0;
    type = 0;
    bytes = sizeof(owner_creation_time);
    valid_owner = valid_owner &&
                  RegQueryValueExW(
                    lock_key,
                    L"OwnerCreationTime",
                    nullptr,
                    &type,
                    reinterpret_cast<BYTE *>(&owner_creation_time),
                    &bytes) == ERROR_SUCCESS &&
                  type == REG_QWORD && bytes == sizeof(owner_creation_time) && owner_creation_time != 0;
    if (!valid_owner) {
      return {.stale_key = lock_key, .active = false};
    }
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, owner_pid);
    if (!process) {
      if (GetLastError() == ERROR_ACCESS_DENIED) {
        return {.stale_key = lock_key, .active = true};
      }
      return {.stale_key = lock_key, .active = false};
    }
    const DWORD wait = WaitForSingleObject(process, 0);
    FILETIME created {}, exited {}, kernel {}, user {};
    const bool same_process = GetProcessTimes(process, &created, &exited, &kernel, &user) != FALSE &&
                              (((static_cast<ULONGLONG>(created.dwHighDateTime) << 32U) |
                                created.dwLowDateTime) == owner_creation_time);
    CloseHandle(process);
    return {.stale_key = lock_key, .active = wait == WAIT_TIMEOUT && same_process};
  }

  inline bool is_hex_digit(const wchar_t ch) {
    return (ch >= L'0' && ch <= L'9') ||
           (ch >= L'a' && ch <= L'f') ||
           (ch >= L'A' && ch <= L'F');
  }

  inline bool is_snapshot_directory_basename(const std::wstring_view name) {
    if (name == L"Sunshine") {
      return true;
    }
    constexpr std::wstring_view prefix = L"Sunshine-State-";
    constexpr size_t guid_length = 38;
    if (!name.starts_with(prefix) || name.size() != prefix.size() + guid_length) {
      return false;
    }
    const auto guid = name.substr(prefix.size());
    for (size_t index = 0; index < guid.size(); ++index) {
      const bool dash = index == 9 || index == 14 || index == 19 || index == 24;
      if ((index == 0 && guid[index] != L'{') ||
          (index == guid.size() - 1 && guid[index] != L'}') ||
          (dash && guid[index] != L'-') ||
          (index != 0 && index != guid.size() - 1 && !dash && !is_hex_digit(guid[index]))) {
        return false;
      }
    }
    return true;
  }

  inline std::optional<std::wstring> read_snapshot_locator(HKEY key) {
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, L"SnapshotDirectory", nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        type != REG_SZ || bytes < sizeof(wchar_t) || bytes > 256 * sizeof(wchar_t) ||
        (bytes % sizeof(wchar_t)) != 0) {
      return std::nullopt;
    }
    std::vector<wchar_t> value(bytes / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(
          key,
          L"SnapshotDirectory",
          nullptr,
          &type,
          reinterpret_cast<BYTE *>(value.data()),
          &bytes) != ERROR_SUCCESS ||
        value.empty() || value.back() != L'\0') {
      return std::nullopt;
    }
    const std::wstring result(value.data(), value.size() - 1);
    if (result.find(L'\0') != std::wstring::npos || !is_snapshot_directory_basename(result)) {
      return std::nullopt;
    }
    return result;
  }

  inline bool write_snapshot_locator(HKEY key, const std::wstring_view basename) {
    if (!is_snapshot_directory_basename(basename)) {
      return false;
    }
    const std::wstring value(basename);
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    return RegSetValueExW(
             key,
             L"SnapshotDirectory",
             0,
             REG_SZ,
             reinterpret_cast<const BYTE *>(value.c_str()),
             bytes) == ERROR_SUCCESS;
  }

  inline std::optional<std::filesystem::path> validated_snapshot_locator_path(
    const std::filesystem::path &program_data,
    HKEY key
  ) {
    const auto basename = read_snapshot_locator(key);
    if (!basename) {
      return std::nullopt;
    }
    const auto path = program_data / *basename;
    if (!is_trusted_system_object(path, true) || !remove_untrusted_snapshot_objects(path)) {
      return std::nullopt;
    }
    return path;
  }

  inline std::optional<std::filesystem::path> create_random_private_snapshot_dir(
    const std::filesystem::path &program_data,
    HKEY key
  ) {
    for (unsigned int attempt = 0; attempt < 32; ++attempt) {
      GUID guid {};
      wchar_t guid_text[40] {};
      if (FAILED(CoCreateGuid(&guid)) || StringFromGUID2(guid, guid_text, ARRAYSIZE(guid_text)) != 39) {
        return std::nullopt;
      }
      const std::wstring basename = L"Sunshine-State-" + std::wstring(guid_text);
      const auto path = program_data / basename;
      DWORD create_error = ERROR_SUCCESS;
      if (!create_private_system_directory(path, &create_error)) {
        if (create_error == ERROR_ALREADY_EXISTS) {
          continue;
        }
        return std::nullopt;
      }
      if (!write_snapshot_locator(key, basename)) {
        RemoveDirectoryW(path.c_str());
        return std::nullopt;
      }
      // Re-read the published value so a concurrent initializer cannot make
      // this process use a directory different from the machine locator.
      return validated_snapshot_locator_path(program_data, key);
    }
    return std::nullopt;
  }

  inline std::optional<std::filesystem::path> private_system_snapshot_dir() {
    static std::mutex mutex;
    static std::optional<std::filesystem::path> cached;
    const std::lock_guard lock(mutex);
    if (cached) {
      return cached;
    }

    std::wstring program_data(MAX_PATH, L'\0');
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, SHGFP_TYPE_CURRENT, program_data.data()))) {
      return std::nullopt;
    }
    program_data.resize(wcslen(program_data.c_str()));
    const auto program_data_path = std::filesystem::path(program_data);
    HKEY key = open_private_snapshot_locator_key();
    if (!key) {
      return std::nullopt;
    }

    cached = validated_snapshot_locator_path(program_data_path, key);
    for (unsigned int recovery_round = 0; !cached && recovery_round < 2; ++recovery_round) {
      for (unsigned int wait_attempt = 0; !cached && wait_attempt < 100; ++wait_attempt) {
        HKEY initialization_lock = try_acquire_snapshot_initialization_lock(key);
        if (initialization_lock) {
          // Recheck after winning the atomic registry-subkey election: a prior
          // initializer may have published just before releasing its lock.
          cached = validated_snapshot_locator_path(program_data_path, key);
          if (!cached) {
            // Preserve prior state only when the legacy directory is already
            // an exact private SYSTEM/Admin object. A hostile canonical path
            // is ignored rather than renamed, repaired, or traversed.
            const auto legacy = program_data_path / L"Sunshine";
            if (is_trusted_system_object(legacy, true) && remove_untrusted_snapshot_objects(legacy) &&
                write_snapshot_locator(key, L"Sunshine")) {
              cached = validated_snapshot_locator_path(program_data_path, key);
            }
          }
          if (!cached) {
            cached = create_random_private_snapshot_dir(program_data_path, key);
          }
          RegCloseKey(initialization_lock);
          RegDeleteKeyW(key, L"Initialization");
          break;
        }

        Sleep(50);
        cached = validated_snapshot_locator_path(program_data_path, key);
      }
      if (!cached) {
        auto initializer = inspect_snapshot_initializer(key);
        if (!initializer.active) {
          // Keep the observed key open while marking it deleted. Windows does
          // not remove/recreate that name until the last handle closes, which
          // prevents deleting a replacement lock through an ABA race.
          if (initializer.stale_key) {
            RegDeleteKeyW(key, L"Initialization");
            RegCloseKey(initializer.stale_key);
          }
          continue;
        }
        if (initializer.stale_key) {
          RegCloseKey(initializer.stale_key);
        }
        break;
      } else {
        break;
      }
    }
    RegCloseKey(key);
    return cached;
  }

  inline std::optional<std::filesystem::path> restore_state_file_from_command_line() {
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return std::nullopt;
    std::optional<std::filesystem::path> path;
    constexpr std::wstring_view prefix = L"--restore-state-file=";
    for (int i = 1; i < argc; ++i) {
      const std::wstring_view argument(argv[i]);
      if (argument.starts_with(prefix) && argument.size() > prefix.size()) {
        path = std::filesystem::path(argument.substr(prefix.size()));
      }
    }
    LocalFree(argv);
    return path;
  }

  inline std::wstring restore_task_arguments() {
    std::wstring arguments = L"--restore";
    if (const auto state_file = restore_state_file_from_command_line()) {
      arguments += L" " + display_helper::restore_state_file_argument(*state_file);
    }
    return arguments;
  }

  inline HANDLE make_named_mutex(const wchar_t *name) {
    SECURITY_ATTRIBUTES sa {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    return CreateMutexW(&sa, FALSE, name);
  }

  inline bool ensure_single_instance(HANDLE &out_handle) {
    out_handle = make_named_mutex(L"Global\\SunshineDisplayHelper");
    if (!out_handle && GetLastError() == ERROR_ACCESS_DENIED) {
      out_handle = make_named_mutex(L"Local\\SunshineDisplayHelper");
    }
    if (!out_handle) {
      return true;  // continue; best-effort singleton failed
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      return false;  // another instance running
    }
    return true;
  }

  inline void hide_console_window() {
    if (HWND console = GetConsoleWindow()) {
      ShowWindow(console, SW_HIDE);
    }
  }

  inline std::filesystem::path compute_log_dir() {
    // Try roaming AppData first
    std::wstring appdataW;
    appdataW.resize(MAX_PATH);
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appdataW.data()))) {
      appdataW.resize(wcslen(appdataW.c_str()));
      auto path = std::filesystem::path(appdataW) / L"Sunshine";
      std::error_code ec;
      std::filesystem::create_directories(path, ec);
      return path;
    }

    // Next, %APPDATA%
    std::wstring envAppData;
    DWORD needed = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
    if (needed > 0) {
      envAppData.resize(needed);
      DWORD written = GetEnvironmentVariableW(L"APPDATA", envAppData.data(), needed);
      if (written > 0) {
        envAppData.resize(written);
        auto path = std::filesystem::path(envAppData) / L"Sunshine";
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        return path;
      }
    }

    // Fallback: temp directory or current dir
    std::wstring tempW;
    tempW.resize(MAX_PATH);
    DWORD tlen = GetTempPathW(MAX_PATH, tempW.data());
    if (tlen > 0 && tlen < MAX_PATH) {
      tempW.resize(tlen);
      auto path = std::filesystem::path(tempW) / L"Sunshine";
      std::error_code ec;
      std::filesystem::create_directories(path, ec);
      return path;
    }
    auto path = std::filesystem::path(L".") / L"Sunshine";
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return path;
  }

  inline std::filesystem::path compute_snapshot_dir() {
    // When running as SYSTEM, prefer a shared ProgramData location for snapshots.
    if (platf::dxgi::is_running_as_system()) {
      if (const auto path = private_system_snapshot_dir()) {
        return *path;
      }
      return {};
    }

    // Default to per-user roaming AppData (or fallback locations inside compute_log_dir).
    return compute_log_dir();
  }

  struct SnapshotPaths {
    std::filesystem::path golden;
    std::filesystem::path golden_status;
    std::filesystem::path session_current;
    std::filesystem::path session_previous;
    std::filesystem::path vibeshine_state;
  };

  inline SnapshotPaths make_snapshot_paths(const std::filesystem::path &root) {
    return SnapshotPaths {
      .golden = root / L"display_golden_restore.json",
      .golden_status = root / L"display_golden_restore_status.json",
      .session_current = root / L"display_session_current.json",
      .session_previous = root / L"display_session_previous.json",
      .vibeshine_state = root / L"vibeshine_state.json",
    };
  }

  inline std::vector<std::filesystem::path> executable_config_search_roots() {
    std::vector<std::filesystem::path> roots;
    wchar_t exe_path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exe_path, MAX_PATH)) {
      return roots;
    }

    const auto module_path = std::filesystem::path(exe_path);
    const auto module_dir = module_path.parent_path();
    if (module_dir.empty()) {
      return roots;
    }

    roots.push_back(module_dir / L"config");
    roots.push_back(module_dir.parent_path() / L"config");
    roots.push_back(module_dir.parent_path());
    return roots;
  }

  inline std::vector<std::filesystem::path> snapshot_search_roots() {
    std::vector<std::filesystem::path> roots;
    const auto user_root = compute_log_dir();
    if (!user_root.empty()) {
      roots.push_back(user_root);
    }
    if (platf::dxgi::is_running_as_system()) {
      if (const auto path = private_system_snapshot_dir()) {
        roots.push_back(*path);
      }
    }
    for (const auto &root : executable_config_search_roots()) {
      if (!root.empty()) {
        roots.push_back(root);
      }
    }
    // De-duplicate while preserving order.
    std::vector<std::filesystem::path> uniq;
    for (const auto &root : roots) {
      if (root.empty()) {
        continue;
      }
      if (std::find(uniq.begin(), uniq.end(), root) == uniq.end()) {
        uniq.push_back(root);
      }
    }
    return uniq;
  }
}  // namespace display_helper_paths

#endif  // _WIN32
