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
  #include <string>
  #include <vector>

  #include <shlobj.h>
  #include <windows.h>

  #include "src/platform/windows/ipc/misc_utils.h"

namespace display_helper_paths {
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
      std::wstring programDataW;
      programDataW.resize(MAX_PATH);
      if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, SHGFP_TYPE_CURRENT, programDataW.data()))) {
        programDataW.resize(wcslen(programDataW.c_str()));
        auto path = std::filesystem::path(programDataW) / L"Sunshine";
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        return path;
      }
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
    {
      std::wstring programDataW;
      programDataW.resize(MAX_PATH);
      if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, SHGFP_TYPE_CURRENT, programDataW.data()))) {
        programDataW.resize(wcslen(programDataW.c_str()));
        roots.push_back(std::filesystem::path(programDataW) / L"Sunshine");
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
