/**
 * @file src/platform/windows/rtss_integration.cpp
 * @brief Apply/restore RTSS frame limit and related properties on stream start/stop.
 */

#ifdef _WIN32

  // standard includes
  #include <array>
  #include <atomic>
  #include <chrono>
  #include <cstdio>
  #include <cwchar>
  #include <filesystem>
  #include <fstream>
  #include <future>
  #include <nlohmann/json.hpp>
  #include <optional>
  #include <string>
  #include <system_error>
  #include <thread>
  #include <type_traits>
  #include <utility>
  #include <vector>

// clang-format off
  #include <winsock2.h>
  #include <Windows.h>
  #include <tlhelp32.h>
// clang-format on

  // local includes
  #include "src/config.h"
  #include "src/logging.h"
  #include "src/platform/windows/misc.h"
  #include "src/platform/windows/rtss_integration.h"

using namespace std::literals;
namespace fs = std::filesystem;

namespace platf {

  namespace {
    // RTSSHooks function pointer types
    using fn_LoadProfile = BOOL(__cdecl *)(LPCSTR profileName);
    using fn_SaveProfile = BOOL(__cdecl *)(LPCSTR profileName);
    using fn_GetProfileProperty = BOOL(__cdecl *)(LPCSTR name, LPVOID pBuf, DWORD size);
    using fn_SetProfileProperty = BOOL(__cdecl *)(LPCSTR name, LPVOID pBuf, DWORD size);
    using fn_UpdateProfiles = VOID(__cdecl *)();
    using fn_GetFlags = DWORD(__cdecl *)();
    using fn_SetFlags = DWORD(__cdecl *)(DWORD, DWORD);

    struct hooks_t {
      HMODULE module = nullptr;
      fn_LoadProfile LoadProfile = nullptr;
      fn_SaveProfile SaveProfile = nullptr;
      fn_GetProfileProperty GetProfileProperty = nullptr;
      fn_SetProfileProperty SetProfileProperty = nullptr;
      fn_UpdateProfiles UpdateProfiles = nullptr;
      fn_GetFlags GetFlags = nullptr;
      fn_SetFlags SetFlags = nullptr;

      explicit operator bool() const {
        return module && LoadProfile && SaveProfile && GetProfileProperty && SetProfileProperty && UpdateProfiles && GetFlags && SetFlags;
      }
    };

    hooks_t g_hooks;
    bool g_limit_active = false;
    bool g_recovery_file_owned = false;
    bool g_settings_dirty = false;
    bool g_flags_modified = false;
    bool g_denominator_modified = false;
    bool g_limit_modified = false;
    bool g_sync_limiter_modified = false;

    // Remember original values so we can restore on stream end
    std::optional<int> g_original_limit;
    std::optional<std::string> g_sync_limiter_override;
    std::optional<int> g_original_sync_limiter;
    std::optional<int> g_original_denominator;
    std::optional<DWORD> g_original_flags;

    // Install path resolved from config (root RTSS folder)
    fs::path g_rtss_root;

    PROCESS_INFORMATION g_rtss_process_info {};
    bool g_rtss_started_by_sunshine = false;
    std::atomic<unsigned int> g_active_hook_calls {0};
    bool g_hooks_failed = false;

    constexpr DWORD k_rtss_shutdown_timeout_ms = 5000;
    constexpr auto k_rtss_response_timeout = std::chrono::seconds(1);
    constexpr DWORD k_rtss_flag_limiter_disabled = 4;

    const std::array<const wchar_t *, 2> k_rtss_process_names = {L"RTSS.exe", L"RTSS64.exe"};
    const std::array<const wchar_t *, 2> k_rtss_executable_names = {L"RTSS.exe", L"RTSS64.exe"};

    const fs::path profile_path(const fs::path &root) {
      return root / "Profiles" / "Global";
    }

    bool load_hooks(const fs::path &root);
    bool hooks_available();
    std::optional<DWORD> get_hook_flags();
    std::optional<DWORD> set_hook_flags(DWORD and_mask, DWORD xor_mask);
    bool update_profiles();
    std::optional<int> set_limit_denominator(const fs::path &root, int new_denominator);
    std::optional<int> set_profile_property_int(const char *name, int new_value);
    bool write_profile_value_int(const fs::path &root, const char *key, int new_value);
    fs::path resolve_rtss_root();

    template<typename Result, typename Callable>
    std::optional<Result> call_rtss_hooks(const char *operation, Callable &&callable) {
      std::promise<Result> promise;
      auto result = promise.get_future();
      g_active_hook_calls.fetch_add(1, std::memory_order_acq_rel);

      std::thread worker;
      try {
        worker = std::thread([
                               promise = std::move(promise),
                               callable = std::forward<Callable>(callable)
                             ]() mutable {
          try {
            promise.set_value(callable());
          } catch (...) {
            promise.set_exception(std::current_exception());
          }
          g_active_hook_calls.fetch_sub(1, std::memory_order_acq_rel);
        });
      } catch (const std::exception &ex) {
        g_active_hook_calls.fetch_sub(1, std::memory_order_acq_rel);
        BOOST_LOG(error) << "Unable to start RTSS hooks operation '" << operation << "': " << ex.what();
        g_hooks_failed = true;
        return std::nullopt;
      }

      if (result.wait_for(k_rtss_response_timeout) != std::future_status::ready) {
        g_hooks_failed = true;
        worker.detach();
        BOOST_LOG(error) << "RTSS did not respond to '" << operation
                         << "' within 1 second and appears to be stalled. "
                            "Try restarting RTSS to resolve the issue; continuing without RTSS hooks.";
        return std::nullopt;
      }

      worker.join();
      try {
        return result.get();
      } catch (const std::exception &ex) {
        BOOST_LOG(error) << "RTSS hooks operation '" << operation << "' failed: " << ex.what();
      } catch (...) {
        BOOST_LOG(error) << "RTSS hooks operation '" << operation << "' failed with an unknown exception";
      }
      g_hooks_failed = true;
      return std::nullopt;
    }

    bool hooks_available() {
      return static_cast<bool>(g_hooks) && !g_hooks_failed && g_active_hook_calls.load(std::memory_order_acquire) == 0;
    }

    std::optional<DWORD> get_hook_flags() {
      if (!hooks_available()) {
        return std::nullopt;
      }
      auto get_flags = g_hooks.GetFlags;
      return call_rtss_hooks<DWORD>("GetFlags", [get_flags]() {
        return get_flags();
      });
    }

    std::optional<DWORD> set_hook_flags(DWORD and_mask, DWORD xor_mask) {
      if (!hooks_available()) {
        return std::nullopt;
      }
      auto set_flags = g_hooks.SetFlags;
      return call_rtss_hooks<DWORD>("SetFlags", [set_flags, and_mask, xor_mask]() {
        return set_flags(and_mask, xor_mask);
      });
    }

    bool update_profiles() {
      if (!hooks_available()) {
        return false;
      }
      auto update = g_hooks.UpdateProfiles;
      return call_rtss_hooks<bool>("UpdateProfiles", [update]() {
               update();
               return true;
             }).value_or(false);
    }

    struct recovery_snapshot_t {
      bool flags_modified = false;
      std::optional<DWORD> original_flags;
      bool denominator_modified = false;
      std::optional<int> original_denominator;
      bool limit_modified = false;
      std::optional<int> original_limit;
      bool sync_limiter_modified = false;
      std::optional<int> original_sync_limiter;
    };

    bool snapshot_has_changes(const recovery_snapshot_t &snapshot) {
      return snapshot.flags_modified || snapshot.denominator_modified || snapshot.limit_modified || snapshot.sync_limiter_modified;
    }

    std::optional<fs::path> rtss_overrides_dir_path() {
      static std::optional<fs::path> cached;
      if (cached.has_value()) {
        return cached;
      }

      wchar_t program_data_env[MAX_PATH] = {};
      DWORD len = GetEnvironmentVariableW(L"ProgramData", program_data_env, _countof(program_data_env));
      if (len == 0 || len >= _countof(program_data_env)) {
        return std::nullopt;
      }

      fs::path base(program_data_env);
      std::error_code ec;
      if (!fs::exists(base, ec)) {
        return std::nullopt;
      }

      cached = base / L"Sunshine";
      return cached;
    }

    std::optional<fs::path> rtss_overrides_file_path() {
      auto dir = rtss_overrides_dir_path();
      if (!dir) {
        return std::nullopt;
      }
      return *dir / L"rtss_overrides.json";
    }

    bool write_overrides_file(const recovery_snapshot_t &snapshot) {
      if (!snapshot_has_changes(snapshot)) {
        return true;
      }

      auto file_path_opt = rtss_overrides_file_path();
      if (!file_path_opt) {
        BOOST_LOG(warning) << "RTSS overrides: unable to resolve ProgramData path for crash recovery";
        return false;
      }

      const auto &file_path = *file_path_opt;
      std::error_code ec;
      if (auto dir = file_path.parent_path(); !dir.empty()) {
        if (!fs::exists(dir, ec)) {
          if (!fs::create_directories(dir, ec) && ec) {
            BOOST_LOG(warning) << "RTSS overrides: failed to create recovery directory: " << ec.message();
            return false;
          }
        }
      }

      nlohmann::json j;
      auto encode = [&](const char *key, bool modified, const auto &value_opt) {
        nlohmann::json node;
        node["modified"] = modified;
        if (modified) {
          if (value_opt.has_value()) {
            node["value"] = *value_opt;
          } else {
            node["value"] = nullptr;
          }
        }
        j[key] = node;
      };

      encode("flags", snapshot.flags_modified, snapshot.original_flags);
      encode("denominator", snapshot.denominator_modified, snapshot.original_denominator);
      encode("limit", snapshot.limit_modified, snapshot.original_limit);
      encode("sync_limiter", snapshot.sync_limiter_modified, snapshot.original_sync_limiter);

      std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
      if (!out.is_open()) {
        BOOST_LOG(warning) << "RTSS overrides: failed to open recovery file for write";
        return false;
      }

      try {
        out << j.dump();
        if (!out.good()) {
          BOOST_LOG(warning) << "RTSS overrides: failed to write recovery file";
          return false;
        }
      } catch (const std::exception &ex) {
        BOOST_LOG(warning) << "RTSS overrides: exception while writing recovery file: " << ex.what();
        return false;
      }

      return true;
    }

    std::optional<recovery_snapshot_t> read_overrides_file() {
      auto file_path_opt = rtss_overrides_file_path();
      if (!file_path_opt) {
        return std::nullopt;
      }

      std::error_code ec;
      if (!fs::exists(*file_path_opt, ec) || ec) {
        return std::nullopt;
      }

      std::ifstream in(*file_path_opt, std::ios::binary);
      if (!in.is_open()) {
        BOOST_LOG(warning) << "RTSS overrides: unable to open recovery file for read";
        return std::nullopt;
      }

      nlohmann::json j;
      try {
        in >> j;
      } catch (const std::exception &ex) {
        BOOST_LOG(warning) << "RTSS overrides: failed to parse recovery file: " << ex.what();
        return std::nullopt;
      }

      recovery_snapshot_t snapshot;
      auto decode = [&](const char *key, bool &modified, auto &value_opt) {
        modified = false;
        value_opt.reset();
        if (!j.contains(key)) {
          return;
        }
        const auto &node = j[key];
        if (!node.is_object()) {
          return;
        }
        modified = node.value("modified", false);
        if (node.contains("value") && !node["value"].is_null()) {
          try {
            using value_type = typename std::decay_t<decltype(value_opt)>::value_type;
            auto raw = node["value"].get<long long>();
            value_opt = static_cast<value_type>(raw);
          } catch (...) {
            value_opt.reset();
          }
        }
      };

      decode("flags", snapshot.flags_modified, snapshot.original_flags);
      decode("denominator", snapshot.denominator_modified, snapshot.original_denominator);
      decode("limit", snapshot.limit_modified, snapshot.original_limit);
      decode("sync_limiter", snapshot.sync_limiter_modified, snapshot.original_sync_limiter);

      if (!snapshot_has_changes(snapshot)) {
        return std::nullopt;
      }

      return snapshot;
    }

    void delete_overrides_file() {
      auto file_path_opt = rtss_overrides_file_path();
      if (!file_path_opt) {
        return;
      }
      std::error_code ec;
      fs::remove(*file_path_opt, ec);
      if (ec) {
        BOOST_LOG(warning) << "RTSS overrides: failed to delete recovery file: " << ec.message();
      }
    }

    bool restore_from_snapshot(const recovery_snapshot_t &snapshot) {
      fs::path root = resolve_rtss_root();
      if (!fs::exists(root)) {
        BOOST_LOG(warning) << "RTSS overrides: install path not found for recovery: "sv << root.string();
        return false;
      }

      bool hooks_loaded = false;
      auto unload_hooks = [&]() {
        if (hooks_loaded && g_hooks.module && g_active_hook_calls.load(std::memory_order_acquire) == 0) {
          FreeLibrary(g_hooks.module);
          g_hooks = {};
        }
      };

      auto ensure_hooks_loaded = [&]() -> bool {
        if (hooks_loaded) {
          return true;
        }
        if (!load_hooks(root)) {
          return false;
        }
        hooks_loaded = true;
        return true;
      };

      bool success = true;

      if (snapshot.denominator_modified && snapshot.original_denominator.has_value()) {
        if (!set_limit_denominator(root, *snapshot.original_denominator).has_value()) {
          success = false;
        }
      }

      if (snapshot.limit_modified) {
        int value = snapshot.original_limit.value_or(0);
        bool applied = false;
        if (ensure_hooks_loaded()) {
          applied = set_profile_property_int("FramerateLimit", value).has_value();
        }
        if (!applied && write_profile_value_int(root, "FramerateLimit", value)) {
          applied = true;
        }
        if (!applied) {
          success = false;
        }
      }

      if (snapshot.sync_limiter_modified && snapshot.original_sync_limiter.has_value()) {
        bool applied = false;
        if (ensure_hooks_loaded()) {
          applied = set_profile_property_int("SyncLimiter", *snapshot.original_sync_limiter).has_value();
        }
        if (!applied && write_profile_value_int(root, "SyncLimiter", *snapshot.original_sync_limiter)) {
          applied = true;
        }
        if (!applied) {
          success = false;
        }
      }

      if (snapshot.flags_modified && snapshot.original_flags.has_value()) {
        if (ensure_hooks_loaded()) {
          constexpr DWORD limiter_mask = k_rtss_flag_limiter_disabled;
          DWORD xor_mask = (*snapshot.original_flags & limiter_mask) ? limiter_mask : 0;
          auto updated_flags = set_hook_flags(~limiter_mask, xor_mask);
          if (!updated_flags || (*updated_flags & limiter_mask) != xor_mask) {
            BOOST_LOG(warning) << "RTSS overrides: limiter flags restore mismatch";
            success = false;
          }
        } else {
          BOOST_LOG(warning) << "RTSS overrides: unable to load hooks to restore limiter flags";
          success = false;
        }
      }

      unload_hooks();
      return success;
    }

    void maybe_restore_from_overrides_file() {
      if (g_recovery_file_owned) {
        return;
      }
      auto snapshot = read_overrides_file();
      if (!snapshot) {
        return;
      }

      BOOST_LOG(info) << "RTSS overrides: pending recovery file detected; attempting restore";
      if (restore_from_snapshot(*snapshot)) {
        delete_overrides_file();
      }
    }

    bool ensure_profile_exists(const fs::path &root) {
      auto path = profile_path(root);
      if (fs::exists(path)) {
        return true;
      }
      try {
        fs::create_directories(path.parent_path());
        static constexpr char k_default_profile[] = "[Framerate]\nLimit=0\nLimitDenominator=1\nSyncLimiter=0\n";
        std::ofstream init_out(path, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!init_out) {
          BOOST_LOG(warning) << "Unable to create RTSS Global profile at: "sv << path.string();
          return false;
        }
        init_out.write(k_default_profile, sizeof(k_default_profile) - 1);
        init_out.flush();
        BOOST_LOG(info) << "Created default RTSS Global profile"sv;
        return true;
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Failed to ensure RTSS Global profile exists: "sv << e.what();
        return false;
      }
    }

    std::optional<int> read_profile_value_int(const fs::path &root, const char *key) {
      auto path = profile_path(root);
      if (!fs::exists(path)) {
        return std::nullopt;
      }
      try {
        std::string content;
        {
          std::ifstream in(path, std::ios::in | std::ios::binary);
          content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        std::string needle = std::string(key) + '=';
        auto pos = content.find(needle);
        if (pos == std::string::npos) {
          return std::nullopt;
        }
        auto end = content.find_first_of("\r\n", pos);
        auto line = content.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        auto eq = line.find('=');
        if (eq == std::string::npos) {
          return std::nullopt;
        }
        try {
          return std::stoi(line.substr(eq + 1));
        } catch (...) {
          return std::nullopt;
        }
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Failed reading RTSS profile value '"sv << key << "': "sv << e.what();
        return std::nullopt;
      }
    }

    bool write_profile_value_int(const fs::path &root, const char *key, int new_value) {
      try {
        if (!ensure_profile_exists(root)) {
          return false;
        }
        auto path = profile_path(root);
        std::string content;
        {
          std::ifstream in(path, std::ios::in | std::ios::binary);
          content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        std::string needle = std::string(key) + '=';
        auto pos = content.find(needle);
        char buf[64];
        snprintf(buf, sizeof(buf), "%s=%d", key, new_value);
        if (pos != std::string::npos) {
          auto end = content.find_first_of("\r\n", pos);
          auto line = content.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
          content.replace(pos, line.size(), buf);
        } else {
          content.append("\n");
          content.append(buf);
          content.append("\n");
        }
        std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
        out.write(content.data(), (std::streamsize) content.size());
        return true;
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Failed writing RTSS profile value '"sv << key << "': "sv << e.what();
        return false;
      }
    }

    bool is_rtss_process_running() {
      HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
      if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
      }

      PROCESSENTRY32W entry {};
      entry.dwSize = sizeof(entry);
      bool running = false;
      if (Process32FirstW(snapshot, &entry)) {
        do {
          for (auto name : k_rtss_process_names) {
            if (_wcsicmp(entry.szExeFile, name) == 0) {
              running = true;
              break;
            }
          }
        } while (!running && Process32NextW(snapshot, &entry));
      }

      CloseHandle(snapshot);
      return running;
    }

    std::optional<fs::path> find_rtss_executable(const fs::path &root) {
      for (auto name : k_rtss_executable_names) {
        fs::path candidate = root / name;
        if (fs::exists(candidate)) {
          return candidate;
        }
      }
      return std::nullopt;
    }

    void reset_rtss_process_state() {
      if (g_rtss_process_info.hProcess) {
        CloseHandle(g_rtss_process_info.hProcess);
      }
      if (g_rtss_process_info.hThread) {
        CloseHandle(g_rtss_process_info.hThread);
      }
      g_rtss_process_info = {};
      g_rtss_started_by_sunshine = false;
    }

    bool ensure_rtss_running(const fs::path &root) {
      // If we previously launched RTSS, check if the process is still alive.
      if (g_rtss_process_info.hProcess) {
        DWORD exit_code = 0;
        if (GetExitCodeProcess(g_rtss_process_info.hProcess, &exit_code) && exit_code == STILL_ACTIVE) {
          return true;
        }
        reset_rtss_process_state();
      }

      if (is_rtss_process_running()) {
        return true;
      }

      auto exe = find_rtss_executable(root);
      if (!exe) {
        BOOST_LOG(warning) << "RTSS executable not found in: "sv << root.string();
        return false;
      }

      std::wstring exe_path = exe->wstring();
      std::wstring working_dir = root.wstring();
      std::string cmd_utf8 = "\"" + to_utf8(exe_path) + "\"";

      std::error_code startup_ec;
      STARTUPINFOEXW startup_info = create_startup_info(nullptr, nullptr, startup_ec);
      if (startup_ec) {
        BOOST_LOG(warning) << "Failed to allocate startup info for RTSS launch"sv;
        return false;
      }
      startup_info.StartupInfo.dwFlags |= STARTF_USESHOWWINDOW;
      startup_info.StartupInfo.wShowWindow = SW_HIDE;

      DWORD creation_flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_BREAKAWAY_FROM_JOB | CREATE_NO_WINDOW;

      PROCESS_INFORMATION process_info {};
      std::error_code launch_ec;
      bool launched = launch_process_with_impersonation(
        true,
        cmd_utf8,
        working_dir,
        creation_flags,
        startup_info,
        process_info,
        launch_ec
      );

      if (startup_info.lpAttributeList) {
        free_proc_thread_attr_list(startup_info.lpAttributeList);
      }

      if (!launched) {
        if (launch_ec) {
          BOOST_LOG(warning) << "Failed to launch RTSS via impersonation: "sv << launch_ec.message();
        } else {
          BOOST_LOG(warning) << "Failed to launch RTSS via impersonation"sv;
        }
        reset_rtss_process_state();
        return false;
      }

      CloseHandle(process_info.hThread);

      g_rtss_process_info = process_info;
      g_rtss_started_by_sunshine = true;
      BOOST_LOG(info) << "Launched RTSS for frame limiter support"sv;
      return true;
    }

    struct close_ctx_t {
      DWORD pid;
      bool signaled;
    };

    BOOL CALLBACK enum_close_windows(HWND hwnd, LPARAM lparam) {
      auto ctx = reinterpret_cast<close_ctx_t *>(lparam);
      if (!ctx) {
        return TRUE;
      }

      DWORD wnd_pid = 0;
      if (!GetWindowThreadProcessId(hwnd, &wnd_pid)) {
        return TRUE;
      }

      if (wnd_pid == ctx->pid) {
        if (SendNotifyMessageW(hwnd, WM_CLOSE, 0, 0)) {
          ctx->signaled = true;
        }
      }
      return TRUE;
    }

    bool request_process_close(DWORD pid) {
      close_ctx_t ctx {pid, false};
      EnumWindows(enum_close_windows, reinterpret_cast<LPARAM>(&ctx));
      return ctx.signaled;
    }

    void stop_rtss_process() {
      if (!g_rtss_started_by_sunshine || !g_rtss_process_info.hProcess) {
        reset_rtss_process_state();
        return;
      }

      DWORD exit_code = 0;
      if (GetExitCodeProcess(g_rtss_process_info.hProcess, &exit_code) && exit_code == STILL_ACTIVE) {
        bool requested = request_process_close(g_rtss_process_info.dwProcessId);
        if (requested) {
          WaitForSingleObject(g_rtss_process_info.hProcess, k_rtss_shutdown_timeout_ms);
        }

        if (GetExitCodeProcess(g_rtss_process_info.hProcess, &exit_code) && exit_code == STILL_ACTIVE) {
          TerminateProcess(g_rtss_process_info.hProcess, 0);
        }
      }

      reset_rtss_process_state();
    }

    // Map config string to SyncLimiter integer
    std::optional<int> map_sync_limiter(const std::string &type) {
      std::string t = type;
      for (auto &c : t) {
        c = (char) ::tolower(c);
      }

      if (t == "async") {
        return 0;
      }
      if (t == "front edge sync" || t == "front_edge_sync") {
        return 1;
      }
      if (t == "back edge sync" || t == "back_edge_sync") {
        return 2;
      }
      if (t == "nvidia reflex" || t == "nvidia_reflex" || t == "reflex") {
        return 3;
      }
      return std::nullopt;
    }

    // Load RTSSHooks DLL from the RTSS root
    bool load_hooks(const fs::path &root) {
      if (hooks_available()) {
        return true;
      }
      if (g_hooks.module || g_active_hook_calls.load(std::memory_order_acquire) != 0 || g_hooks_failed) {
        return false;
      }

      auto try_load = [&](const wchar_t *dll_name) -> bool {
        fs::path p = root / dll_name;
        HMODULE m = LoadLibraryW(p.c_str());
        if (!m) {
          return false;
        }
        g_hooks.module = m;
        g_hooks.LoadProfile = (fn_LoadProfile) GetProcAddress(m, "LoadProfile");
        g_hooks.SaveProfile = (fn_SaveProfile) GetProcAddress(m, "SaveProfile");
        g_hooks.GetProfileProperty = (fn_GetProfileProperty) GetProcAddress(m, "GetProfileProperty");
        g_hooks.SetProfileProperty = (fn_SetProfileProperty) GetProcAddress(m, "SetProfileProperty");
        g_hooks.UpdateProfiles = (fn_UpdateProfiles) GetProcAddress(m, "UpdateProfiles");
        g_hooks.GetFlags = (fn_GetFlags) GetProcAddress(m, "GetFlags");
        g_hooks.SetFlags = (fn_SetFlags) GetProcAddress(m, "SetFlags");
        if (!g_hooks) {
          BOOST_LOG(warning) << "RTSSHooks DLL missing required exports"sv;
          FreeLibrary(m);
          g_hooks = {};
          return false;
        }
        return true;
      };

      // Prefer 64-bit hooks DLL name; fall back to generic
      if (!try_load(L"RTSSHooks64.dll")) {
        if (!try_load(L"RTSSHooks.dll")) {
          BOOST_LOG(warning) << "Failed to load RTSSHooks DLL from: "sv << root.string();
          return false;
        }
      }
      return true;
    }

    // Read and replace LimitDenominator in the RTSS Global profile. Returns previous value (or 1 if missing).
    std::optional<int> set_limit_denominator(const fs::path &root, int new_denominator) {
      try {
        if (!ensure_profile_exists(root)) {
          return std::nullopt;
        }
        auto global_path = profile_path(root);
        std::string content;
        {
          std::ifstream in(global_path, std::ios::in | std::ios::binary);
          content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        // Find current denominator
        int old_den = 1;
        {
          auto pos = content.find("LimitDenominator=");
          if (pos != std::string::npos) {
            auto end = content.find_first_of("\r\n", pos);
            auto line = content.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            auto eq = line.find('=');
            if (eq != std::string::npos) {
              try {
                old_den = std::stoi(line.substr(eq + 1));
              } catch (...) {
                old_den = 1;
              }
            }
            // Replace existing value
            char buf[64];
            snprintf(buf, sizeof(buf), "LimitDenominator=%d", new_denominator);
            content.replace(pos, line.size(), buf);
          } else {
            // Append setting if not present
            char buf[64];
            snprintf(buf, sizeof(buf), "\nLimitDenominator=%d\n", new_denominator);
            content.append(buf);
          }
        }

        {
          std::ofstream out(global_path, std::ios::out | std::ios::binary | std::ios::trunc);
          out.write(content.data(), (std::streamsize) content.size());
        }

        BOOST_LOG(info) << "RTSS LimitDenominator set to "sv << new_denominator << ", original "sv << old_den;
        return old_den;
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Failed updating RTSS Global profile: "sv << e.what();
        return std::nullopt;
      }
    }

    // Helper: read integer profile property, returns value if present
    std::optional<int> get_profile_property_int(const char *name) {
      if (!hooks_available()) {
        return std::nullopt;
      }

      auto load_profile = g_hooks.LoadProfile;
      auto get_property = g_hooks.GetProfileProperty;
      std::string property_name(name);
      auto result = call_rtss_hooks<std::optional<int>>(name, [load_profile, get_property, property_name = std::move(property_name)]() {
        int value = 0;
        if (!load_profile("")) {
          return std::optional<int> {};
        }
        if (get_property(property_name.c_str(), &value, sizeof(value))) {
          return std::optional<int> {value};
        }
        return std::optional<int> {};
      });
      return result.value_or(std::nullopt);
    }

    // Helper: set integer profile property and return previous value if present
    std::optional<int> set_profile_property_int(const char *name, int new_value) {
      if (!hooks_available()) {
        return std::nullopt;
      }

      struct set_result_t {
        int old_value;
        bool had_old;
        bool applied;
      };

      auto load_profile = g_hooks.LoadProfile;
      auto get_property = g_hooks.GetProfileProperty;
      auto set_property = g_hooks.SetProfileProperty;
      auto save_profile = g_hooks.SaveProfile;
      auto update = g_hooks.UpdateProfiles;
      std::string property_name(name);
      auto result = call_rtss_hooks<set_result_t>(name, [
                                                         load_profile,
                                                         get_property,
                                                         set_property,
                                                         save_profile,
                                                         update,
                                                         property_name = std::move(property_name),
                                                         new_value
                                                       ]() mutable {
        set_result_t state {0, false, false};
        // Empty string selects the global profile as in the RTSS UI.
        if (!load_profile("")) {
          return state;
        }
        state.had_old = get_property(property_name.c_str(), &state.old_value, sizeof(state.old_value)) != FALSE;
        if (!set_property(property_name.c_str(), &new_value, sizeof(new_value)) || !save_profile("")) {
          return state;
        }
        update();
        state.applied = true;
        return state;
      });
      if (!result || !result->applied) {
        return std::nullopt;
      }

      if (result->had_old) {
        BOOST_LOG(info) << "RTSS property "sv << name << " set to "sv << new_value << ", original "sv << result->old_value;
      } else {
        BOOST_LOG(info) << "RTSS property "sv << name << " set to "sv << new_value << ", original (implicit) 0"sv;
      }
      // Always return the previous value (0 if not present) so callers can restore it
      return result->old_value;
    }

    // Resolve RTSS root path from config (absolute path or relative to Program Files)
    fs::path resolve_rtss_root() {
      // Default subfolder if not configured
      std::string sub = config::rtss.install_path;
      if (sub.empty()) {
        sub = "RivaTuner Statistics Server";
      }

      auto is_abs = sub.size() > 1 && (sub[1] == ':' || (sub[0] == '\\' && sub[1] == '\\'));
      if (is_abs) {
        return fs::path(sub);
      }

      // Prefer Program Files (x86) on 64-bit Windows if present
      {
        wchar_t buf[MAX_PATH] = {};
        DWORD len = GetEnvironmentVariableW(L"PROGRAMFILES(X86)", buf, ARRAYSIZE(buf));
        if (len > 0 && len < ARRAYSIZE(buf)) {
          fs::path base = buf;
          fs::path candidate = base / fs::path(std::wstring(sub.begin(), sub.end()));
          if (fs::exists(candidate)) {
            return candidate;
          }
        }
      }

      // Resolve %PROGRAMFILES%\<sub>
      wchar_t buf[MAX_PATH] = {};
      DWORD len = GetEnvironmentVariableW(L"PROGRAMFILES", buf, ARRAYSIZE(buf));
      fs::path base;
      if (len == 0 || len >= ARRAYSIZE(buf)) {
        base = L"C:\\Program Files";
      } else {
        base = buf;
      }
      return base / fs::path(std::wstring(sub.begin(), sub.end()));
    }
  }  // namespace

  void rtss_restore_pending_overrides() {
    maybe_restore_from_overrides_file();
  }

  void rtss_set_sync_limiter_override(std::optional<std::string> value) {
    if (value && value->empty()) {
      g_sync_limiter_override.reset();
    } else {
      g_sync_limiter_override = std::move(value);
    }
  }

  std::optional<std::string> rtss_get_sync_limiter_override() {
    return g_sync_limiter_override;
  }

  bool rtss_warmup_process() {
    g_rtss_root = resolve_rtss_root();
    if (!fs::exists(g_rtss_root)) {
      BOOST_LOG(warning) << "RTSS install path not found: "sv << g_rtss_root.string();
      return false;
    }
    return ensure_rtss_running(g_rtss_root);
  }

  bool rtss_streaming_start(int scaled_limit, int denominator) {
    if (g_active_hook_calls.load(std::memory_order_acquire) == 0) {
      g_hooks_failed = false;
    }
    g_limit_active = false;
    g_settings_dirty = false;
    g_flags_modified = false;
    g_denominator_modified = false;
    g_limit_modified = false;
    g_sync_limiter_modified = false;
    maybe_restore_from_overrides_file();

    if (!config::frame_limiter.enable) {
      return false;
    }

    g_rtss_root = resolve_rtss_root();
    if (!fs::exists(g_rtss_root)) {
      BOOST_LOG(warning) << "RTSS install path not found: "sv << g_rtss_root.string();
      return false;
    }

    ensure_rtss_running(g_rtss_root);

    if (!load_hooks(g_rtss_root)) {
      // We can still change Global profile denominator even if hooks are missing
      BOOST_LOG(warning) << "RTSSHooks not loaded; will only update Global profile denominator"sv;
    }

    if (hooks_available()) {
      auto current_flags = get_hook_flags();
      g_original_flags = current_flags;
      if (current_flags && (*current_flags & k_rtss_flag_limiter_disabled)) {
        constexpr DWORD limiter_mask = k_rtss_flag_limiter_disabled;
        auto updated_flags = set_hook_flags(~limiter_mask, 0);
        if (!updated_flags || (*updated_flags & limiter_mask)) {
          BOOST_LOG(warning) << "Failed to enable RTSS limiter via SetFlags"sv;
          // A timed-out call may still have reached RTSS before it stalled, so retain
          // enough state to restore the original flags later.
          g_flags_modified = true;
          g_settings_dirty = true;
        } else {
          BOOST_LOG(info) << "RTSS limiter enabled via hooks (originally disabled)"sv;
          g_flags_modified = true;
          g_settings_dirty = true;
        }
      }
    } else {
      g_original_flags.reset();
    }

    int current_denominator = denominator > 0 ? denominator : 1;
    int applied_limit = scaled_limit;
    if (applied_limit < 0) {
      applied_limit = 0;
    }

    // Update LimitDenominator in Global profile and remember previous value
    g_original_denominator = set_limit_denominator(g_rtss_root, current_denominator);
    if (g_original_denominator.has_value() && *g_original_denominator != current_denominator) {
      g_denominator_modified = true;
      g_settings_dirty = true;
    }
    if (hooks_available()) {
      // Nudge RTSS to reload profiles after file change
      (void) update_profiles();
    }

    // If hooks are available, capture original values BEFORE making further changes
    if (hooks_available()) {
      g_original_limit = get_profile_property_int("FramerateLimit");
      g_original_sync_limiter = get_profile_property_int("SyncLimiter");
    }
    if (hooks_available()) {
      BOOST_LOG(info) << "RTSS original values: limit="
                      << (g_original_limit.has_value() ? std::to_string(*g_original_limit) : std::string("<unset>"))
                      << ", syncLimiter="
                      << (g_original_sync_limiter.has_value() ? std::to_string(*g_original_sync_limiter) : std::string("<unset>"));
    } else {
      g_original_limit = read_profile_value_int(g_rtss_root, "FramerateLimit");
      g_original_sync_limiter = read_profile_value_int(g_rtss_root, "SyncLimiter");
      BOOST_LOG(info) << "RTSS profile snapshot: limit="
                      << (g_original_limit.has_value() ? std::to_string(*g_original_limit) : std::string("<unset>"))
                      << ", syncLimiter="
                      << (g_original_sync_limiter.has_value() ? std::to_string(*g_original_sync_limiter) : std::string("<unset>"));
    }

    std::optional<int> sync_limiter_value;
    std::optional<std::string> sync_limiter_label;
    if (g_sync_limiter_override && !g_sync_limiter_override->empty()) {
      if (auto mapped = map_sync_limiter(*g_sync_limiter_override)) {
        sync_limiter_value = mapped;
        sync_limiter_label = *g_sync_limiter_override;
      } else {
        BOOST_LOG(warning) << "RTSS SyncLimiter override ignored; unknown mode: "sv << *g_sync_limiter_override;
      }
    }
    if (!sync_limiter_value) {
      if (auto v = map_sync_limiter(config::rtss.frame_limit_type)) {
        sync_limiter_value = v;
        if (!config::rtss.frame_limit_type.empty()) {
          sync_limiter_label = config::rtss.frame_limit_type;
        }
      }
    }
    if (sync_limiter_value) {
      bool already_set = g_original_sync_limiter.has_value() && *g_original_sync_limiter == *sync_limiter_value;
      bool applied = false;
      if (!already_set) {
        if (hooks_available()) {
          applied = set_profile_property_int("SyncLimiter", *sync_limiter_value).has_value();
        }
        if (!applied && write_profile_value_int(g_rtss_root, "SyncLimiter", *sync_limiter_value)) {
          applied = true;
        }
        if (applied) {
          g_sync_limiter_modified = true;
          g_settings_dirty = true;
        }
      } else {
        applied = true;
      }
      if (applied) {
        if (sync_limiter_label) {
          BOOST_LOG(info) << (already_set ? "RTSS SyncLimiter already set ("sv : "RTSS SyncLimiter applied ("sv)
                          << *sync_limiter_label << ')';
        } else {
          BOOST_LOG(info) << (already_set ? "RTSS SyncLimiter already set"sv : "RTSS SyncLimiter applied"sv);
        }
      }
    }

    // Apply framerate limit
    bool limit_already_set = g_original_limit.has_value() && *g_original_limit == applied_limit;
    double limit_fps = current_denominator > 0 ? (double) applied_limit / current_denominator : 0.0;

    if (hooks_available()) {
      if (limit_already_set) {
        BOOST_LOG(info) << "RTSS framerate limit already at " << limit_fps << "Hz (raw=" << applied_limit << ", denominator=" << current_denominator << ")";
        g_limit_active = true;
      } else {
        if (set_profile_property_int("FramerateLimit", applied_limit).has_value()) {
          BOOST_LOG(info) << "RTSS applied framerate limit=" << limit_fps << "Hz (raw=" << applied_limit << ", denominator=" << current_denominator << ")";
          g_limit_active = true;
          g_limit_modified = true;
          g_settings_dirty = true;
        }
      }
    }
    if (!g_limit_active) {
      if (limit_already_set) {
        BOOST_LOG(info) << "RTSS profile framerate limit already "sv << applied_limit << " ("sv << limit_fps << "Hz)"sv;
        g_limit_active = true;
      } else if (write_profile_value_int(g_rtss_root, "FramerateLimit", applied_limit)) {
        BOOST_LOG(info) << "RTSS profile framerate limit set to "sv << applied_limit << " ("sv << limit_fps << "Hz)"sv;
        g_limit_active = true;
        g_limit_modified = true;
        g_settings_dirty = true;
      }
    }
    
    if (g_settings_dirty) {
      recovery_snapshot_t snapshot;
      snapshot.flags_modified = g_flags_modified && g_original_flags.has_value();
      snapshot.original_flags = g_original_flags;
      snapshot.denominator_modified = g_denominator_modified && g_original_denominator.has_value();
      snapshot.original_denominator = g_original_denominator;
      snapshot.limit_modified = g_limit_modified;
      snapshot.original_limit = g_original_limit;
      snapshot.sync_limiter_modified = g_sync_limiter_modified;
      snapshot.original_sync_limiter = g_original_sync_limiter;
      g_recovery_file_owned = write_overrides_file(snapshot);
    } else {
      g_recovery_file_owned = false;
    }
    // A profile-file write cannot prove that a stalled RTSS process consumed
    // the change. Report failure so the provider orchestrator can use a driver
    // fallback while retaining our recovery snapshot for stream cleanup.
    return g_limit_active && !g_hooks_failed;
  }

  bool rtss_streaming_refresh(int fps) {
    if (!config::frame_limiter.enable) {
      return false;
    }

    if (!g_limit_active && !g_settings_dirty) {
      return rtss_streaming_start(fps, 1);
    }

    g_rtss_root = resolve_rtss_root();
    if (!fs::exists(g_rtss_root)) {
      BOOST_LOG(warning) << "RTSS install path not found: "sv << g_rtss_root.string();
      return false;
    }

    ensure_rtss_running(g_rtss_root);

    if (!hooks_available()) {
      (void) load_hooks(g_rtss_root);
    }

    bool dirty = false;

    if (hooks_available()) {
      auto current_flags = get_hook_flags();
      if (current_flags && !g_original_flags.has_value()) {
        g_original_flags = *current_flags;
      }
      if (current_flags && (*current_flags & k_rtss_flag_limiter_disabled)) {
        constexpr DWORD limiter_mask = k_rtss_flag_limiter_disabled;
        auto updated_flags = set_hook_flags(~limiter_mask, 0);
        if (updated_flags && !(*updated_flags & limiter_mask)) {
          g_flags_modified = true;
          dirty = true;
        } else if (!updated_flags) {
          // Treat a timed-out write as potentially applied so cleanup restores it.
          g_flags_modified = true;
          dirty = true;
        }
      }
    }

    int current_denominator = 1;
    auto old_den = set_limit_denominator(g_rtss_root, current_denominator);
    if (old_den.has_value() && *old_den != current_denominator) {
      if (!g_original_denominator.has_value()) {
        g_original_denominator = *old_den;
      }
      g_denominator_modified = true;
      dirty = true;
    }
    if (hooks_available()) {
      (void) update_profiles();
    }

    std::optional<int> sync_limiter_value;
    std::optional<std::string> sync_limiter_label;
    if (g_sync_limiter_override && !g_sync_limiter_override->empty()) {
      if (auto mapped = map_sync_limiter(*g_sync_limiter_override)) {
        sync_limiter_value = mapped;
        sync_limiter_label = *g_sync_limiter_override;
      }
    }
    if (!sync_limiter_value) {
      if (auto v = map_sync_limiter(config::rtss.frame_limit_type)) {
        sync_limiter_value = v;
        if (!config::rtss.frame_limit_type.empty()) {
          sync_limiter_label = config::rtss.frame_limit_type;
        }
      }
    }
    if (sync_limiter_value) {
      bool applied = false;
      if (hooks_available()) {
        applied = set_profile_property_int("SyncLimiter", *sync_limiter_value).has_value();
      }
      if (!applied && write_profile_value_int(g_rtss_root, "SyncLimiter", *sync_limiter_value)) {
        applied = true;
      }
      if (applied) {
        g_sync_limiter_modified = true;
        dirty = true;
        if (sync_limiter_label) {
          BOOST_LOG(info) << "RTSS SyncLimiter refreshed ("sv << *sync_limiter_label << ')';
        } else {
          BOOST_LOG(info) << "RTSS SyncLimiter refreshed";
        }
      }
    }

    int scaled_limit = fps;
    bool applied_limit = false;
    if (hooks_available()) {
      applied_limit = set_profile_property_int("FramerateLimit", scaled_limit).has_value();
    }
    if (!applied_limit && write_profile_value_int(g_rtss_root, "FramerateLimit", scaled_limit)) {
      applied_limit = true;
    }
    if (applied_limit) {
      g_limit_active = true;
      g_limit_modified = true;
      dirty = true;
      BOOST_LOG(info) << "RTSS refreshed framerate limit=" << scaled_limit << " (denominator=" << current_denominator << ")";
    }

    if (dirty && !g_settings_dirty) {
      g_settings_dirty = true;
      recovery_snapshot_t snapshot;
      snapshot.flags_modified = g_flags_modified && g_original_flags.has_value();
      snapshot.original_flags = g_original_flags;
      snapshot.denominator_modified = g_denominator_modified && g_original_denominator.has_value();
      snapshot.original_denominator = g_original_denominator;
      snapshot.limit_modified = g_limit_modified;
      snapshot.original_limit = g_original_limit;
      snapshot.sync_limiter_modified = g_sync_limiter_modified;
      snapshot.original_sync_limiter = g_original_sync_limiter;
      g_recovery_file_owned = write_overrides_file(snapshot);
    }

    return applied_limit && !g_hooks_failed;
  }

  bool rtss_hooks_stalled() {
    return g_hooks_failed;
  }

  void rtss_streaming_stop(bool keep_process_running) {
    g_sync_limiter_override.reset();
    auto cleanup = [&]() {
      g_original_limit.reset();
      g_original_sync_limiter.reset();
      g_original_denominator.reset();
      g_original_flags.reset();
      g_limit_active = false;
      g_settings_dirty = false;
      g_flags_modified = false;
      g_denominator_modified = false;
      g_limit_modified = false;
      g_sync_limiter_modified = false;
      if (g_hooks.module && g_active_hook_calls.load(std::memory_order_acquire) == 0) {
        FreeLibrary(g_hooks.module);
        g_hooks = {};
      }
      if (!keep_process_running) {
        stop_rtss_process();
      }
    };

    if (!g_settings_dirty) {
      if (g_recovery_file_owned) {
        delete_overrides_file();
        g_recovery_file_owned = false;
      }
      cleanup();
      return;
    }

    bool restore_success = true;

    if (hooks_available() && g_flags_modified && g_original_flags.has_value()) {
      constexpr DWORD limiter_mask = k_rtss_flag_limiter_disabled;
      bool limiter_disabled = (*g_original_flags & limiter_mask) != 0;
      DWORD xor_mask = limiter_disabled ? limiter_mask : 0;
      auto updated_flags = set_hook_flags(~limiter_mask, xor_mask);
      if (updated_flags && (*updated_flags & limiter_mask) == xor_mask) {
        BOOST_LOG(info) << "RTSS limiter flags restored"sv;
      } else {
        BOOST_LOG(warning) << "RTSS limiter flags restore mismatch"sv;
        restore_success = false;
      }
    } else if (g_flags_modified && g_original_flags.has_value()) {
      // Flags have no profile-file fallback. Keep the recovery snapshot so a
      // later run can restore them after RTSS begins responding again.
      restore_success = false;
    }

    if (g_denominator_modified && g_original_denominator.has_value()) {
      if (!set_limit_denominator(g_rtss_root, *g_original_denominator).has_value()) {
        restore_success = false;
      }
    }

    if (hooks_available()) {
      if (g_sync_limiter_modified && g_original_sync_limiter.has_value()) {
        if (!set_profile_property_int("SyncLimiter", *g_original_sync_limiter).has_value()) {
          restore_success = false;
        }
      }

      if (g_limit_modified) {
        if (g_original_limit.has_value()) {
          if (set_profile_property_int("FramerateLimit", *g_original_limit).has_value()) {
            BOOST_LOG(info) << "RTSS restored framerate limit=" << *g_original_limit;
          } else {
            restore_success = false;
          }
        } else {
          if (set_profile_property_int("FramerateLimit", 0).has_value()) {
            BOOST_LOG(info) << "RTSS restored framerate limit=<unset> (set 0)";
          } else {
            restore_success = false;
          }
        }
      }
    }
    if (!hooks_available()) {
      if (g_sync_limiter_modified && g_original_sync_limiter.has_value()) {
        if (write_profile_value_int(g_rtss_root, "SyncLimiter", *g_original_sync_limiter)) {
          BOOST_LOG(info) << "RTSS profile SyncLimiter restored to "sv << *g_original_sync_limiter;
        } else {
          restore_success = false;
        }
      }

      if (g_limit_modified) {
        if (g_original_limit.has_value()) {
          if (write_profile_value_int(g_rtss_root, "FramerateLimit", *g_original_limit)) {
            BOOST_LOG(info) << "RTSS profile framerate limit restored to "sv << *g_original_limit;
          } else {
            restore_success = false;
          }
        } else if (write_profile_value_int(g_rtss_root, "FramerateLimit", 0)) {
          BOOST_LOG(info) << "RTSS profile framerate limit restored to 0"sv;
        } else {
          restore_success = false;
        }
      }
    }

    if (restore_success) {
      delete_overrides_file();
    } else {
      BOOST_LOG(warning) << "RTSS overrides: failed to restore one or more settings";
    }
    g_recovery_file_owned = false;

    cleanup();
  }

  bool rtss_is_configured() {
    auto st = rtss_get_status();
    return st.path_exists && st.hooks_found;
  }

  rtss_status_t rtss_get_status() {
    rtss_status_t st {};
    st.enabled = config::frame_limiter.enable;
    st.configured_path = config::rtss.install_path;
    st.path_configured = !config::rtss.install_path.empty();

    // Resolve candidate root
    fs::path root = resolve_rtss_root();
    st.resolved_path = root.string();
    st.path_exists = fs::exists(root);
    st.can_bootstrap_profile = st.path_exists;
    if (st.path_exists) {
      // Check for hooks DLL presence
      bool hooks64 = fs::exists(root / "RTSSHooks64.dll");
      bool hooks = fs::exists(root / "RTSSHooks.dll");
      st.hooks_found = hooks64 || hooks;
      st.profile_found = fs::exists(root / "Profiles" / "Global");
    }
    st.process_running = is_rtss_process_running();
    return st;
  }
}  // namespace platf

#endif  // _WIN32
