/**
 * @file src/platform/windows/rtss_integration.cpp
 * @brief Apply/restore RTSS frame limit and related properties on stream start/stop.
 */

#ifdef _WIN32

  // standard includes
  #include <array>
  #include <atomic>
  #include <chrono>
  #include <cwchar>
  #include <filesystem>
  #include <fstream>
  #include <future>
  #include <limits>
  #include <memory>
  #include <nlohmann/json.hpp>
  #include <optional>
  #include <mutex>
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
    using fn_UpdateProfiles = VOID(__cdecl *)();
    using fn_GetFlags = DWORD(__cdecl *)();
    using fn_SetFlags = DWORD(__cdecl *)(DWORD, DWORD);

    struct hooks_t {
      HMODULE module = nullptr;
      fn_LoadProfile LoadProfile = nullptr;
      fn_UpdateProfiles UpdateProfiles = nullptr;
      fn_GetFlags GetFlags = nullptr;
      fn_SetFlags SetFlags = nullptr;

      explicit operator bool() const {
        return module && LoadProfile && UpdateProfiles && GetFlags && SetFlags;
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
    constexpr DWORD k_rtss_flag_limiter_disabled = 4;
    constexpr char k_rtss_limit_profile_key[] = "Limit";
    constexpr char k_rtss_denominator_profile_key[] = "LimitDenominator";
    constexpr char k_rtss_sync_limiter_profile_key[] = "SyncLimiter";
    constexpr char k_rtss_framerate_section[] = "[Framerate]";

    const std::array<const wchar_t *, 2> k_rtss_process_names = {L"RTSS.exe", L"RTSS64.exe"};
    const std::array<const wchar_t *, 2> k_rtss_executable_names = {L"RTSS.exe", L"RTSS64.exe"};

    const fs::path profile_path(const fs::path &root) {
      return root / "Profiles" / "Global";
    }

    bool load_hooks(const fs::path &root);
    bool hooks_available();
    std::optional<DWORD> get_hook_flags();
    std::optional<DWORD> set_hook_flags(DWORD and_mask, DWORD xor_mask);
    bool write_framerate_values(
      const fs::path &root,
      const std::optional<int> *limit,
      const std::optional<int> *denominator,
      const std::optional<int> *sync_limiter
    );
    bool reload_profiles_from_disk();
    fs::path resolve_rtss_root();

    template<typename Result, typename Callable>
    std::optional<Result> call_rtss_hooks(const char *operation, Callable &&callable) {
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
      } catch (const std::exception &ex) {
        call_state->active_calls.fetch_sub(1, std::memory_order_acq_rel);
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
      return static_cast<bool>(g_hooks) && !g_hooks_failed && g_hook_call_state->active_calls.load(std::memory_order_acquire) == 0;
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

    bool write_framerate_values(
      const fs::path &root,
      const std::optional<int> *limit,
      const std::optional<int> *denominator,
      const std::optional<int> *sync_limiter
    ) {
      try {
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

    bool reload_profiles_from_disk() {
      if (!hooks_available()) {
        return false;
      }
      const auto load_profile = g_hooks.LoadProfile;
      const auto update = g_hooks.UpdateProfiles;
      return call_rtss_hooks<bool>("LoadProfile/UpdateProfiles", [load_profile, update]() {
               load_profile("");
               update();
               return true;
             }).value_or(false);
    }

    std::optional<int> read_profile_value_int(const fs::path &root, const char *key) {
      const auto path = profile_path(root);
      if (!fs::exists(path)) {
        return std::nullopt;
      }
      try {
        std::ifstream in(path, std::ios::in | std::ios::binary);
        if (!in) {
          return std::nullopt;
        }
        std::string content {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        const auto pos = find_framerate_key(content, find_framerate_section_body(content), key);
        return pos == std::string::npos ? std::nullopt : parse_profile_value(content, pos);
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Failed reading RTSS profile value '"sv << key << "': "sv << e.what();
        return std::nullopt;
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

  void rtss_restore_pending_overrides() {
    std::scoped_lock lock {g_rtss_lifecycle_mutex};
    maybe_restore_from_overrides_file();
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

  bool rtss_streaming_start(int numerator, int denominator) {
    std::scoped_lock lock {g_rtss_lifecycle_mutex};
    if (g_hook_call_state->active_calls.load(std::memory_order_acquire) == 0) {
      g_hooks_failed = false;
    }
    g_limit_active = false;
    g_settings_dirty = false;
    g_flags_modified = false;
    g_denominator_modified = false;
    g_limit_modified = false;
    g_sync_limiter_modified = false;
    maybe_restore_from_overrides_file();

    if (!config::frame_limiter.enable || numerator <= 0 || denominator <= 0) {
      return false;
    }

    g_rtss_root = resolve_rtss_root();
    if (!fs::exists(g_rtss_root)) {
      BOOST_LOG(warning) << "RTSS install path not found: "sv << g_rtss_root.string();
      return false;
    }
    ensure_rtss_running(g_rtss_root);
    if (!load_hooks(g_rtss_root)) {
      BOOST_LOG(warning) << "RTSSHooks could not be loaded; exact frame limits cannot be acknowledged.";
      return false;
    }

    const std::optional<int> requested_limit {numerator};
    const std::optional<int> requested_denominator {denominator};
    g_original_limit = read_profile_value_int(g_rtss_root, k_rtss_limit_profile_key);
    g_original_denominator = read_profile_value_int(g_rtss_root, k_rtss_denominator_profile_key);
    g_original_sync_limiter = read_profile_value_int(g_rtss_root, k_rtss_sync_limiter_profile_key);
    g_original_flags = get_hook_flags();

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
        return false;
      }
    } else {
      g_recovery_file_owned = false;
    }

    if (g_flags_modified) {
      constexpr DWORD limiter_mask = k_rtss_flag_limiter_disabled;
      const auto updated_flags = set_hook_flags(~limiter_mask, 0);
      if (!updated_flags || (*updated_flags & limiter_mask)) {
        BOOST_LOG(warning) << "Failed to enable RTSS limiter via SetFlags.";
        return false;
      }
    }

    const auto *limit_to_write = g_limit_modified ? &requested_limit : nullptr;
    const auto *denominator_to_write = g_denominator_modified ? &requested_denominator : nullptr;
    const auto *sync_to_write = g_sync_limiter_modified ? &sync_limiter_value : nullptr;
    if (limit_to_write || denominator_to_write || sync_to_write) {
      if (!write_framerate_values(g_rtss_root, limit_to_write, denominator_to_write, sync_to_write) ||
          !reload_profiles_from_disk()) {
        BOOST_LOG(warning) << "RTSS did not acknowledge the atomic frame-limit profile update.";
        return false;
      }
    }

    g_limit_active = true;
    BOOST_LOG(info) << "RTSS applied framerate limit=" << (static_cast<double>(numerator) / denominator)
                    << " Hz (raw=" << numerator << ", denominator=" << denominator << ")";
    if (sync_limiter_label) {
      BOOST_LOG(info) << "RTSS SyncLimiter applied (" << *sync_limiter_label << ')';
    }
    return !g_hooks_failed;
  }

  bool rtss_streaming_refresh(int numerator, int denominator) {
    std::scoped_lock lock {g_rtss_lifecycle_mutex};
    if (!config::frame_limiter.enable || numerator <= 0 || denominator <= 0) {
      return false;
    }
    if (!g_limit_active && !g_settings_dirty) {
      return rtss_streaming_start(numerator, denominator);
    }

    g_rtss_root = resolve_rtss_root();
    if (!fs::exists(g_rtss_root) || !load_hooks(g_rtss_root)) {
      return false;
    }

    const std::optional<int> requested_limit {numerator};
    const std::optional<int> requested_denominator {denominator};
    const auto current_limit = read_profile_value_int(g_rtss_root, k_rtss_limit_profile_key);
    const auto current_denominator = read_profile_value_int(g_rtss_root, k_rtss_denominator_profile_key);
    const bool write_limit = current_limit != requested_limit;
    const bool write_denominator = current_denominator != requested_denominator;
    if (!write_limit && !write_denominator) {
      return g_limit_active && !g_hooks_failed;
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
    const bool needs_snapshot = !g_settings_dirty || !g_recovery_file_owned || acquiring_limit || acquiring_denominator;
    if (needs_snapshot) {
      recovery_snapshot_t snapshot;
      snapshot.flags_modified = g_flags_modified;
      snapshot.original_flags = g_original_flags;
      snapshot.limit_modified = next_limit_modified;
      snapshot.original_limit = next_original_limit;
      snapshot.denominator_modified = next_denominator_modified;
      snapshot.original_denominator = next_original_denominator;
      snapshot.sync_limiter_modified = g_sync_limiter_modified;
      snapshot.original_sync_limiter = g_original_sync_limiter;
      if (!write_overrides_file(snapshot)) {
        BOOST_LOG(error) << "RTSS overrides: refusing to refresh without a durable recovery snapshot.";
        return false;
      }
      g_recovery_file_owned = true;
    }

    g_original_limit = next_original_limit;
    g_original_denominator = next_original_denominator;
    g_limit_modified = next_limit_modified;
    g_denominator_modified = next_denominator_modified;
    g_settings_dirty = true;

    if (!write_framerate_values(
          g_rtss_root,
          write_limit ? &requested_limit : nullptr,
          write_denominator ? &requested_denominator : nullptr,
          nullptr
        ) ||
        !reload_profiles_from_disk()) {
      return false;
    }

    g_limit_active = true;
    BOOST_LOG(info) << "RTSS refreshed framerate limit=" << (static_cast<double>(numerator) / denominator)
                    << " Hz (raw=" << numerator << ", denominator=" << denominator << ")";
    return !g_hooks_failed;
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
