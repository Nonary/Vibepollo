#include "src/platform/windows/lossless_scaling_paths.h"

#ifdef _WIN32

#include <array>
#include <algorithm>
#include <cwctype>
#include <unordered_set>
#include <windows.h>

namespace lossless_paths {
  namespace {

    const std::array<std::wstring, 2> k_lossless_names {L"LosslessScaling.exe", L"Lossless Scaling.exe"};

    std::wstring lowercase_wstring(std::wstring value) {
      std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) { return std::towlower(ch); });
      return value;
    }

    bool is_lossless_executable(const std::filesystem::path &path) {
      std::error_code ec;
      if (!std::filesystem::exists(path, ec) || !std::filesystem::is_regular_file(path, ec)) {
        return false;
      }
      if (!path.has_filename()) {
        return false;
      }
      const auto filename = lowercase_wstring(path.filename().wstring());
      for (const auto &expected : k_lossless_names) {
        if (filename == lowercase_wstring(expected)) {
          return true;
        }
      }
      return false;
    }

    std::optional<std::filesystem::path> find_lossless_in_directory(const std::filesystem::path &directory) {
      std::error_code ec;
      if (!std::filesystem::exists(directory, ec) || !std::filesystem::is_directory(directory, ec)) {
        return std::nullopt;
      }
      for (const auto &name : k_lossless_names) {
        auto candidate = (directory / name).lexically_normal();
        if (is_lossless_executable(candidate)) {
          return candidate;
        }
      }
      std::filesystem::directory_options opts = std::filesystem::directory_options::skip_permission_denied;
      for (const auto &entry : std::filesystem::directory_iterator(directory, opts, ec)) {
        if (entry.is_directory(ec)) {
          for (const auto &name : k_lossless_names) {
            auto nested = (entry.path() / name).lexically_normal();
            if (is_lossless_executable(nested)) {
              return nested;
            }
          }
        }
      }
      return std::nullopt;
    }

    void append_candidate(std::vector<std::filesystem::path> &out, std::unordered_set<std::wstring> &seen, const std::optional<std::filesystem::path> &candidate) {
      if (!candidate) {
        return;
      }
      auto normalized = candidate->lexically_normal();
      auto key = lowercase_wstring(normalized.wstring());
      if (seen.insert(key).second) {
        out.push_back(normalized);
      }
    }

    void collect_lossless_candidates(std::vector<std::filesystem::path> &out, std::unordered_set<std::wstring> &seen, const std::filesystem::path &hint) {
      if (hint.empty()) {
        return;
      }
      append_candidate(out, seen, resolve_lossless_candidate(hint));
      append_candidate(out, seen, resolve_lossless_candidate(hint.parent_path()));
    }

  }  // namespace

  std::filesystem::path default_steam_lossless_path() {
    return std::filesystem::path(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Lossless Scaling\\LosslessScaling.exe");
  }

  std::optional<std::filesystem::path> resolve_lossless_candidate(const std::filesystem::path &candidate) {
    if (candidate.empty()) {
      return std::nullopt;
    }
    std::error_code ec;
    if (std::filesystem::is_regular_file(candidate, ec)) {
      if (is_lossless_executable(candidate)) {
        return candidate.lexically_normal();
      }
      return std::nullopt;
    }
    if (std::filesystem::is_directory(candidate, ec)) {
      return find_lossless_in_directory(candidate);
    }
    return std::nullopt;
  }

  std::vector<std::filesystem::path> discover_lossless_candidates(const std::optional<std::filesystem::path> &configured,
                                                                  const std::optional<std::filesystem::path> &override_candidate,
                                                                  const std::optional<std::filesystem::path> &default_path) {
    std::vector<std::filesystem::path> result;
    std::unordered_set<std::wstring> seen;

    auto add_known = [&](const std::filesystem::path &path) {
      if (path.empty()) {
        return;
      }
      collect_lossless_candidates(result, seen, path);
      std::error_code ec;
      if (std::filesystem::is_directory(path, ec)) {
        collect_lossless_candidates(result, seen, path / L"Lossless Scaling");
        collect_lossless_candidates(result, seen, path / L"Steam\\steamapps\\common\\Lossless Scaling");
      }
    };

    if (override_candidate) {
      collect_lossless_candidates(result, seen, *override_candidate);
    }
    if (configured) {
      collect_lossless_candidates(result, seen, *configured);
    }
    if (default_path) {
      collect_lossless_candidates(result, seen, *default_path);
    }

    if (default_path) {
      add_known(default_path->parent_path());
    }

    auto append_env_dir = [&](const wchar_t *env_name) {
      wchar_t buffer[MAX_PATH];
      auto len = GetEnvironmentVariableW(env_name, buffer, MAX_PATH);
      if (len == 0 || len >= MAX_PATH) {
        return;
      }
      std::wstring value(buffer, len);
      if (value.size() == 2 && value[1] == L':') {
        value.push_back(L'\\');
      }
      std::filesystem::path env_path(value);
      collect_lossless_candidates(result, seen, env_path / L"Lossless Scaling");
      collect_lossless_candidates(result, seen, env_path / L"Steam\\steamapps\\common\\Lossless Scaling");
    };

    append_env_dir(L"ProgramFiles");
    append_env_dir(L"ProgramFiles(x86)");
    append_env_dir(L"ProgramW6432");

    for (wchar_t drive = L'C'; drive <= L'Z'; ++drive) {
      std::wstring root;
      root.push_back(drive);
      root.push_back(L':');
      root.push_back(L'\\');
      collect_lossless_candidates(result, seen, std::filesystem::path(root) / L"SteamLibrary\\steamapps\\common\\Lossless Scaling");
      collect_lossless_candidates(result, seen, std::filesystem::path(root) / L"Steam\\steamapps\\common\\Lossless Scaling");
    }

    return result;
  }

}  // namespace lossless_paths

#endif  // _WIN32
