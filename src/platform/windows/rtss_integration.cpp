/**
 * @file src/platform/windows/rtss_integration.cpp
 * @brief Apply/restore RTSS frame limit and related properties on stream start/stop.
 */

#ifdef _WIN32

// standard includes
#include <windows.h>
#include <string>
#include <fstream>
#include <filesystem>
#include <optional>
#include <cstdio>

// local includes
#include "src/platform/windows/rtss_integration.h"
#include "src/config.h"
#include "src/logging.h"

using namespace std::literals;
namespace fs = std::filesystem;

namespace platf {

  namespace {
    // RTSSHooks function pointer types
    using fn_LoadProfile = BOOL (__cdecl *)(LPCSTR profileName);
    using fn_SaveProfile = BOOL (__cdecl *)(LPCSTR profileName);
    using fn_GetProfileProperty = BOOL (__cdecl *)(LPCSTR name, LPVOID pBuf, DWORD size);
    using fn_SetProfileProperty = BOOL (__cdecl *)(LPCSTR name, LPVOID pBuf, DWORD size);
    using fn_UpdateProfiles = VOID (__cdecl *)();

    struct hooks_t {
      HMODULE module = nullptr;
      fn_LoadProfile LoadProfile = nullptr;
      fn_SaveProfile SaveProfile = nullptr;
      fn_GetProfileProperty GetProfileProperty = nullptr;
      fn_SetProfileProperty SetProfileProperty = nullptr;
      fn_UpdateProfiles UpdateProfiles = nullptr;

      explicit operator bool() const {
        return module && LoadProfile && SaveProfile && GetProfileProperty && SetProfileProperty && UpdateProfiles;
      }
    };

    hooks_t g_hooks;

    // Remember original values so we can restore on stream end
    std::optional<int> g_original_limit;
    std::optional<int> g_original_sync_limiter;
    std::optional<int> g_original_denominator;

    // Install path resolved from config (root RTSS folder)
    fs::path g_rtss_root;

    // Map config string to SyncLimiter integer
    std::optional<int> map_sync_limiter(const std::string &type) {
      std::string t = type;
      for (auto &c : t) c = (char) ::tolower(c);

      if (t == "async") return 0;
      if (t == "front edge sync" || t == "front_edge_sync") return 1;
      if (t == "back edge sync" || t == "back_edge_sync") return 2;
      if (t == "nvidia reflex" || t == "nvidia_reflex" || t == "reflex") return 3;
      return std::nullopt;
    }

    // Load RTSSHooks DLL from the RTSS root
    bool load_hooks(const fs::path &root) {
      if (g_hooks) return true;

      auto try_load = [&](const wchar_t *dll_name) -> bool {
        fs::path p = root / dll_name;
        HMODULE m = LoadLibraryW(p.c_str());
        if (!m) return false;
        g_hooks.module = m;
        g_hooks.LoadProfile = (fn_LoadProfile) GetProcAddress(m, "LoadProfile");
        g_hooks.SaveProfile = (fn_SaveProfile) GetProcAddress(m, "SaveProfile");
        g_hooks.GetProfileProperty = (fn_GetProfileProperty) GetProcAddress(m, "GetProfileProperty");
        g_hooks.SetProfileProperty = (fn_SetProfileProperty) GetProcAddress(m, "SetProfileProperty");
        g_hooks.UpdateProfiles = (fn_UpdateProfiles) GetProcAddress(m, "UpdateProfiles");
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

    // Read and replace LimitDenominator in Global profile. Returns previous value (or 1 if missing).
    std::optional<int> set_limit_denominator(const fs::path &root, int new_denominator) {
      try {
        auto global_path = root / "Profiles" / "Global";
        if (!fs::exists(global_path)) {
          BOOST_LOG(warning) << "RTSS Global profile not found at: "sv << global_path.string();
          return std::nullopt;
        }

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
      if (!g_hooks) return std::nullopt;
      int value = 0;
      g_hooks.LoadProfile("");
      if (g_hooks.GetProfileProperty(name, &value, sizeof(value))) {
        return value;
      }
      return std::nullopt;
    }

    // Helper: set integer profile property and return previous value if present
    std::optional<int> set_profile_property_int(const char *name, int new_value) {
      if (!g_hooks) return std::nullopt;

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
  } // namespace

  void rtss_streaming_start(int fps) {
    if (!config::rtss.enable_frame_limit) {
      return;
    }

    g_rtss_root = resolve_rtss_root();
    if (!fs::exists(g_rtss_root)) {
      BOOST_LOG(warning) << "RTSS install path not found: "sv << g_rtss_root.string();
      return;
    }

    if (!load_hooks(g_rtss_root)) {
      // We can still change Global profile denominator even if hooks are missing
      BOOST_LOG(warning) << "RTSSHooks not loaded; will only update Global profile denominator"sv;
    }

    // Compute denominator and scaled limit (we have integer fps, so denominator=1)
    int current_denominator = 1;
    int scaled_limit = fps;

    // If hooks are available, capture original values BEFORE making changes
    if (g_hooks) {
      g_original_limit = get_profile_property_int("FramerateLimit");
      g_original_sync_limiter = get_profile_property_int("SyncLimiter");
      BOOST_LOG(info) << "RTSS original values: limit="
                      << (g_original_limit.has_value() ? std::to_string(*g_original_limit) : std::string("<unset>"))
                      << ", syncLimiter="
                      << (g_original_sync_limiter.has_value() ? std::to_string(*g_original_sync_limiter) : std::string("<unset>"));
    }

    // Update LimitDenominator in Global profile and remember previous value
    g_original_denominator = set_limit_denominator(g_rtss_root, current_denominator);
    if (g_hooks) {
      // Nudge RTSS to reload profiles after file change
      g_hooks.UpdateProfiles();
    }

    // Set SyncLimiter based on config (if hooks are available)
    if (g_hooks) {
      if (auto v = map_sync_limiter(config::rtss.frame_limit_type)) {
        set_profile_property_int("SyncLimiter", *v);
      }
    }

    // Set FramerateLimit via hooks (raw value is scaled by denominator)
    if (g_hooks) {
      set_profile_property_int("FramerateLimit", scaled_limit);
      BOOST_LOG(info) << "RTSS applied framerate limit=" << scaled_limit << " (denominator=" << current_denominator << ")";
    }
  }

  void rtss_streaming_stop() {
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
    }

    // Cleanup state
    g_original_limit.reset();
    g_original_sync_limiter.reset();
    g_original_denominator.reset();

    if (g_hooks.module) {
      FreeLibrary(g_hooks.module);
      g_hooks = {};
    }
  }

  rtss_status_t rtss_get_status() {
    rtss_status_t st {};
    st.enabled = config::rtss.enable_frame_limit;
    st.configured_path = config::rtss.install_path;
    st.path_configured = !config::rtss.install_path.empty();

    // Resolve candidate root
    fs::path root = resolve_rtss_root();
    st.resolved_path = root.string();
    st.path_exists = fs::exists(root);
    if (st.path_exists) {
      // Check for hooks DLL presence
      bool hooks64 = fs::exists(root / "RTSSHooks64.dll");
      bool hooks = fs::exists(root / "RTSSHooks.dll");
      st.hooks_found = hooks64 || hooks;
      // Check Global profile
      st.profile_found = fs::exists(root / "Profiles" / "Global");
    }
    return st;
  }
} // namespace platf

#endif // _WIN32
