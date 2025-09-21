/**
 * @file src/platform/windows/rtss_integration.cpp
 * @brief Apply/restore RTSS frame limit and related properties on stream start/stop.
 */

#ifdef _WIN32

  // standard includes
  #include <array>
  #include <cstdio>
  #include <cwchar>
  #include <filesystem>
  #include <fstream>
  #include <optional>
  #include <string>
  #include <system_error>
  #include <vector>

// clang-format off
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

    // Remember original values so we can restore on stream end
    std::optional<int> g_original_limit;
    std::optional<int> g_original_sync_limiter;
    std::optional<int> g_original_denominator;
    std::optional<DWORD> g_original_flags;

    // Install path resolved from config (root RTSS folder)
    fs::path g_rtss_root;

    PROCESS_INFORMATION g_rtss_process_info {};
    bool g_rtss_started_by_sunshine = false;

    constexpr DWORD k_rtss_shutdown_timeout_ms = 5000;
    constexpr DWORD k_rtss_flag_limiter_disabled = 4;

    const std::array<const wchar_t *, 2> k_rtss_process_names = {L"RTSS.exe", L"RTSS64.exe"};
    const std::array<const wchar_t *, 2> k_rtss_executable_names = {L"RTSS.exe", L"RTSS64.exe"};

    const fs::path profile_path(const fs::path &root) {
      return root / "Profiles" / "Global";
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
      std::string cmd_utf8 = "\"" + to_utf8(exe_path) + "\" -s";

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
      if (g_hooks) {
        return true;
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
      if (!g_hooks) {
        return std::nullopt;
      }
      int value = 0;
      g_hooks.LoadProfile("");
      if (g_hooks.GetProfileProperty(name, &value, sizeof(value))) {
        return value;
      }
      return std::nullopt;
    }

    // Helper: set integer profile property and return previous value if present
    std::optional<int> set_profile_property_int(const char *name, int new_value) {
      if (!g_hooks) {
        return std::nullopt;
      }

      int old_value = 0;
      BOOL had_old = FALSE;

      // Empty string selects global profile as in RTSS UI
      g_hooks.LoadProfile("");

      if (g_hooks.GetProfileProperty(name, &old_value, sizeof(old_value))) {
        had_old = TRUE;
      }

      g_hooks.SetProfileProperty(name, &new_value, sizeof(new_value));
      g_hooks.SaveProfile("");
      g_hooks.UpdateProfiles();

      if (had_old) {
        BOOST_LOG(info) << "RTSS property "sv << name << " set to "sv << new_value << ", original "sv << old_value;
      } else {
        BOOST_LOG(info) << "RTSS property "sv << name << " set to "sv << new_value << ", original (implicit) 0"sv;
      }
      // Always return the previous value (0 if not present) so callers can restore it
      return std::optional<int>(old_value);
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

  bool rtss_streaming_start(int fps) {
    g_limit_active = false;

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

    if (g_hooks) {
      DWORD current_flags = g_hooks.GetFlags();
      g_original_flags = current_flags;
      if (current_flags & k_rtss_flag_limiter_disabled) {
        DWORD updated_flags = g_hooks.SetFlags(~k_rtss_flag_limiter_disabled, 0);
        if (updated_flags & k_rtss_flag_limiter_disabled) {
          BOOST_LOG(warning) << "Failed to enable RTSS limiter via SetFlags"sv;
        } else {
          BOOST_LOG(info) << "RTSS limiter enabled via hooks (originally disabled)"sv;
        }
      }
    } else {
      g_original_flags.reset();
    }

    // Compute denominator and scaled limit (we have integer fps, so denominator=1)
    int current_denominator = 1;
    int scaled_limit = fps;

    // Update LimitDenominator in Global profile and remember previous value
    g_original_denominator = set_limit_denominator(g_rtss_root, current_denominator);
    if (g_original_denominator.has_value()) {
      g_limit_active = true;
    }
    if (g_hooks) {
      // Nudge RTSS to reload profiles after file change
      g_hooks.UpdateProfiles();
    }

    // If hooks are available, capture original values BEFORE making further changes
    if (g_hooks) {
      g_original_limit = get_profile_property_int("FramerateLimit");
      g_original_sync_limiter = get_profile_property_int("SyncLimiter");
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

    // Apply SyncLimiter preference
    if (auto v = map_sync_limiter(config::rtss.frame_limit_type)) {
      if (g_hooks) {
        set_profile_property_int("SyncLimiter", *v);
      } else if (write_profile_value_int(g_rtss_root, "SyncLimiter", *v)) {
        BOOST_LOG(info) << "RTSS profile SyncLimiter set to "sv << *v;
      }
    }

    // Apply framerate limit
    if (g_hooks) {
      set_profile_property_int("FramerateLimit", scaled_limit);
      BOOST_LOG(info) << "RTSS applied framerate limit=" << scaled_limit << " (denominator=" << current_denominator << ")";
      g_limit_active = true;
    } else if (write_profile_value_int(g_rtss_root, "FramerateLimit", scaled_limit)) {
      BOOST_LOG(info) << "RTSS profile framerate limit set to "sv << scaled_limit;
      g_limit_active = true;
    }
    return g_limit_active;
  }

  void rtss_streaming_stop() {
    if (!g_limit_active && !g_original_denominator && !g_original_limit && !g_original_sync_limiter && !g_original_flags) {
      if (g_hooks.module) {
        FreeLibrary(g_hooks.module);
        g_hooks = {};
      }
      g_limit_active = false;
      stop_rtss_process();
      return;
    }

    if (g_hooks && g_original_flags.has_value()) {
      bool limiter_disabled = (*g_original_flags & k_rtss_flag_limiter_disabled) != 0;
      DWORD updated_flags = g_hooks.SetFlags(~k_rtss_flag_limiter_disabled, limiter_disabled ? k_rtss_flag_limiter_disabled : 0);
      if ((updated_flags & k_rtss_flag_limiter_disabled) == (limiter_disabled ? k_rtss_flag_limiter_disabled : 0)) {
        BOOST_LOG(info) << "RTSS limiter flags restored"sv;
      } else {
        BOOST_LOG(warning) << "RTSS limiter flags restore mismatch"sv;
      }
    }

    // Always attempt to restore if we previously applied any changes,
    // regardless of current config state. Users may toggle the setting
    // during a stream; we still need to revert to the original values.

    // Restore denominator in Global profile first to ensure raw limit maps back correctly
    if (g_original_denominator.has_value()) {
      set_limit_denominator(g_rtss_root, *g_original_denominator);
    }

    if (g_hooks) {
      // Restore SyncLimiter (if we captured it); otherwise leave as-is
      if (g_original_sync_limiter.has_value()) {
        set_profile_property_int("SyncLimiter", *g_original_sync_limiter);
      }

      // Restore FramerateLimit; if unknown, disable (0)
      if (g_original_limit.has_value()) {
        set_profile_property_int("FramerateLimit", *g_original_limit);
        BOOST_LOG(info) << "RTSS restored framerate limit=" << *g_original_limit;
      } else {
        set_profile_property_int("FramerateLimit", 0);
        BOOST_LOG(info) << "RTSS restored framerate limit=<unset> (set 0)";
      }
    } else {
      if (g_original_sync_limiter.has_value()) {
        if (write_profile_value_int(g_rtss_root, "SyncLimiter", *g_original_sync_limiter)) {
          BOOST_LOG(info) << "RTSS profile SyncLimiter restored to "sv << *g_original_sync_limiter;
        }
      }

      if (g_original_limit.has_value()) {
        if (write_profile_value_int(g_rtss_root, "FramerateLimit", *g_original_limit)) {
          BOOST_LOG(info) << "RTSS profile framerate limit restored to "sv << *g_original_limit;
        }
      } else if (write_profile_value_int(g_rtss_root, "FramerateLimit", 0)) {
        BOOST_LOG(info) << "RTSS profile framerate limit restored to 0"sv;
      }
    }

    // Cleanup state
    g_original_limit.reset();
    g_original_sync_limiter.reset();
    g_original_denominator.reset();
    g_original_flags.reset();
    g_limit_active = false;

    if (g_hooks.module) {
      FreeLibrary(g_hooks.module);
      g_hooks = {};
    }

    stop_rtss_process();
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
