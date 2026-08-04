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
  #include <limits>
  #include <memory>
  #include <mutex>
  #include <nlohmann/json.hpp>
  #include <optional>
  #include <string>
  #include <string_view>
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
    // RTSS' profile SDK declares load/save as void. Treating their undefined
    // return registers as BOOL makes successful operations look like failures.
    using fn_LoadProfile = VOID(__cdecl *)(LPCSTR profileName);
    using fn_SaveProfile = VOID(__cdecl *)(LPCSTR profileName);
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
        return module && LoadProfile && UpdateProfiles && GetFlags && SetFlags;
      }
    };

    hooks_t g_hooks;
    bool g_limit_active = false;
    rtss_apply_result g_last_apply_result = rtss_apply_result::safe_to_fallback;
    bool g_recovery_file_owned = false;
    bool g_settings_dirty = false;
    bool g_flags_modified = false;
    bool g_denominator_modified = false;
    bool g_limit_modified = false;
    bool g_sync_limiter_modified = false;
    std::recursive_mutex g_rtss_lifecycle_mutex;

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
    // RTSSHooks talks to the RTSS process through its message loop. Until that loop is
    // pumping, hook calls block and trip the stall watchdog below. Readiness belongs
    // to one process instance, so remember its PID rather than a process-global bit.
    std::optional<DWORD> g_rtss_ready_pid;
    struct hook_call_state_t {
      std::atomic<unsigned int> active_calls {0};
    };

    // Timed-out RTSS calls must be allowed to finish without touching a
    // destroyed static atomic during CRT teardown. Each detached call owns a
    // shared reference to this state until it has returned.
    std::shared_ptr<hook_call_state_t> g_hook_call_state = std::make_shared<hook_call_state_t>();
    bool g_hooks_failed = false;

    constexpr DWORD k_rtss_shutdown_timeout_ms = 5000;
    constexpr auto k_rtss_response_timeout = std::chrono::seconds(1);
    constexpr DWORD k_rtss_ready_timeout_ms = 3000;
    constexpr DWORD k_rtss_flag_limiter_disabled = 4;
    constexpr char k_rtss_limit_profile_key[] = "Limit";
    constexpr char k_rtss_denominator_profile_key[] = "LimitDenominator";
    constexpr char k_rtss_sync_limiter_profile_key[] = "SyncLimiter";
    constexpr char k_rtss_framerate_section[] = "[Framerate]";

    const std::array<const wchar_t *, 2> k_rtss_process_names = {L"RTSS.exe", L"RTSS64.exe"};
    const std::array<const wchar_t *, 2> k_rtss_executable_names = {L"RTSS.exe", L"RTSS64.exe"};

    rtss_apply_result mark_safe_to_fallback() {
      g_limit_active = false;
      g_last_apply_result = rtss_apply_result::safe_to_fallback;
      return g_last_apply_result;
    }

    rtss_apply_result mark_applied() {
      g_limit_active = true;
      g_last_apply_result = rtss_apply_result::applied;
      return g_last_apply_result;
    }

    rtss_apply_result retain_rtss_exclusive(std::string_view reason) {
      g_limit_active = true;
      g_last_apply_result = rtss_apply_result::retained_exclusive;
      BOOST_LOG(warning) << reason
                         << " RTSS remains the exclusive frame-limiter provider to avoid a second limiter.";
      return g_last_apply_result;
    }

    const fs::path profile_path(const fs::path &root) {
      return root / "Profiles" / "Global";
    }

    bool load_hooks(const fs::path &root);
    bool hooks_available();
    std::optional<DWORD> get_hook_flags();
    std::optional<DWORD> set_hook_flags(
      DWORD and_mask,
      DWORD xor_mask,
      bool *call_scheduled = nullptr,
      bool reload_profiles = false
    );
    bool write_framerate_values(
      const fs::path &root,
      const std::optional<int> *limit,
      const std::optional<int> *denominator,
      const std::optional<int> *sync_limiter
    );
    bool ensure_rtss_running(const fs::path &root);
    bool reload_profiles_from_disk(
      bool mark_hooks_failed = true,
      bool *call_scheduled = nullptr
    );
    fs::path resolve_rtss_root();

    template<typename Result, typename Callable>
    std::optional<Result> call_rtss_hooks(
      const char *operation,
      Callable &&callable,
      bool mark_hooks_failed = true,
      bool *call_scheduled = nullptr
    ) {
      if (call_scheduled) {
        *call_scheduled = false;
      }
      std::promise<Result> promise;
      auto result = promise.get_future();
      auto call_state = g_hook_call_state;
      call_state->active_calls.fetch_add(1, std::memory_order_acq_rel);

      std::thread worker;
      try {
        worker = std::thread([
                               promise = std::move(promise),
                               callable = std::forward<Callable>(callable),
                               call_state
                             ]() mutable {
          try {
            promise.set_value(callable());
          } catch (...) {
            promise.set_exception(std::current_exception());
          }
          call_state->active_calls.fetch_sub(1, std::memory_order_acq_rel);
        });
        if (call_scheduled) {
          *call_scheduled = true;
        }
      } catch (const std::exception &ex) {
        call_state->active_calls.fetch_sub(1, std::memory_order_acq_rel);
        BOOST_LOG(error) << "Unable to start RTSS hooks operation '" << operation << "': " << ex.what();
        if (mark_hooks_failed) {
          g_hooks_failed = true;
        }
        return std::nullopt;
      }

      if (result.wait_for(k_rtss_response_timeout) != std::future_status::ready) {
        if (mark_hooks_failed) {
          g_hooks_failed = true;
        }
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
      if (mark_hooks_failed) {
        g_hooks_failed = true;
      }
      return std::nullopt;
    }

    bool hooks_available() {
      return static_cast<bool>(g_hooks) && !g_hooks_failed && g_hook_call_state->active_calls.load(std::memory_order_acquire) == 0;
    }

    bool profile_hooks_available() {
      return hooks_available() && g_hooks.SaveProfile && g_hooks.GetProfileProperty && g_hooks.SetProfileProperty;
    }

    bool profile_hooks_support_fractional_limits(const fs::path &root) {
      std::string version;
      if (!getFileVersionInfo(root / "RTSS.exe", version)) {
        BOOST_LOG(warning) << "Could not determine RTSS version; using the profile file instead of the RTSSHooks profile SDK.";
        return false;
      }

      unsigned int major = 0;
      unsigned int minor = 0;
      unsigned int build = 0;
      unsigned int revision = 0;
      if (std::sscanf(version.c_str(), "%u.%u.%u.%u", &major, &minor, &build, &revision) != 4) {
        BOOST_LOG(warning) << "Could not parse RTSS version '" << version << "'; using the profile file instead of the RTSSHooks profile SDK.";
        return false;
      }

      const bool supported = major > 7 ||
                             (major == 7 && (minor > 3 || (minor == 3 && build >= 7)));
      if (!supported) {
        BOOST_LOG(info) << "RTSS " << version << " predates fractional profile SDK support in 7.3.7; using the profile file.";
      }
      return supported;
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

    std::optional<DWORD> set_hook_flags(
      DWORD and_mask,
      DWORD xor_mask,
      bool *call_scheduled,
      bool reload_profiles
    ) {
      if (call_scheduled) {
        *call_scheduled = false;
      }
      if (!hooks_available()) {
        return std::nullopt;
      }
      auto set_flags = g_hooks.SetFlags;
      auto load_profile = g_hooks.LoadProfile;
      auto update_profiles = g_hooks.UpdateProfiles;
      return call_rtss_hooks<DWORD>(
        reload_profiles ? "SetFlags/LoadProfile/UpdateProfiles" : "SetFlags",
        [set_flags, load_profile, update_profiles, and_mask, xor_mask, reload_profiles]() {
          const auto flags = set_flags(and_mask, xor_mask);
          if (reload_profiles) {
            load_profile("");
            update_profiles();
          }
          return flags;
        },
        true,
        call_scheduled
      );
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

      try {
        // The recovery snapshot is the commit point for a profile mutation.
        // Never truncate its live copy in place: a power loss between truncate
        // and write would make the pre-stream limit unrecoverable.
        const fs::path temporary_path = file_path.wstring() + L".sunshine." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
        {
          std::ofstream out(temporary_path, std::ios::binary | std::ios::trunc);
          if (!out.is_open()) {
            BOOST_LOG(warning) << "RTSS overrides: failed to open recovery file for write";
            return false;
          }
          out << j.dump();
          out.flush();
          if (!out.good()) {
            BOOST_LOG(warning) << "RTSS overrides: failed to write recovery file";
            std::error_code remove_ec;
            fs::remove(temporary_path, remove_ec);
            return false;
          }
        }
        if (!MoveFileExW(temporary_path.c_str(), file_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
          const auto error = GetLastError();
          std::error_code remove_ec;
          fs::remove(temporary_path, remove_ec);
          BOOST_LOG(warning) << "RTSS overrides: failed to atomically replace recovery file (winerr=" << error << ").";
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
      auto decode = [&](const char *key, bool &modified, auto &value_opt) -> bool {
        modified = false;
        value_opt.reset();
        if (!j.contains(key)) {
          return true;
        }
        const auto &node = j[key];
        if (!node.is_object()) {
          return false;
        }
        if (node.contains("modified") && !node["modified"].is_boolean()) {
          return false;
        }
        modified = node.value("modified", false);
        if (!modified) {
          return true;
        }
        if (!node.contains("value")) {
          return false;
        }
        if (node["value"].is_null()) {
          return true;
        }
        if (!node["value"].is_number_integer() && !node["value"].is_number_unsigned()) {
          return false;
        }
        try {
          using value_type = typename std::decay_t<decltype(value_opt)>::value_type;
          const auto raw = node["value"].get<long long>();
          if (raw < static_cast<long long>(std::numeric_limits<value_type>::lowest()) ||
              raw > static_cast<long long>(std::numeric_limits<value_type>::max())) {
            return false;
          }
          value_opt = static_cast<value_type>(raw);
        } catch (...) {
          return false;
        }
        return true;
      };

      const bool snapshot_valid =
        decode("flags", snapshot.flags_modified, snapshot.original_flags) &&
        decode("denominator", snapshot.denominator_modified, snapshot.original_denominator) &&
        decode("limit", snapshot.limit_modified, snapshot.original_limit) &&
        decode("sync_limiter", snapshot.sync_limiter_modified, snapshot.original_sync_limiter);
      if (!snapshot_valid) {
        BOOST_LOG(warning) << "RTSS overrides: recovery file contains an invalid original value";
        return std::nullopt;
      }
      if (snapshot.flags_modified && !snapshot.original_flags) {
        BOOST_LOG(warning) << "RTSS overrides: recovery file is missing original limiter flags";
        return std::nullopt;
      }

      if (!snapshot_has_changes(snapshot)) {
        return std::nullopt;
      }

      return snapshot;
    }

    bool delete_overrides_file() {
      auto file_path_opt = rtss_overrides_file_path();
      if (!file_path_opt) {
        return false;
      }
      std::error_code ec;
      fs::remove(*file_path_opt, ec);
      if (ec) {
        BOOST_LOG(warning) << "RTSS overrides: failed to delete recovery file: " << ec.message();
        return false;
      }
      return true;
    }

    enum class recovery_result_t {
      /// Nothing was left over, or the persisted mutation was undone.
      resolved,
      /// RTSS is provably not limiting anything: its install root is gone, or it
      /// is not running and could not be started. A persisted snapshot cannot be
      /// in force, so callers must release the provider slot instead of retaining
      /// exclusivity they cannot honour.
      rtss_absent,
      /// The persisted state could not be resolved and live RTSS state is
      /// uncertain, so exclusivity must be retained.
      unresolved
    };

    recovery_result_t restore_from_snapshot(const recovery_snapshot_t &snapshot) {
      fs::path root = resolve_rtss_root();
      if (!fs::exists(root)) {
        BOOST_LOG(warning) << "RTSS overrides: install path not found for recovery: "sv << root.string();
        return recovery_result_t::rtss_absent;
      }
      if (!ensure_rtss_running(root)) {
        // Same predicate rtss_streaming_start already falls back on: RTSS was not
        // running and could not be started, so it is enforcing nothing.
        BOOST_LOG(warning) << "RTSS overrides: unable to start RTSS for recovery";
        return recovery_result_t::rtss_absent;
      }

      bool hooks_loaded = false;
      auto unload_hooks = [&]() {
        if (hooks_loaded && g_hooks.module && g_hook_call_state->active_calls.load(std::memory_order_acquire) == 0) {
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

      const auto *limit = snapshot.limit_modified ? &snapshot.original_limit : nullptr;
      const auto *denominator = snapshot.denominator_modified ? &snapshot.original_denominator : nullptr;
      const auto *sync_limiter = snapshot.sync_limiter_modified ? &snapshot.original_sync_limiter : nullptr;
      if (limit || denominator || sync_limiter) {
        if (!write_framerate_values(root, limit, denominator, sync_limiter)) {
          success = false;
        } else if (!ensure_hooks_loaded() || !reload_profiles_from_disk()) {
          // Keep the durable recovery file if RTSS could not acknowledge the
          // atomic profile transaction. A later process start can retry.
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
      return success ? recovery_result_t::resolved : recovery_result_t::unresolved;
    }

    /**
     * @brief Resolve any durable RTSS recovery state left by an earlier stream.
     * @param resolved_mutation Optional out-flag, set only when persisted state describing a
     *        real mutation was found and dealt with. It stays false when there was simply
     *        nothing to restore, which lets callers tell "Sunshine owes exclusivity" apart
     *        from "Sunshine never mutated anything".
     * @return resolved when no unresolved recovery state remains, rtss_absent when the
     *         snapshot cannot be in force because RTSS is gone, unresolved otherwise.
     */
    recovery_result_t maybe_restore_from_overrides_file(bool *resolved_mutation = nullptr) {
      if (resolved_mutation) {
        *resolved_mutation = false;
      }
      if (g_recovery_file_owned) {
        return recovery_result_t::unresolved;
      }

      auto file_path_opt = rtss_overrides_file_path();
      if (!file_path_opt) {
        BOOST_LOG(warning) << "RTSS overrides: unable to resolve the recovery path; persisted state cannot be ruled out.";
        return recovery_result_t::unresolved;
      }
      std::error_code exists_ec;
      const bool recovery_file_exists = fs::exists(*file_path_opt, exists_ec);
      if (exists_ec) {
        BOOST_LOG(warning) << "RTSS overrides: unable to check recovery file: " << exists_ec.message();
        return recovery_result_t::unresolved;
      }
      if (!recovery_file_exists) {
        return recovery_result_t::resolved;
      }
      if (g_hook_call_state->active_calls.load(std::memory_order_acquire) != 0) {
        BOOST_LOG(warning) << "RTSS overrides: a late hooks operation is still active; deferring recovery.";
        return recovery_result_t::unresolved;
      }

      auto snapshot = read_overrides_file();
      if (!snapshot) {
        // The file cannot describe a restorable mutation (unreadable, unparseable, invalid,
        // or empty). Retaining exclusivity forever would wedge the limiter, so delete it:
        // a successful delete proves the persisted state is gone. A file that is locked
        // hard enough to resist deletion is the same file that resisted the read, so the
        // conservative retain still covers transient failures.
        if (!delete_overrides_file()) {
          BOOST_LOG(warning) << "RTSS overrides: pending recovery file could not be read or deleted; retaining RTSS provider ownership.";
          return recovery_result_t::unresolved;
        }
        BOOST_LOG(warning) << "RTSS overrides: pending recovery file could not be read; discarded it and verified live RTSS instead.";
        if (resolved_mutation) {
          *resolved_mutation = true;
        }
        return recovery_result_t::resolved;
      }

      BOOST_LOG(info) << "RTSS overrides: pending recovery file detected; attempting restore";
      const auto restored = restore_from_snapshot(*snapshot);
      if (restored != recovery_result_t::resolved) {
        // Deliberately keep the recovery file when RTSS is absent. It is the only
        // record of the user's original Global profile values, it can no longer
        // wedge the limiter now that callers release the provider slot on absence,
        // and a later stream can still undo the mutation once RTSS comes back.
        return restored;
      }
      // The mutation is already undone, so a leftover file no longer describes live state.
      // Failing to delete it must not wedge the limiter for every future stream.
      if (!delete_overrides_file()) {
        BOOST_LOG(warning) << "RTSS overrides: restored the recovery snapshot, but the stale recovery file could not be deleted.";
      }
      if (resolved_mutation) {
        *resolved_mutation = true;
      }
      return recovery_result_t::resolved;
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

    std::string_view trim_profile_line(std::string_view line) {
      while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.remove_prefix(1);
      }
      while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
        line.remove_suffix(1);
      }
      return line;
    }

    size_t find_framerate_section_body(const std::string &content) {
      size_t pos = 0;
      while (pos <= content.size()) {
        const auto end = content.find_first_of("\r\n", pos);
        const auto length = (end == std::string::npos ? content.size() : end) - pos;
        if (trim_profile_line(std::string_view(content).substr(pos, length)) == k_rtss_framerate_section) {
          if (end == std::string::npos) {
            return content.size();
          }
          const auto body = content.find_first_not_of("\r\n", end);
          return body == std::string::npos ? content.size() : body;
        }
        if (end == std::string::npos) {
          break;
        }
        pos = end + 1;
      }
      return std::string::npos;
    }

    size_t find_framerate_key(const std::string &content, size_t section_body, std::string_view key) {
      if (section_body == std::string::npos) {
        return std::string::npos;
      }
      const std::string prefix = std::string(key) + '=';
      size_t pos = section_body;
      while (pos < content.size()) {
        const auto end = content.find_first_of("\r\n", pos);
        const auto length = (end == std::string::npos ? content.size() : end) - pos;
        const auto line = std::string_view(content).substr(pos, length);
        if (!line.empty() && line.front() == '[') {
          return std::string::npos;
        }
        if (line.compare(0, prefix.size(), prefix) == 0) {
          return pos;
        }
        if (end == std::string::npos) {
          break;
        }
        pos = end + 1;
      }
      return std::string::npos;
    }

    std::optional<int> parse_profile_value(const std::string &content, size_t pos) {
      const auto end = content.find_first_of("\r\n", pos);
      const auto line = content.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
      const auto equals = line.find('=');
      if (equals == std::string::npos) {
        return std::nullopt;
      }
      try {
        size_t consumed = 0;
        const int value = std::stoi(line.substr(equals + 1), &consumed);
        return consumed == line.size() - equals - 1 ? std::optional<int> {value} : std::nullopt;
      } catch (...) {
        return std::nullopt;
      }
    }

    bool set_framerate_key(std::string &content, std::string_view key, const std::optional<int> &value) {
      const auto body = find_framerate_section_body(content);
      if (body == std::string::npos) {
        return false;
      }
      const auto pos = find_framerate_key(content, body, key);
      if (!value) {
        if (pos == std::string::npos) {
          return true;
        }
        auto end = content.find_first_of("\r\n", pos);
        if (end == std::string::npos) {
          content.erase(pos);
        } else {
          while (end < content.size() && (content[end] == '\r' || content[end] == '\n')) {
            ++end;
          }
          content.erase(pos, end - pos);
        }
        return true;
      }

      char replacement[64];
      snprintf(replacement, sizeof(replacement), "%.*s=%d", static_cast<int>(key.size()), key.data(), *value);
      if (pos != std::string::npos) {
        const auto end = content.find_first_of("\r\n", pos);
        content.replace(pos, (end == std::string::npos ? content.size() : end) - pos, replacement);
      } else {
        const auto eol = content.find("\r\n") != std::string::npos ? "\r\n" : "\n";
        content.insert(body, std::string(replacement) + eol);
      }
      return true;
    }

    bool write_profile_content_in_place(const fs::path &path, const std::string &content) {
      const HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        nullptr
      );
      if (file == INVALID_HANDLE_VALUE) {
        BOOST_LOG(warning) << "Failed opening RTSS Global profile for in-place update (winerr=" << GetLastError() << ").";
        return false;
      }

      DWORD written = 0;
      const bool success =
        content.size() <= std::numeric_limits<DWORD>::max() &&
        WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) &&
        written == content.size() &&
        SetEndOfFile(file) &&
        FlushFileBuffers(file);
      const auto error = success ? ERROR_SUCCESS : GetLastError();
      CloseHandle(file);
      if (!success) {
        BOOST_LOG(warning) << "Failed writing RTSS Global profile in place (winerr=" << error << ").";
      }
      return success;
    }

    bool write_profile_content_atomically(const fs::path &path, const std::string &content) {
      const fs::path temporary_path = path.wstring() + L".sunshine." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
      try {
        {
          std::ofstream out(temporary_path, std::ios::out | std::ios::binary | std::ios::trunc);
          if (!out) {
            return false;
          }
          out.write(content.data(), static_cast<std::streamsize>(content.size()));
          out.flush();
          if (!out.good()) {
            return false;
          }
        }
        if (!MoveFileExW(temporary_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
          const auto error = GetLastError();
          std::error_code ec;
          fs::remove(temporary_path, ec);
          if (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION) {
            // RTSS opens the selected profile without delete sharing, which
            // prevents replacing its directory entry while the UI is running.
            // Updating the complete profile in one write is compatible with
            // that lock; RTSS does not consume it until UpdateProfiles below.
            if (write_profile_content_in_place(path, content)) {
              BOOST_LOG(info) << "Updated RTSS Global profile in place because RTSS blocked atomic replacement.";
              return true;
            }
          }
          BOOST_LOG(warning) << "Failed atomically replacing RTSS Global profile (winerr=" << error << ").";
          return false;
        }
        return true;
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Failed atomically writing RTSS Global profile: "sv << e.what();
        return false;
      }
    }

    std::optional<bool> write_framerate_values_via_hooks(
      const fs::path &root,
      const std::optional<int> *limit,
      const std::optional<int> *denominator,
      const std::optional<int> *sync_limiter
    ) {
      // The SDK cannot remove a property. Use the file path for restoration of
      // a profile key that was absent before the stream.
      if (!profile_hooks_available() ||
          !profile_hooks_support_fractional_limits(root) ||
          (limit && !*limit) ||
          (denominator && !*denominator) ||
          (sync_limiter && !*sync_limiter)) {
        return std::nullopt;
      }

      const auto load_profile = g_hooks.LoadProfile;
      const auto save_profile = g_hooks.SaveProfile;
      const auto get_property = g_hooks.GetProfileProperty;
      const auto set_property = g_hooks.SetProfileProperty;
      const auto update_profiles = g_hooks.UpdateProfiles;
      const bool has_limit = limit != nullptr;
      const bool has_denominator = denominator != nullptr;
      const bool has_sync_limiter = sync_limiter != nullptr;
      const auto limit_value = has_limit ? *limit : std::optional<int> {};
      const auto denominator_value = has_denominator ? *denominator : std::optional<int> {};
      const auto sync_limiter_value = has_sync_limiter ? *sync_limiter : std::optional<int> {};
      return call_rtss_hooks<bool>(
        "LoadProfile/SetProfileProperty/SaveProfile/UpdateProfiles",
        [load_profile,
         save_profile,
         get_property,
         set_property,
         update_profiles,
         has_limit,
         has_denominator,
         has_sync_limiter,
         limit_value,
         denominator_value,
         sync_limiter_value]() {
          load_profile("");

          auto set_value = [set_property](const char *name, bool enabled, const std::optional<int> &value) {
            if (!enabled) {
              return true;
            }
            int raw_value = *value;
            return set_property(name, &raw_value, sizeof(raw_value)) != FALSE;
          };
          if (!set_value("FramerateLimit", has_limit, limit_value) ||
              !set_value("FramerateLimitDenominator", has_denominator, denominator_value) ||
              !set_value("SyncLimiter", has_sync_limiter, sync_limiter_value)) {
            return false;
          }

          save_profile("");
          update_profiles();

          auto get_value = [get_property](const char *name, bool enabled, const std::optional<int> &value) {
            if (!enabled) {
              return true;
            }
            int actual_value = 0;
            return get_property(name, &actual_value, sizeof(actual_value)) != FALSE && actual_value == *value;
          };
          return get_value("FramerateLimit", has_limit, limit_value) &&
                 get_value("FramerateLimitDenominator", has_denominator, denominator_value) &&
                 get_value("SyncLimiter", has_sync_limiter, sync_limiter_value);
        },
        true
      );
    }

    bool write_framerate_values(
      const fs::path &root,
      const std::optional<int> *limit,
      const std::optional<int> *denominator,
      const std::optional<int> *sync_limiter
    ) {
      try {
        if (const auto sdk_result = write_framerate_values_via_hooks(root, limit, denominator, sync_limiter)) {
          if (*sdk_result) {
            BOOST_LOG(info) << "Updated RTSS Global profile through RTSSHooks profile SDK.";
            return true;
          }
          BOOST_LOG(warning) << "RTSSHooks profile SDK rejected the requested profile update; falling back to the profile file.";
        }

        if (!ensure_profile_exists(root)) {
          return false;
        }
        const auto path = profile_path(root);
        std::ifstream in(path, std::ios::in | std::ios::binary);
        if (!in) {
          return false;
        }
        std::string content {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        if ((limit && !set_framerate_key(content, k_rtss_limit_profile_key, *limit)) ||
            (denominator && !set_framerate_key(content, k_rtss_denominator_profile_key, *denominator)) ||
            (sync_limiter && !set_framerate_key(content, k_rtss_sync_limiter_profile_key, *sync_limiter))) {
          BOOST_LOG(warning) << "RTSS Global profile has no [Framerate] section.";
          return false;
        }
        return write_profile_content_atomically(path, content);
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Failed updating RTSS Global profile: "sv << e.what();
        return false;
      }
    }

    bool reload_profiles_from_disk(bool mark_hooks_failed, bool *call_scheduled) {
      if (!hooks_available()) {
        if (call_scheduled) {
          *call_scheduled = false;
        }
        return false;
      }
      const auto load_profile = g_hooks.LoadProfile;
      const auto update = g_hooks.UpdateProfiles;
      return call_rtss_hooks<bool>("LoadProfile/UpdateProfiles", [load_profile, update]() {
               load_profile("");
               update();
               return true;
             },
                                   mark_hooks_failed,
                                   call_scheduled)
        .value_or(false);
    }

    struct profile_value_read_t {
      bool known = false;
      std::optional<int> value;
    };

    profile_value_read_t read_profile_value_int(const fs::path &root, const char *key) {
      const auto path = profile_path(root);
      std::error_code exists_ec;
      const bool profile_exists = fs::exists(path, exists_ec);
      if (exists_ec) {
        BOOST_LOG(warning) << "Failed checking RTSS Global profile while reading '"sv << key << "': "sv << exists_ec.message();
        return {};
      }
      if (!profile_exists) {
        return {.known = true, .value = std::nullopt};
      }
      try {
        std::ifstream in(path, std::ios::in | std::ios::binary);
        if (!in) {
          BOOST_LOG(warning) << "Failed opening RTSS Global profile while reading '"sv << key << "'.";
          return {};
        }
        std::string content {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        if (in.bad()) {
          BOOST_LOG(warning) << "Failed reading RTSS Global profile value '"sv << key << "'.";
          return {};
        }
        const auto pos = find_framerate_key(content, find_framerate_section_body(content), key);
        if (pos == std::string::npos) {
          return {.known = true, .value = std::nullopt};
        }
        auto value = parse_profile_value(content, pos);
        if (!value) {
          BOOST_LOG(warning) << "RTSS Global profile value '"sv << key << "' is invalid.";
          return {};
        }
        return {.known = true, .value = value};
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Failed reading RTSS profile value '"sv << key << "': "sv << e.what();
        return {};
      }
    }

    struct rtss_process_probe_t {
      bool known = false;
      std::optional<DWORD> pid;
    };

    rtss_process_probe_t probe_rtss_process_id() {
      HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
      if (snapshot == INVALID_HANDLE_VALUE) {
        return {};
      }

      PROCESSENTRY32W entry {};
      entry.dwSize = sizeof(entry);
      std::optional<DWORD> pid;
      bool enumeration_known = false;
      if (Process32FirstW(snapshot, &entry)) {
        enumeration_known = true;
        do {
          for (auto name : k_rtss_process_names) {
            if (_wcsicmp(entry.szExeFile, name) == 0) {
              pid = entry.th32ProcessID;
              break;
            }
          }
        } while (!pid && Process32NextW(snapshot, &entry));
        if (!pid && GetLastError() != ERROR_NO_MORE_FILES) {
          enumeration_known = false;
        }
      } else if (GetLastError() == ERROR_NO_MORE_FILES) {
        enumeration_known = true;
      }

      CloseHandle(snapshot);
      return {.known = enumeration_known, .pid = pid};
    }

    std::optional<DWORD> find_rtss_process_id() {
      return probe_rtss_process_id().pid;
    }

    bool is_rtss_process_running() {
      return find_rtss_process_id().has_value();
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
      g_rtss_ready_pid.reset();
    }

    /**
     * @brief Block until the running RTSS instance is pumping messages.
     *
     * RTSSHooks marshals every profile/flag call into the RTSS process, so calling into
     * the hooks while RTSS is still initializing blocks for longer than the one-second
     * stall watchdog allows. That marks the hooks as failed and silently hands the stream
     * to the NVIDIA limiter, which is the classic "RTSS only works some of the time"
     * symptom right after Sunshine launches RTSS itself.
     */
    void wait_for_rtss_ready() {
      HANDLE handle = g_rtss_process_info.hProcess;
      bool owned_handle = false;
      DWORD pid = handle ? GetProcessId(handle) : 0;
      if (!handle) {
        const auto found_pid = find_rtss_process_id();
        if (!found_pid) {
          return;
        }
        pid = *found_pid;
        handle = OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, pid);
        owned_handle = handle != nullptr;
      }
      if (!handle || pid == 0) {
        return;
      }
      if (g_rtss_ready_pid && *g_rtss_ready_pid == pid) {
        if (owned_handle) {
          CloseHandle(handle);
        }
        return;
      }

      const DWORD result = WaitForInputIdle(handle, k_rtss_ready_timeout_ms);
      const DWORD wait_error = result == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
      if (owned_handle) {
        CloseHandle(handle);
      }

      if (result == WAIT_OBJECT_0) {
        g_rtss_ready_pid = pid;
        return;
      }
      if (result == WAIT_TIMEOUT) {
        BOOST_LOG(warning) << "RTSS did not finish initializing within "sv
                           << k_rtss_ready_timeout_ms << "ms; continuing without waiting."sv;
        return;
      }
      if (result == WAIT_FAILED) {
        BOOST_LOG(warning) << "Unable to confirm RTSS input readiness (winerr=" << wait_error << ").";
      }
    }

    bool ensure_rtss_running(const fs::path &root) {
      // If we previously launched RTSS, check if the process is still alive.
      if (g_rtss_process_info.hProcess) {
        DWORD exit_code = 0;
        if (GetExitCodeProcess(g_rtss_process_info.hProcess, &exit_code) && exit_code == STILL_ACTIVE) {
          wait_for_rtss_ready();
          return true;
        }
        reset_rtss_process_state();
      }

      if (is_rtss_process_running()) {
        wait_for_rtss_ready();
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
      g_rtss_ready_pid.reset();
      BOOST_LOG(info) << "Launched RTSS for frame limiter support"sv;
      wait_for_rtss_ready();
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
      if (g_hooks.module || g_hook_call_state->active_calls.load(std::memory_order_acquire) != 0 || g_hooks_failed) {
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

  rtss_recovery_audit_result rtss_audit_pending_recovery() {
    std::scoped_lock lock {g_rtss_lifecycle_mutex};
    auto retain_exclusive = []() {
      g_limit_active = true;
      g_last_apply_result = rtss_apply_result::retained_exclusive;
      return rtss_recovery_audit_result::retained_exclusive;
    };
    auto clear_for_other_provider = []() {
      g_limit_active = false;
      g_last_apply_result = rtss_apply_result::safe_to_fallback;
      return rtss_recovery_audit_result::clear;
    };

    if (g_recovery_file_owned && g_last_apply_result != rtss_apply_result::safe_to_fallback) {
      return retain_exclusive();
    }
    if (g_hook_call_state->active_calls.load(std::memory_order_acquire) == 0) {
      g_hooks_failed = false;
    }
    if (!g_recovery_file_owned) {
      bool resolved_mutation = false;
      const auto recovery = maybe_restore_from_overrides_file(&resolved_mutation);
      if (recovery == recovery_result_t::rtss_absent) {
        // The live-state proof below is exactly this answer, but it is only
        // reachable once recovery stops short-circuiting to retain. An absent
        // RTSS cannot be enforcing the snapshot, so hand the slot over instead
        // of claiming an exclusivity that leaves the stream with no limiter.
        BOOST_LOG(warning) << "RTSS recovery audit found persisted overrides but RTSS is absent; releasing the provider slot.";
        return clear_for_other_provider();
      }
      if (recovery != recovery_result_t::resolved) {
        return retain_exclusive();
      }
      if (!resolved_mutation) {
        // Nothing was owned and nothing needed restoring, so Sunshine has no outstanding
        // mutation of its own. It owes no exclusivity here: probing the user's live RTSS
        // would hand their own unrelated Global limit the provider slot, and the probe's
        // profile reload would revert their unflushed RTSS edits.
        return clear_for_other_provider();
      }
    }

    // A successfully restored snapshot can restore an originally active RTSS
    // limit. Before another provider is allowed, prove that the live RTSS
    // process is absent or that its limiter is synchronously confirmed disabled.
    // The profile file is not sufficient proof while RTSS is running because
    // RTSS may still have an older positive limit cached in memory.
    const auto process = probe_rtss_process_id();
    if (!process.known) {
      BOOST_LOG(warning) << "RTSS recovery audit could not determine whether RTSS is running.";
      return retain_exclusive();
    }
    if (!process.pid) {
      return clear_for_other_provider();
    }

    const auto root = resolve_rtss_root();
    const bool hooks_already_loaded = hooks_available();
    if (!load_hooks(root)) {
      BOOST_LOG(warning) << "RTSS recovery audit could not load hooks to verify limiter flags.";
      return retain_exclusive();
    }
    const bool hooks_loaded_for_probe = !hooks_already_loaded;
    auto unload_probe_hooks = [&]() {
      if (hooks_loaded_for_probe &&
          g_hooks.module &&
          g_hook_call_state->active_calls.load(std::memory_order_acquire) == 0) {
        FreeLibrary(g_hooks.module);
        g_hooks = {};
      }
    };

    const auto initial_flags = get_hook_flags();
    if (!initial_flags) {
      unload_probe_hooks();
      BOOST_LOG(warning) << "RTSS recovery audit could not verify limiter flags.";
      return retain_exclusive();
    }
    if (*initial_flags & k_rtss_flag_limiter_disabled) {
      unload_probe_hooks();
      return clear_for_other_provider();
    }

    bool reload_scheduled = false;
    if (!reload_profiles_from_disk(true, &reload_scheduled)) {
      unload_probe_hooks();
      BOOST_LOG(warning) << (reload_scheduled ?
                               "RTSS recovery audit did not receive acknowledgement for the profile reload." :
                               "RTSS recovery audit could not schedule the profile reload.");
      return retain_exclusive();
    }

    const auto flags = get_hook_flags();
    const auto limit = read_profile_value_int(root, k_rtss_limit_profile_key);
    unload_probe_hooks();

    if (!flags) {
      BOOST_LOG(warning) << "RTSS recovery audit could not re-verify limiter flags after reload.";
      return retain_exclusive();
    }
    if (*flags & k_rtss_flag_limiter_disabled) {
      return clear_for_other_provider();
    }
    if (!limit.known) {
      BOOST_LOG(warning) << "RTSS recovery audit could not read the live Global profile limit.";
      return retain_exclusive();
    }
    if (limit.value && *limit.value < 0) {
      BOOST_LOG(warning) << "RTSS recovery audit found an invalid negative Global profile limit.";
      return retain_exclusive();
    }

    // A determined absence -- no Global profile, no [Framerate] section, or no Limit key --
    // means RTSS definitively has no cap, which is as safe to hand off as an explicit zero.
    // rtss_streaming_start already reads the same absent value as safe to fall back from.
    if (!limit.value || *limit.value == 0) {
      return clear_for_other_provider();
    }

    BOOST_LOG(warning) << "RTSS recovery audit found a positive live Global profile limit with its limiter enabled.";
    return retain_exclusive();
  }

  void rtss_restore_pending_overrides() {
    (void) rtss_audit_pending_recovery();
  }

  void rtss_set_sync_limiter_override(std::optional<std::string> value) {
    std::scoped_lock lock {g_rtss_lifecycle_mutex};
    if (value && value->empty()) {
      g_sync_limiter_override.reset();
    } else {
      g_sync_limiter_override = std::move(value);
    }
  }

  std::optional<std::string> rtss_get_sync_limiter_override() {
    std::scoped_lock lock {g_rtss_lifecycle_mutex};
    return g_sync_limiter_override;
  }

  bool rtss_warmup_process() {
    std::scoped_lock lock {g_rtss_lifecycle_mutex};
    g_rtss_root = resolve_rtss_root();
    if (!fs::exists(g_rtss_root)) {
      BOOST_LOG(warning) << "RTSS install path not found: "sv << g_rtss_root.string();
      return false;
    }
    return ensure_rtss_running(g_rtss_root);
  }

  rtss_apply_result rtss_streaming_start(int numerator, int denominator) {
    std::scoped_lock lock {g_rtss_lifecycle_mutex};
    if (g_hook_call_state->active_calls.load(std::memory_order_acquire) == 0) {
      g_hooks_failed = false;
    }
    g_limit_active = false;
    g_last_apply_result = rtss_apply_result::safe_to_fallback;
    const auto recovery = maybe_restore_from_overrides_file();
    if (recovery == recovery_result_t::unresolved) {
      // Keep the previous stream's modification flags: they still describe live
      // state that a later stop has to undo.
      return retain_rtss_exclusive(
        "RTSS has unresolved persisted overrides from an earlier stream.");
    }

    g_settings_dirty = false;
    g_flags_modified = false;
    g_denominator_modified = false;
    g_limit_modified = false;
    g_sync_limiter_modified = false;

    if (recovery == recovery_result_t::rtss_absent) {
      // The provider loop deliberately skips the availability check for RTSS, so
      // an absent RTSS reaches this point. Retaining exclusivity here would
      // force-disable the driver limiter and leave the stream with no cap at all.
      BOOST_LOG(warning) << "RTSS has persisted overrides from an earlier stream but is no longer present; falling back to another limiter."sv;
      return mark_safe_to_fallback();
    }

    if (!config::frame_limiter.enable || numerator <= 0 || denominator <= 0) {
      return mark_safe_to_fallback();
    }

    g_rtss_root = resolve_rtss_root();
    if (!fs::exists(g_rtss_root)) {
      BOOST_LOG(warning) << "RTSS install path not found: "sv << g_rtss_root.string();
      return mark_safe_to_fallback();
    }
    if (!ensure_rtss_running(g_rtss_root)) {
      BOOST_LOG(warning) << "RTSS is not running; refusing to report a file-only frame limit as active."sv;
      return mark_safe_to_fallback();
    }

    const std::optional<int> requested_limit {numerator};
    const std::optional<int> requested_denominator {denominator};
    const auto original_limit = read_profile_value_int(g_rtss_root, k_rtss_limit_profile_key);
    const auto original_denominator = read_profile_value_int(g_rtss_root, k_rtss_denominator_profile_key);
    const auto original_sync_limiter = read_profile_value_int(g_rtss_root, k_rtss_sync_limiter_profile_key);
    if (!original_limit.known || !original_denominator.known || !original_sync_limiter.known) {
      return retain_rtss_exclusive(
        "One or more RTSS Global profile values could not be read reliably.");
    }
    g_original_limit = original_limit.value;
    g_original_denominator = original_denominator.value;
    g_original_sync_limiter = original_sync_limiter.value;
    if (!load_hooks(g_rtss_root)) {
      BOOST_LOG(warning) << "RTSSHooks could not be loaded; limiter state cannot be verified."sv;
      if (g_original_limit && *g_original_limit > 0) {
        return retain_rtss_exclusive(
          "The running RTSS profile contains a positive frame limit whose enabled state is unknown.");
      }
      return mark_safe_to_fallback();
    }

    g_original_flags = get_hook_flags();
    if (!g_original_flags) {
      BOOST_LOG(warning) << "RTSS limiter flags could not be read; refusing to apply an unverified profile-only limit."sv;
      if (g_original_limit && *g_original_limit > 0) {
        return retain_rtss_exclusive(
          "The running RTSS profile contains a positive frame limit whose enabled state could not be read.");
      }
      return mark_safe_to_fallback();
    }

    std::optional<int> sync_limiter_value;
    std::optional<std::string> sync_limiter_label;
    if (g_sync_limiter_override && !g_sync_limiter_override->empty()) {
      sync_limiter_value = map_sync_limiter(*g_sync_limiter_override);
      if (sync_limiter_value) {
        sync_limiter_label = *g_sync_limiter_override;
      } else {
        BOOST_LOG(warning) << "RTSS SyncLimiter override ignored; unknown mode: "sv << *g_sync_limiter_override;
      }
    }
    if (!sync_limiter_value) {
      sync_limiter_value = map_sync_limiter(config::rtss.frame_limit_type);
      if (sync_limiter_value && !config::rtss.frame_limit_type.empty()) {
        sync_limiter_label = config::rtss.frame_limit_type;
      }
    }

    g_flags_modified = g_original_flags && (*g_original_flags & k_rtss_flag_limiter_disabled);
    g_limit_modified = g_original_limit != requested_limit;
    g_denominator_modified = g_original_denominator != requested_denominator;
    g_sync_limiter_modified = sync_limiter_value && g_original_sync_limiter != sync_limiter_value;
    g_settings_dirty = g_flags_modified || g_limit_modified || g_denominator_modified || g_sync_limiter_modified;
    if (g_settings_dirty) {
      recovery_snapshot_t snapshot;
      snapshot.flags_modified = g_flags_modified;
      snapshot.original_flags = g_original_flags;
      snapshot.denominator_modified = g_denominator_modified;
      snapshot.original_denominator = g_original_denominator;
      snapshot.limit_modified = g_limit_modified;
      snapshot.original_limit = g_original_limit;
      snapshot.sync_limiter_modified = g_sync_limiter_modified;
      snapshot.original_sync_limiter = g_original_sync_limiter;
      // Do not alter RTSS until the original pair is recoverable after a
      // crash. Numerator and denominator must never be restored separately.
      g_recovery_file_owned = write_overrides_file(snapshot);
      if (!g_recovery_file_owned) {
        BOOST_LOG(error) << "RTSS overrides: refusing to apply changes without a durable recovery snapshot.";
        const bool existing_limit_may_be_active =
          !g_flags_modified && g_original_limit && *g_original_limit > 0;
        g_settings_dirty = false;
        g_flags_modified = false;
        g_denominator_modified = false;
        g_limit_modified = false;
        g_sync_limiter_modified = false;
        if (existing_limit_may_be_active) {
          return retain_rtss_exclusive(
            "RTSS was already limiting, but its original settings could not be persisted for recovery.");
        }
        return mark_safe_to_fallback();
      }
    } else {
      g_recovery_file_owned = false;
    }

    const auto *limit_to_write = g_limit_modified ? &requested_limit : nullptr;
    const auto *denominator_to_write = g_denominator_modified ? &requested_denominator : nullptr;
    const auto *sync_to_write = g_sync_limiter_modified ? &sync_limiter_value : nullptr;

    // Publish the requested rate before enabling the limiter flag. When the
    // flag must change, its hook call also reloads the profile so a timed-out
    // call that completes later activates the intended pair, not the old one.
    if (limit_to_write || denominator_to_write || sync_to_write) {
      if (!write_framerate_values(g_rtss_root, limit_to_write, denominator_to_write, sync_to_write)) {
        if (g_flags_modified) {
          // The original flags still confirm the RTSS limiter is disabled;
          // profile uncertainty alone cannot create a second active limiter.
          BOOST_LOG(warning) << "RTSS Global profile could not be updated while its limiter remained disabled.";
          return mark_safe_to_fallback();
        }
        return retain_rtss_exclusive(
          "RTSS Global profile update failed after mutation began.");
      }
    }

    if (g_flags_modified) {
      constexpr DWORD limiter_mask = k_rtss_flag_limiter_disabled;
      bool flag_call_scheduled = false;
      const bool reload_with_flag_update = limit_to_write || denominator_to_write || sync_to_write;
      const auto updated_flags =
        set_hook_flags(~limiter_mask, 0, &flag_call_scheduled, reload_with_flag_update);
      if (!updated_flags) {
        if (!flag_call_scheduled) {
          BOOST_LOG(warning) << "RTSS limiter enable was not scheduled; allowing another provider.";
          return mark_safe_to_fallback();
        }
        return retain_rtss_exclusive(
          "RTSS limiter enable could not be confirmed after mutation began.");
      }
      if (*updated_flags & limiter_mask) {
        BOOST_LOG(warning) << "RTSS confirmed that its limiter remained disabled; allowing another provider.";
        return mark_safe_to_fallback();
      }
    }

    if ((limit_to_write || denominator_to_write || sync_to_write) && !g_flags_modified) {
      bool reload_scheduled = false;
      if (!reload_profiles_from_disk(false, &reload_scheduled)) {
        return retain_rtss_exclusive(
          reload_scheduled ?
            "RTSS did not acknowledge the requested frame-limit profile reload." :
            "The requested RTSS frame-limit profile reload was not scheduled.");
      }
    }

    BOOST_LOG(info) << "RTSS applied framerate limit=" << (static_cast<double>(numerator) / denominator)
                    << " Hz (raw=" << numerator << ", denominator=" << denominator << ")";
    if (sync_limiter_label) {
      BOOST_LOG(info) << "RTSS SyncLimiter applied (" << *sync_limiter_label << ')';
    }
    return mark_applied();
  }

  rtss_apply_result rtss_streaming_refresh(int numerator, int denominator) {
    std::scoped_lock lock {g_rtss_lifecycle_mutex};
    if (!config::frame_limiter.enable || numerator <= 0 || denominator <= 0) {
      if (g_limit_active || g_settings_dirty || g_recovery_file_owned) {
        return retain_rtss_exclusive(
          "The RTSS refresh request was invalid while RTSS-owned state remained active.");
      }
      return mark_safe_to_fallback();
    }
    if (g_last_apply_result == rtss_apply_result::retained_exclusive) {
      return retain_rtss_exclusive(
        "The requested RTSS limit remains unconfirmed after an earlier operation.");
    }
    if (!g_limit_active && !g_settings_dirty) {
      return rtss_streaming_start(numerator, denominator);
    }

    g_rtss_root = resolve_rtss_root();
    if (!fs::exists(g_rtss_root)) {
      return retain_rtss_exclusive(
        "The RTSS install path disappeared while RTSS-owned settings still require cleanup.");
    }
    if (!ensure_rtss_running(g_rtss_root)) {
      return retain_rtss_exclusive(
        "RTSS stopped before the requested frame-limit refresh could be applied.");
    }
    const bool hooks_loaded = load_hooks(g_rtss_root);

    const std::optional<int> requested_limit {numerator};
    const std::optional<int> requested_denominator {denominator};
    const auto current_limit_read = read_profile_value_int(g_rtss_root, k_rtss_limit_profile_key);
    const auto current_denominator_read = read_profile_value_int(g_rtss_root, k_rtss_denominator_profile_key);
    if (!current_limit_read.known || !current_denominator_read.known) {
      return retain_rtss_exclusive(
        "The current RTSS frame-limit profile could not be read reliably during refresh.");
    }
    if (!hooks_loaded) {
      return retain_rtss_exclusive(
        "RTSSHooks could not be loaded, so limiter flags could not be verified during refresh.");
    }

    const auto current_flags = get_hook_flags();
    if (!current_flags) {
      return retain_rtss_exclusive(
        "RTSS limiter flags could not be verified during refresh.");
    }

    const auto &current_limit = current_limit_read.value;
    const auto &current_denominator = current_denominator_read.value;
    const bool write_limit = current_limit != requested_limit;
    const bool write_denominator = current_denominator != requested_denominator;
    constexpr DWORD limiter_mask = k_rtss_flag_limiter_disabled;
    const bool limiter_disabled = (*current_flags & limiter_mask) != 0;
    if (!write_limit && !write_denominator && !limiter_disabled) {
      return mark_applied();
    }

    // If another program changed a profile key that this stream did not own
    // yet, refresh is about to take ownership of it. Capture that *current*
    // value and persist the expanded recovery snapshot before overwriting the
    // raw numerator/denominator pair.
    const bool acquiring_limit = write_limit && !g_limit_modified;
    const bool acquiring_denominator = write_denominator && !g_denominator_modified;
    const auto next_original_limit = acquiring_limit ? current_limit : g_original_limit;
    const auto next_original_denominator = acquiring_denominator ? current_denominator : g_original_denominator;
    const bool next_limit_modified = g_limit_modified || write_limit;
    const bool next_denominator_modified = g_denominator_modified || write_denominator;
    const bool acquiring_flags = limiter_disabled && !g_flags_modified;
    const auto next_original_flags = acquiring_flags ? current_flags : g_original_flags;
    const bool next_flags_modified = g_flags_modified || limiter_disabled;
    const bool needs_snapshot =
      !g_settings_dirty ||
      !g_recovery_file_owned ||
      acquiring_limit ||
      acquiring_denominator ||
      acquiring_flags;
    if (needs_snapshot) {
      recovery_snapshot_t snapshot;
      snapshot.flags_modified = next_flags_modified;
      snapshot.original_flags = next_original_flags;
      snapshot.limit_modified = next_limit_modified;
      snapshot.original_limit = next_original_limit;
      snapshot.denominator_modified = next_denominator_modified;
      snapshot.original_denominator = next_original_denominator;
      snapshot.sync_limiter_modified = g_sync_limiter_modified;
      snapshot.original_sync_limiter = g_original_sync_limiter;
      if (!write_overrides_file(snapshot)) {
        BOOST_LOG(error) << "RTSS overrides: refusing to mutate the active RTSS provider without a durable recovery snapshot; retaining its current limit.";
        if (limiter_disabled) {
          return mark_safe_to_fallback();
        }
        return retain_rtss_exclusive(
          "The requested RTSS refresh was not applied because its recovery snapshot could not be persisted.");
      }
      g_recovery_file_owned = true;
    }

    g_original_limit = next_original_limit;
    g_original_denominator = next_original_denominator;
    g_limit_modified = next_limit_modified;
    g_denominator_modified = next_denominator_modified;
    g_original_flags = next_original_flags;
    g_flags_modified = next_flags_modified;
    g_settings_dirty =
      g_flags_modified ||
      g_limit_modified ||
      g_denominator_modified ||
      g_sync_limiter_modified;

    if (write_limit || write_denominator) {
      if (!write_framerate_values(
            g_rtss_root,
            write_limit ? &requested_limit : nullptr,
            write_denominator ? &requested_denominator : nullptr,
            nullptr
          )) {
        if (limiter_disabled) {
          return mark_safe_to_fallback();
        }
        return retain_rtss_exclusive(
          "The requested RTSS refresh could not update the Global profile.");
      }
    }

    if (limiter_disabled) {
      bool flag_call_scheduled = false;
      const bool reload_with_flag_update = write_limit || write_denominator;
      const auto updated_flags =
        set_hook_flags(~limiter_mask, 0, &flag_call_scheduled, reload_with_flag_update);
      if (!updated_flags) {
        if (!flag_call_scheduled) {
          return mark_safe_to_fallback();
        }
        return retain_rtss_exclusive(
          "RTSS limiter enable could not be confirmed during refresh.");
      }
      if (*updated_flags & limiter_mask) {
        return mark_safe_to_fallback();
      }
    } else if (write_limit || write_denominator) {
      bool reload_scheduled = false;
      if (!reload_profiles_from_disk(false, &reload_scheduled)) {
        return retain_rtss_exclusive(
          reload_scheduled ?
            "RTSS did not acknowledge the requested frame-limit refresh." :
            "The requested RTSS frame-limit refresh was not scheduled.");
      }
    }

    BOOST_LOG(info) << "RTSS refreshed framerate limit=" << (static_cast<double>(numerator) / denominator)
                    << " Hz (raw=" << numerator << ", denominator=" << denominator << ")";
    return mark_applied();
  }

  bool rtss_hooks_stalled() {
    std::scoped_lock lock {g_rtss_lifecycle_mutex};
    return g_hooks_failed;
  }

  void rtss_streaming_stop(bool keep_process_running) {
    std::scoped_lock lock {g_rtss_lifecycle_mutex};
    g_sync_limiter_override.reset();
    auto cleanup = [&]() {
      g_original_limit.reset();
      g_original_sync_limiter.reset();
      g_original_denominator.reset();
      g_original_flags.reset();
      g_limit_active = false;
      g_last_apply_result = rtss_apply_result::safe_to_fallback;
      g_settings_dirty = false;
      g_flags_modified = false;
      g_denominator_modified = false;
      g_limit_modified = false;
      g_sync_limiter_modified = false;
      if (g_hooks.module && g_hook_call_state->active_calls.load(std::memory_order_acquire) == 0) {
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

    if (g_hook_call_state->active_calls.load(std::memory_order_acquire) != 0) {
      // A timed-out SetFlags/reload worker may still complete. Do not race it
      // with an in-memory restore and then delete the only durable snapshot.
      // Hand recovery to the next audit after the late call has exited.
      BOOST_LOG(warning) << "RTSS overrides: deferring stream cleanup while a late hooks operation is still active.";
      g_recovery_file_owned = false;
      cleanup();
      return;
    }

    bool restore_success = true;

    // Limit and LimitDenominator represent one rate. Restore every changed
    // [Framerate] key in one file replacement and reload once, so a reader
    // never observes the old numerator with the restored denominator (or the
    // reverse). A null original value removes the key rather than inventing a
    // zero that was not present before the stream.
    const auto *limit_to_restore = g_limit_modified ? &g_original_limit : nullptr;
    const auto *denominator_to_restore = g_denominator_modified ? &g_original_denominator : nullptr;
    const auto *sync_to_restore = g_sync_limiter_modified ? &g_original_sync_limiter : nullptr;
    if (limit_to_restore || denominator_to_restore || sync_to_restore) {
      if (!write_framerate_values(g_rtss_root, limit_to_restore, denominator_to_restore, sync_to_restore) ||
          !reload_profiles_from_disk()) {
        BOOST_LOG(warning) << "RTSS did not acknowledge the atomic frame-limit restore.";
        restore_success = false;
      } else {
        BOOST_LOG(info) << "RTSS restored frame-limit profile values atomically.";
      }
    }

    if (g_flags_modified && g_original_flags.has_value() && hooks_available()) {
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
    std::scoped_lock lock {g_rtss_lifecycle_mutex};
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
