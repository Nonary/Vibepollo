/**
 * @file src/confighttp_rtss.cpp
 * @brief RTSS-specific HTTP endpoints (Windows-only).
 */

#ifdef _WIN32

  // standard includes
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>
#include <cwctype>
#include <algorithm>
#include <array>
#include <optional>

  // third-party includes
  #include <nlohmann/json.hpp>
  #include <Simple-Web-Server/server_https.hpp>

  // local includes
  #include "confighttp.h"
  #include "src/config.h"
  #include "src/logging.h"
  #include "src/platform/windows/frame_limiter.h"
  #include "src/platform/windows/ipc/misc_utils.h"
  #include "src/platform/windows/misc.h"

namespace confighttp {

  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;

  // Forward declarations for helpers defined in confighttp.cpp
  bool authenticate(resp_https_t response, req_https_t request);
  void print_req(const req_https_t &request);
  void send_response(resp_https_t response, const nlohmann::json &output_tree);

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

    std::vector<std::filesystem::path> discover_lossless_candidates(const std::optional<std::filesystem::path> &configured, const std::optional<std::filesystem::path> &override_candidate, const std::optional<std::filesystem::path> &default_path) {
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

  }  // namespace

  void getRtssStatus(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);

    nlohmann::json out;
    auto rtss = platf::rtss_get_status();
    auto fl = platf::frame_limiter_get_status();

    out["enabled"] = fl.enabled;
    out["configured_provider"] = platf::frame_limiter_provider_to_string(fl.configured_provider);
    out["active_provider"] = platf::frame_limiter_provider_to_string(fl.active_provider);
    out["nvidia_available"] = fl.nvidia_available;
    out["nvcp_ready"] = fl.nvcp_ready;
    out["rtss_available"] = fl.rtss_available;
    out["disable_vsync"] = fl.disable_vsync;
    out["disable_vsync_ullm"] = fl.disable_vsync;  // legacy key for older clients
    out["nv_overrides_supported"] = fl.nv_overrides_supported;
    out["configured_path"] = rtss.configured_path;
    out["path_configured"] = rtss.path_configured;
    out["resolved_path"] = rtss.resolved_path;
    out["path_exists"] = rtss.path_exists;
    out["hooks_found"] = rtss.hooks_found;
    out["profile_found"] = rtss.profile_found;
    out["can_bootstrap_profile"] = rtss.can_bootstrap_profile;
    out["process_running"] = rtss.process_running;

    // A user-friendly message hinting required action
    std::string message;
    auto provider_to_string = [](platf::frame_limiter_provider provider) -> const char * {
      switch (provider) {
        case platf::frame_limiter_provider::nvidia_control_panel:
          return "nvidia-control-panel";
        case platf::frame_limiter_provider::rtss:
          return "rtss";
        case platf::frame_limiter_provider::auto_detect:
          return "auto";
        case platf::frame_limiter_provider::none:
        default:
          return "none";
      }
    };

    auto describe_provider = [&](const std::string &id) -> std::string {
      if (id == "nvidia-control-panel") {
        return "NVIDIA Control Panel";
      }
      if (id == "rtss") {
        return "RTSS";
      }
      if (id == "auto") {
        return "Auto";
      }
      return "None";
    };

    const auto add_segment = [](std::string &dest, const std::string &segment) {
      if (segment.empty()) {
        return;
      }
      if (!dest.empty()) {
        dest.push_back(' ');
      }
      dest += segment;
    };

    const std::string configured_id = provider_to_string(fl.configured_provider);
    const bool prefer_rtss = fl.configured_provider == platf::frame_limiter_provider::rtss || fl.configured_provider == platf::frame_limiter_provider::auto_detect;
    const bool rtss_ready = rtss.path_exists && rtss.hooks_found;
    const bool rtss_bootstrap_pending = rtss_ready && !rtss.profile_found && rtss.can_bootstrap_profile;

    std::string provider_message;
    if (fl.enabled) {
      if (fl.active_provider == platf::frame_limiter_provider::nvidia_control_panel) {
        add_segment(provider_message, "NVIDIA Control Panel frame limiter active (not recommended; it cannot guarantee perfect frame pacing).");
      } else if (fl.active_provider == platf::frame_limiter_provider::rtss) {
        add_segment(provider_message, "RTSS frame limiter active for this stream.");
      } else {
        if (fl.configured_provider == platf::frame_limiter_provider::nvidia_control_panel) {
          if (!fl.nvidia_available) {
            add_segment(provider_message, "No NVIDIA GPU detected. Switch to RTSS or install NVIDIA drivers.");
          } else if (!fl.nvcp_ready) {
            add_segment(provider_message, "NVIDIA Control Panel integration unavailable (NvAPI not ready).");
          } else {
            add_segment(provider_message, "NVIDIA Control Panel limiter selected (not recommended). Sunshine recommends RTSS for smoother pacing.");
          }
        } else if (prefer_rtss) {
          if (!rtss.path_exists) {
            add_segment(provider_message, "RTSS not found at the resolved path. Install RTSS for the smoothest streaming experience.");
          } else if (!rtss.hooks_found) {
            add_segment(provider_message, "RTSSHooks DLL not found. Reinstall RTSS to restore frame limiter support.");
          } else {
            add_segment(provider_message, std::string("Frame limiter configured for ") + describe_provider(configured_id) + "; awaiting next stream.");
            if (!rtss.process_running) {
              add_segment(provider_message, "Sunshine will launch RTSS automatically when streaming starts.");
            }
            if (rtss_bootstrap_pending) {
              add_segment(provider_message, "Sunshine will refresh RTSS configuration automatically on the next stream.");
            }
          }
        } else {
          add_segment(provider_message, "Frame limiter enabled but no provider applied.");
        }
      }
    } else {
      add_segment(provider_message, "Frame limiter disabled; enable in settings to activate.");
    }

    if (prefer_rtss) {
      add_segment(provider_message, "RTSS provides the smoothest pacing; NVIDIA's limiter is not recommended because it cannot guarantee perfect frame pacing.");
    } else if (fl.configured_provider == platf::frame_limiter_provider::nvidia_control_panel) {
      add_segment(provider_message, "Sunshine recommends installing RTSS for the smoothest streaming experience; NVIDIA's limiter is not recommended because it cannot guarantee perfect frame pacing.");
    }

    std::string override_message;
    if (fl.disable_vsync) {
      if (fl.nv_overrides_supported) {
        override_message = "NVIDIA overrides ready: Sunshine will force VSYNC off during streams.";
      } else if (fl.nvidia_available && !fl.nvcp_ready) {
        override_message = "NvAPI unavailable; Sunshine will fall back to forcing the highest available refresh rate during streams.";
      } else if (!fl.nvidia_available) {
        override_message = "No NVIDIA GPU detected; Sunshine will force the highest available refresh rate during streams as a best-effort VSYNC workaround.";
      }
    }

    if (!override_message.empty() && !provider_message.empty()) {
      message = provider_message + " " + override_message;
    } else if (!override_message.empty()) {
      message = override_message;
    } else {
      message = provider_message;
    }
    out["message"] = message;

    send_response(response, out);
  }

  void getLosslessScalingStatus(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);

    auto query = request->parse_query_string();
    std::string override_path;
    if (auto it = query.find("path"); it != query.end()) {
      override_path = it->second;
    }

    const std::string configured_utf8 = config::lossless_scaling.exe_path;
    const std::string default_hint = "C:\\\\Program Files (x86)\\\\Steam\\\\steamapps\\\\common\\\\Lossless Scaling\\\\LosslessScaling.exe";

    auto to_path = [](const std::string &utf8) -> std::optional<std::filesystem::path> {
      if (utf8.empty()) {
        return std::nullopt;
      }
      try {
        std::wstring wide = platf::dxgi::utf8_to_wide(utf8);
        if (wide.empty()) {
          return std::nullopt;
        }
        return std::filesystem::path(wide);
      } catch (...) {
        return std::nullopt;
      }
    };

    auto path_to_utf8 = [](const std::filesystem::path &path) -> std::string {
      try {
        return platf::dxgi::wide_to_utf8(path.wstring());
      } catch (...) {
        return std::string();
      }
    };

    const std::string check_utf8 = !override_path.empty() ? override_path : configured_utf8;
    const auto configured_path = to_path(configured_utf8);
    const auto checked_path = to_path(check_utf8);
    const auto default_path = to_path(default_hint);

    auto resolved_configured = configured_path ? resolve_lossless_candidate(*configured_path) : std::optional<std::filesystem::path>();
    auto resolved_checked = checked_path ? resolve_lossless_candidate(*checked_path) : std::optional<std::filesystem::path>();
    auto resolved_default = default_path ? resolve_lossless_candidate(*default_path) : std::optional<std::filesystem::path>();

    std::error_code configured_ec;
    const bool configured_is_directory = configured_path && std::filesystem::is_directory(*configured_path, configured_ec);
    std::error_code checked_ec;
    const bool checked_is_directory = checked_path && std::filesystem::is_directory(*checked_path, checked_ec);
    std::error_code default_ec;
    const bool default_is_directory = default_path && std::filesystem::is_directory(*default_path, default_ec);

    nlohmann::json out;
    out["configured_path"] = configured_utf8;
    out["checked_path"] = check_utf8;
    out["configured_exists"] = resolved_configured.has_value();
    out["checked_exists"] = resolved_checked.has_value();
    out["configured_is_directory"] = configured_is_directory;
    out["checked_is_directory"] = checked_is_directory;
    out["default_path"] = default_hint;
    out["default_exists"] = resolved_default.has_value() || (default_path && std::filesystem::exists(*default_path));
    out["default_is_directory"] = default_is_directory;

    std::string suggested_utf8 = configured_utf8;
    if (!suggested_utf8.empty()) {
      if (resolved_configured) {
        suggested_utf8 = path_to_utf8(*resolved_configured);
      }
    } else if (resolved_default) {
      suggested_utf8 = path_to_utf8(*resolved_default);
    } else {
      suggested_utf8 = default_hint;
    }
    out["suggested_path"] = suggested_utf8;
    if (resolved_checked) {
      out["resolved_path"] = path_to_utf8(*resolved_checked);
    }

    auto candidates = discover_lossless_candidates(configured_path, checked_path, default_path);
    if (!candidates.empty()) {
      nlohmann::json arr = nlohmann::json::array();
      for (const auto &candidate : candidates) {
        arr.emplace_back(path_to_utf8(candidate));
      }
      out["candidates"] = arr;
    }

    std::string message;
    if (!resolved_checked) {
      if (!check_utf8.empty()) {
        if (checked_is_directory) {
          message = "Lossless Scaling executable not found in the selected folder. Select LosslessScaling.exe directly.";
        } else {
          message = "Lossless Scaling executable not found at the specified path.";
        }
      } else {
        message = "Lossless Scaling executable not configured.";
      }
      if (resolved_default) {
        message += " Detected installation at \"" + path_to_utf8(*resolved_default) + "\".";
      } else {
        message += " Please locate LosslessScaling.exe manually.";
      }
    } else {
      message = "Lossless Scaling executable detected.";
    }
    out["message"] = message;

    send_response(response, out);
  }

}  // namespace confighttp

#endif  // _WIN32
