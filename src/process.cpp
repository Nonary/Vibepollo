/**
 * @file src/process.cpp
 * @brief Definitions for the startup and shutdown of the apps started by a streaming Session.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <limits>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// lib includes
#include <boost/algorithm/string.hpp>
#include <boost/crc.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <openssl/evp.h>
#include <openssl/sha.h>

// local includes
#include "config.h"
#include "crypto.h"
#include "display_helper_integration.h"
#include "logging.h"
#include "platform/common.h"
#ifdef _WIN32
  #include "config_playnite.h"
  #include "platform/windows/ipc/misc_utils.h"
  #include "platform/windows/playnite_integration.h"
  #include "tools/playnite_launcher/focus_utils.h"
  #include "tools/playnite_launcher/lossless_scaling.h"

  #include <Psapi.h>
#endif
#include "process.h"
#ifdef _WIN32
  #include "platform/windows/virtual_display.h"
#endif
#include "system_tray.h"
#include "utility.h"
#include "uuid.h"

#ifdef _WIN32
  // from_utf8() string conversion function
  #include "platform/windows/misc.h"

  // _SH constants for _wfsopen()
  #include <share.h>
#endif

#define DEFAULT_APP_IMAGE_PATH SUNSHINE_ASSETS_DIR "/box.png"

namespace proc {
  using namespace std::literals;
  namespace pt = boost::property_tree;

  namespace {
    constexpr const char *LOSSLESS_PROFILE_RECOMMENDED = "recommended";
    constexpr const char *LOSSLESS_PROFILE_CUSTOM = "custom";
    constexpr int LOSSLESS_DEFAULT_FLOW_SCALE = 50;
    constexpr int LOSSLESS_DEFAULT_RESOLUTION_SCALE = 100;
    constexpr bool LOSSLESS_DEFAULT_PERFORMANCE_MODE = true;
    constexpr int LOSSLESS_MIN_FLOW_SCALE = 0;
    constexpr int LOSSLESS_MAX_FLOW_SCALE = 100;
    constexpr int LOSSLESS_MIN_RESOLUTION_SCALE = 10;
    constexpr int LOSSLESS_MAX_RESOLUTION_SCALE = 100;
    constexpr int LOSSLESS_SHARPNESS_MIN = 1;
    constexpr int LOSSLESS_SHARPNESS_MAX = 10;

    constexpr const char *ENV_LOSSLESS_PROFILE = "SUNSHINE_LOSSLESS_SCALING_ACTIVE_PROFILE";
    constexpr const char *ENV_LOSSLESS_CAPTURE_API = "SUNSHINE_LOSSLESS_SCALING_CAPTURE_API";
    constexpr const char *ENV_LOSSLESS_QUEUE_TARGET = "SUNSHINE_LOSSLESS_SCALING_QUEUE_TARGET";
    constexpr const char *ENV_LOSSLESS_HDR = "SUNSHINE_LOSSLESS_SCALING_HDR";
    constexpr const char *ENV_LOSSLESS_FLOW_SCALE = "SUNSHINE_LOSSLESS_SCALING_FLOW_SCALE";
    constexpr const char *ENV_LOSSLESS_PERFORMANCE_MODE = "SUNSHINE_LOSSLESS_SCALING_PERFORMANCE_MODE";
    constexpr const char *ENV_LOSSLESS_RESOLUTION = "SUNSHINE_LOSSLESS_SCALING_RESOLUTION_SCALE";
    constexpr const char *ENV_LOSSLESS_FRAMEGEN_MODE = "SUNSHINE_LOSSLESS_SCALING_FRAMEGEN_MODE";
    constexpr const char *ENV_LOSSLESS_LSFG3_MODE = "SUNSHINE_LOSSLESS_SCALING_LSFG3_MODE";
    constexpr const char *ENV_LOSSLESS_SCALING_TYPE = "SUNSHINE_LOSSLESS_SCALING_SCALING_TYPE";
    constexpr const char *ENV_LOSSLESS_SHARPNESS = "SUNSHINE_LOSSLESS_SCALING_SHARPNESS";
    constexpr const char *ENV_LOSSLESS_LS1_SHARPNESS = "SUNSHINE_LOSSLESS_SCALING_LS1_SHARPNESS";
    constexpr const char *ENV_LOSSLESS_ANIME4K_TYPE = "SUNSHINE_LOSSLESS_SCALING_ANIME4K_TYPE";
    constexpr const char *ENV_LOSSLESS_ANIME4K_VRS = "SUNSHINE_LOSSLESS_SCALING_ANIME4K_VRS";

    std::string normalize_frame_generation_provider(const std::string &value) {
      std::string normalized;
      normalized.reserve(value.size());
      for (char ch : value) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
          normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
      }
      if (normalized == "nvidia" || normalized == "smoothmotion" || normalized == "nvidiasmoothmotion") {
        return "nvidia-smooth-motion";
      }
      if (normalized == "lossless" || normalized == "losslessscaling") {
        return "lossless-scaling";
      }
      return "lossless-scaling";
    }

    struct lossless_profile_defaults_t {
      bool performance_mode;
      int flow_scale;
      int resolution_scale;
      std::string scaling_mode;
      int sharpening;
      std::string anime4k_size;
      bool anime4k_vrs;
    };

    const lossless_profile_defaults_t LOSSLESS_DEFAULTS_RECOMMENDED {
      true,
      LOSSLESS_DEFAULT_FLOW_SCALE,
      LOSSLESS_DEFAULT_RESOLUTION_SCALE,
      "off",
      5,
      "S",
      false,
    };

    const lossless_profile_defaults_t LOSSLESS_DEFAULTS_CUSTOM {
      false,
      LOSSLESS_DEFAULT_FLOW_SCALE,
      LOSSLESS_DEFAULT_RESOLUTION_SCALE,
      "off",
      5,
      "S",
      false,
    };

    const std::array<std::string, 11> &lossless_scaling_modes() {
      static const std::array<std::string, 11> modes {
        "off",
        "ls1",
        "fsr",
        "nis",
        "sgsr",
        "bcas",
        "anime4k",
        "xbr",
        "sharp-bilinear",
        "integer",
        "nearest"
      };
      return modes;
    }

    std::optional<std::string> normalize_scaling_mode(const std::string &value) {
      std::string lower = boost::algorithm::to_lower_copy(value);
      const auto &modes = lossless_scaling_modes();
      if (std::find(modes.begin(), modes.end(), lower) != modes.end()) {
        return lower;
      }
      return std::nullopt;
    }

    bool scaling_mode_requires_sharpening(const std::string &mode) {
      static const std::array<std::string, 4> sharpening_modes {"ls1", "fsr", "nis", "sgsr"};
      return std::find(sharpening_modes.begin(), sharpening_modes.end(), mode) != sharpening_modes.end();
    }

    bool scaling_mode_is_anime(const std::string &mode) {
      return mode == "anime4k";
    }

    std::optional<std::string> scaling_mode_to_lossless_value(const std::string &mode) {
      if (mode == "off") {
        return std::string("Off");
      }
      if (mode == "ls1") {
        return std::string("LS1");
      }
      if (mode == "fsr") {
        return std::string("FSR");
      }
      if (mode == "nis") {
        return std::string("NIS");
      }
      if (mode == "sgsr") {
        return std::string("SGSR");
      }
      if (mode == "bcas") {
        return std::string("BicubicCAS");
      }
      if (mode == "anime4k") {
        return std::string("Anime4k");
      }
      if (mode == "xbr") {
        return std::string("XBR");
      }
      if (mode == "sharp-bilinear") {
        return std::string("SharpBilinear");
      }
      if (mode == "integer") {
        return std::string("Integer");
      }
      if (mode == "nearest") {
        return std::string("Nearest");
      }
      return std::nullopt;
    }

    int clamp_sharpness(int value) {
      return std::clamp(value, LOSSLESS_SHARPNESS_MIN, LOSSLESS_SHARPNESS_MAX);
    }

    struct lossless_runtime_values_t {
      std::string profile;
      std::optional<bool> performance_mode;
      std::optional<int> flow_scale;
      std::optional<double> resolution_scale_factor;
      std::optional<std::string> capture_api;
      std::optional<int> queue_target;
      std::optional<bool> hdr_enabled;
      std::optional<std::string> frame_generation;
      std::optional<std::string> lsfg3_mode;
      std::optional<std::string> scaling_type;
      std::optional<int> sharpness;
      std::optional<int> ls1_sharpness;
      std::optional<std::string> anime4k_type;
      std::optional<bool> anime4k_vrs;
    };

    std::optional<bool> pt_get_optional_bool(const pt::ptree &node, const std::string &key) {
      auto child = node.get_child_optional(key);
      if (!child) {
        return std::nullopt;
      }
      try {
        return child->get_value<bool>();
      } catch (...) {
      }
      try {
        auto text = child->get_value<std::string>();
        if (text.empty()) {
          return std::nullopt;
        }
        if (boost::iequals(text, "true") || text == "1") {
          return true;
        }
        if (boost::iequals(text, "false") || text == "0") {
          return false;
        }
      } catch (...) {
      }
      return std::nullopt;
    }

    std::optional<int> pt_get_optional_int(const pt::ptree &node, const std::string &key) {
      auto child = node.get_child_optional(key);
      if (!child) {
        return std::nullopt;
      }
      try {
        return child->get_value<int>();
      } catch (...) {
      }
      try {
        auto text = child->get_value<std::string>();
        if (text.empty()) {
          return std::nullopt;
        }
        return std::stoi(text);
      } catch (...) {
      }
      return std::nullopt;
    }

    void populate_lossless_overrides(const pt::ptree &node, lossless_scaling_profile_overrides_t &target) {
      if (auto perf = pt_get_optional_bool(node, "performance-mode")) {
        target.performance_mode = *perf;
      }
      if (auto flow = pt_get_optional_int(node, "flow-scale")) {
        target.flow_scale = *flow;
      }
      if (auto res = pt_get_optional_int(node, "resolution-scale")) {
        target.resolution_scale = *res;
      }
      if (auto scaling = node.get_optional<std::string>("scaling-type")) {
        if (auto normalized = normalize_scaling_mode(*scaling)) {
          target.scaling_type = *normalized;
        }
      }
      if (auto sharp = pt_get_optional_int(node, "sharpening")) {
        target.sharpening = clamp_sharpness(*sharp);
      }
      if (auto anime = node.get_optional<std::string>("anime4k-size")) {
        std::string value = boost::algorithm::to_upper_copy(*anime);
        target.anime4k_size = std::move(value);
      }
      if (auto vrs = pt_get_optional_bool(node, "anime4k-vrs")) {
        target.anime4k_vrs = *vrs;
      }
    }

    lossless_runtime_values_t compute_lossless_runtime(const ctx_t &ctx, bool frame_gen_enabled) {
      lossless_runtime_values_t result;
      const lossless_profile_defaults_t &defaults = boost::iequals(ctx.lossless_scaling_profile, LOSSLESS_PROFILE_RECOMMENDED) ?
                                                      LOSSLESS_DEFAULTS_RECOMMENDED :
                                                      LOSSLESS_DEFAULTS_CUSTOM;

      const lossless_scaling_profile_overrides_t &overrides = boost::iequals(ctx.lossless_scaling_profile, LOSSLESS_PROFILE_RECOMMENDED) ?
                                                                ctx.lossless_scaling_recommended :
                                                                ctx.lossless_scaling_custom;

      if (boost::iequals(ctx.lossless_scaling_profile, LOSSLESS_PROFILE_RECOMMENDED)) {
        result.profile = LOSSLESS_PROFILE_RECOMMENDED;
        result.capture_api = "WGC";
        result.queue_target = 0;
        result.hdr_enabled = true;
        if (frame_gen_enabled) {
          result.frame_generation = "LSFG3";
          result.lsfg3_mode = "ADAPTIVE";
        }
      } else {
        result.profile = LOSSLESS_PROFILE_CUSTOM;
        if (frame_gen_enabled) {
          result.frame_generation = "LSFG3";
        }
      }

      bool performance_mode = overrides.performance_mode.value_or(defaults.performance_mode);
      result.performance_mode = performance_mode;

      int flow_scale = overrides.flow_scale.value_or(defaults.flow_scale);
      flow_scale = std::clamp(flow_scale, LOSSLESS_MIN_FLOW_SCALE, LOSSLESS_MAX_FLOW_SCALE);
      result.flow_scale = flow_scale;

      std::string scaling_mode = overrides.scaling_type.has_value() ? *overrides.scaling_type : defaults.scaling_mode;
      auto normalized_mode = normalize_scaling_mode(scaling_mode);
      if (!normalized_mode) {
        normalized_mode = defaults.scaling_mode;
      }

      // Only apply resolution scaling if scaling type is not 'off'
      if (*normalized_mode != "off") {
        int resolution_scale = overrides.resolution_scale.value_or(defaults.resolution_scale);
        resolution_scale = std::clamp(resolution_scale, LOSSLESS_MIN_RESOLUTION_SCALE, LOSSLESS_MAX_RESOLUTION_SCALE);
        double factor = 100.0 / static_cast<double>(resolution_scale);
        factor = std::clamp(factor, 1.0, 10.0);
        factor = std::round(factor * 100.0) / 100.0;
        result.resolution_scale_factor = factor;
      } else {
        // When scaling is off, use unity scale factor to disable custom scaling
        result.resolution_scale_factor = 1.0;
      }

      if (auto mapped = scaling_mode_to_lossless_value(*normalized_mode)) {
        result.scaling_type = *mapped;
      }

      if (scaling_mode_requires_sharpening(*normalized_mode)) {
        int sharpness = overrides.sharpening.value_or(defaults.sharpening);
        sharpness = clamp_sharpness(sharpness);
        result.sharpness = sharpness;
        if (*normalized_mode == "ls1") {
          result.ls1_sharpness = sharpness;
        }
      }

      if (scaling_mode_is_anime(*normalized_mode)) {
        std::string anime_type = overrides.anime4k_size.has_value() ? *overrides.anime4k_size : defaults.anime4k_size;
        boost::algorithm::to_upper(anime_type);
        result.anime4k_type = anime_type;
        bool vrs = overrides.anime4k_vrs.value_or(defaults.anime4k_vrs);
        result.anime4k_vrs = vrs;
      }

      return result;
    }

#ifdef _WIN32
    constexpr auto k_lossless_observation_duration = std::chrono::seconds(10);
    constexpr auto k_lossless_poll_interval = std::chrono::milliseconds(250);

    struct lossless_process_candidate {
      DWORD pid = 0;
      ULONGLONG start_cpu = 0;
      ULONGLONG last_cpu = 0;
      SIZE_T peak_working_set = 0;
      std::wstring path;
      std::chrono::steady_clock::time_point first_seen;
      std::chrono::steady_clock::time_point last_seen;
    };

    struct lossless_selection {
      DWORD pid = 0;
      std::wstring path_wide;
      std::string path_utf8;
      std::string directory_utf8;
    };

    std::vector<DWORD> enumerate_process_ids_snapshot() {
      DWORD needed = 0;
      std::vector<DWORD> pids(1024);
      while (true) {
        if (!EnumProcesses(pids.data(), static_cast<DWORD>(pids.size() * sizeof(DWORD)), &needed)) {
          return {};
        }
        if (needed < pids.size() * sizeof(DWORD)) {
          pids.resize(needed / sizeof(DWORD));
          break;
        }
        pids.resize(pids.size() * 2);
      }
      return pids;
    }

    std::unordered_set<DWORD> capture_process_baseline_for_lossless() {
      std::unordered_set<DWORD> result;
      auto snapshot = enumerate_process_ids_snapshot();
      result.reserve(snapshot.size());
      for (auto pid : snapshot) {
        if (pid != 0) {
          result.insert(pid);
        }
      }
      return result;
    }

    bool sample_process_usage(DWORD pid, ULONGLONG &cpu_time, SIZE_T &working_set) {
      HANDLE handle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
      if (!handle) {
        handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
      }
      if (!handle) {
        return false;
      }
      FILETIME creation {}, exit_time {}, kernel {}, user {};
      BOOL got_times = GetProcessTimes(handle, &creation, &exit_time, &kernel, &user);
      PROCESS_MEMORY_COUNTERS_EX pmc {};
      BOOL got_mem = GetProcessMemoryInfo(handle, reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof(pmc));
      CloseHandle(handle);
      if (!got_times) {
        return false;
      }
      ULARGE_INTEGER kernel_int {};
      kernel_int.HighPart = kernel.dwHighDateTime;
      kernel_int.LowPart = kernel.dwLowDateTime;
      ULARGE_INTEGER user_int {};
      user_int.HighPart = user.dwHighDateTime;
      user_int.LowPart = user.dwLowDateTime;
      cpu_time = kernel_int.QuadPart + user_int.QuadPart;
      working_set = got_mem ? pmc.WorkingSetSize : 0;
      return true;
    }

    std::optional<std::wstring> query_process_image_path_optional(DWORD pid) {
      std::wstring path;
      if (playnite_launcher::focus::get_process_image_path(pid, path)) {
        return path;
      }
      return std::nullopt;
    }

    std::wstring normalize_lowercase_path(const std::wstring &path) {
      std::wstring normalized = path;
      for (auto &ch : normalized) {
        if (ch == L'/') {
          ch = L'\\';
        }
        ch = static_cast<wchar_t>(std::towlower(ch));
      }
      return normalized;
    }

    bool path_matches_preferred(const std::wstring &path, const std::wstring &preferred_normalized) {
      if (preferred_normalized.empty()) {
        return false;
      }
      auto normalized = normalize_lowercase_path(path);
      if (normalized.size() < preferred_normalized.size()) {
        return false;
      }
      if (normalized.compare(0, preferred_normalized.size(), preferred_normalized) != 0) {
        return false;
      }
      if (normalized.size() == preferred_normalized.size()) {
        return true;
      }
      return normalized[preferred_normalized.size()] == L'\\';
    }

    std::optional<lossless_selection> detect_lossless_candidate(const std::unordered_set<DWORD> &baseline, DWORD root_pid, const std::wstring &preferred_normalized, std::atomic_bool &stop_flag) {
      std::unordered_map<DWORD, lossless_process_candidate> candidates;
      auto deadline = std::chrono::steady_clock::now() + k_lossless_observation_duration;

      SYSTEM_INFO sys_info {};
      GetSystemInfo(&sys_info);
      double cpu_count = sys_info.dwNumberOfProcessors > 0 ? static_cast<double>(sys_info.dwNumberOfProcessors) : 1.0;

      while (std::chrono::steady_clock::now() < deadline) {
        if (stop_flag.load(std::memory_order_acquire)) {
          return std::nullopt;
        }

        auto now = std::chrono::steady_clock::now();
        auto snapshot = enumerate_process_ids_snapshot();
        for (auto pid : snapshot) {
          if (pid == 0 || baseline.find(pid) != baseline.end()) {
            continue;
          }
          auto &entry = candidates[pid];
          if (entry.pid == 0) {
            entry.pid = pid;
            entry.first_seen = now;
            entry.last_seen = now;
          }
          ULONGLONG cpu_time = 0;
          SIZE_T working_set = 0;
          if (!sample_process_usage(pid, cpu_time, working_set)) {
            if (entry.start_cpu == 0) {
              candidates.erase(pid);
            }
            continue;
          }
          if (entry.start_cpu == 0) {
            entry.start_cpu = cpu_time;
          }
          entry.last_cpu = cpu_time;
          entry.last_seen = now;
          if (working_set > entry.peak_working_set) {
            entry.peak_working_set = working_set;
          }
          if (entry.path.empty()) {
            if (auto path = query_process_image_path_optional(pid)) {
              entry.path = std::move(*path);
            }
          }
        }

        std::this_thread::sleep_for(k_lossless_poll_interval);
      }

      if (stop_flag.load(std::memory_order_acquire)) {
        return std::nullopt;
      }

      if (candidates.empty()) {
        return std::nullopt;
      }

      double max_cpu_ratio = 0.0;
      double max_mem = 0.0;

      struct candidate_score {
        DWORD pid;
        std::wstring path;
        double cpu_ratio;
        double mem_mb;
        bool preferred_match;
      };

      std::vector<candidate_score> scores;
      scores.reserve(candidates.size());

      for (auto &[pid, candidate] : candidates) {
        if (candidate.start_cpu == 0 || candidate.last_cpu < candidate.start_cpu) {
          continue;
        }
        if (candidate.last_seen <= candidate.first_seen) {
          continue;
        }
        if (candidate.path.empty()) {
          if (auto refreshed = query_process_image_path_optional(pid)) {
            candidate.path = std::move(*refreshed);
          }
        }
        if (candidate.path.empty()) {
          continue;
        }
        double elapsed = std::chrono::duration<double>(candidate.last_seen - candidate.first_seen).count();
        if (elapsed <= 0.1) {
          elapsed = 0.1;
        }
        double cpu_seconds = static_cast<double>(candidate.last_cpu - candidate.start_cpu) / 10000000.0;
        if (cpu_seconds < 0.0) {
          cpu_seconds = 0.0;
        }
        double cpu_ratio = cpu_seconds / (elapsed * cpu_count);
        if (cpu_ratio < 0.0) {
          cpu_ratio = 0.0;
        }
        double mem_mb = static_cast<double>(candidate.peak_working_set) / (1024.0 * 1024.0);
        bool matches = path_matches_preferred(candidate.path, preferred_normalized);
        scores.push_back({pid, candidate.path, cpu_ratio, mem_mb, matches});
        max_cpu_ratio = std::max(max_cpu_ratio, cpu_ratio);
        max_mem = std::max(max_mem, mem_mb);
      }

      if (scores.empty()) {
        return std::nullopt;
      }

      bool cpu_low = max_cpu_ratio < 0.08;
      double cpu_weight = cpu_low ? 0.5 : 0.7;
      double mem_weight = 1.0 - cpu_weight;

      auto ensure_dir_prefix = [](std::wstring value) {
        if (!value.empty() && value.back() != L'\\') {
          value.push_back(L'\\');
        }
        return normalize_lowercase_path(value);
      };

      std::wstring windows_dir_norm;
      {
        wchar_t windows_dir[MAX_PATH] = {};
        UINT len = GetWindowsDirectoryW(windows_dir, ARRAYSIZE(windows_dir));
        if (len > 0 && len < ARRAYSIZE(windows_dir)) {
          windows_dir_norm = ensure_dir_prefix(std::wstring(windows_dir, len));
        }
      }

      auto has_prefix = [](const std::wstring &value, const std::wstring &prefix) {
        return !prefix.empty() && value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
      };

      const candidate_score *best = nullptr;
      double best_score = -1.0;

      for (const auto &score : scores) {
        double cpu_norm = max_cpu_ratio > 0.0 ? score.cpu_ratio / max_cpu_ratio : 0.0;
        double mem_norm = max_mem > 0.0 ? score.mem_mb / max_mem : 0.0;
        double combined = (cpu_weight * cpu_norm) + (mem_weight * mem_norm);
        if (score.preferred_match) {
          combined += 0.2;
        }
        if (score.pid == root_pid) {
          combined += score.preferred_match ? 0.05 : -0.05;
        }
        combined += std::min(score.cpu_ratio, 1.0) * 0.15;

        if (!windows_dir_norm.empty()) {
          auto normalized_path = normalize_lowercase_path(score.path);
          bool system_path = has_prefix(normalized_path, windows_dir_norm);
          if (system_path) {
            combined -= 0.2;
          }
          if (system_path && score.cpu_ratio < 0.02 && score.mem_mb < 48.0) {
            combined -= 0.05;
          }
        } else if (score.cpu_ratio < 0.015 && score.mem_mb < 32.0) {
          combined -= 0.05;
        }

        if (combined > best_score) {
          best_score = combined;
          best = &score;
        }
      }

      if (!best) {
        return std::nullopt;
      }

      lossless_selection selection;
      selection.pid = best->pid;
      selection.path_wide = best->path;
      selection.path_utf8 = platf::dxgi::wide_to_utf8(best->path);
      std::filesystem::path fs_path(best->path);
      auto parent = fs_path.parent_path();
      if (!parent.empty()) {
        selection.directory_utf8 = platf::dxgi::wide_to_utf8(parent.wstring());
      }

      BOOST_LOG(debug) << "Lossless Scaling: candidate PID=" << selection.pid << " exe=" << selection.path_utf8
                       << " cpu=" << best->cpu_ratio << " memMB=" << best->mem_mb;

      return selection;
    }
#endif
  }  // namespace

#ifdef _WIN32
  VDISPLAY::DRIVER_STATUS vDisplayDriverStatus = VDISPLAY::DRIVER_STATUS::UNKNOWN;

  void onVDisplayWatchdogFailed() {
    vDisplayDriverStatus = VDISPLAY::DRIVER_STATUS::WATCHDOG_FAILED;
    VDISPLAY::closeVDisplayDevice();
  }

  void initVDisplayDriver() {
    vDisplayDriverStatus = VDISPLAY::openVDisplayDevice();
    if (vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK) {
      if (!VDISPLAY::startPingThread(onVDisplayWatchdogFailed)) {
        onVDisplayWatchdogFailed();
      }
    }
  }
#endif

  proc_t proc;

  // Custom move operations to allow global proc replacement if ever needed
  proc_t::proc_t(proc_t &&other) noexcept:
      _app_id(other._app_id),
      _env(std::move(other._env)),
      _apps(std::move(other._apps)),
      _app(std::move(other._app)),
      _app_launch_time(other._app_launch_time),
      placebo(other.placebo),
      _process(std::move(other._process)),
      _process_group(std::move(other._process_group)),
      _pipe(std::move(other._pipe)),
      _app_prep_it(other._app_prep_it),
      _app_prep_begin(other._app_prep_begin)
#ifdef _WIN32
      ,
      _lossless_thread(std::move(other._lossless_thread)),
      _lossless_profile_applied(other._lossless_profile_applied),
      _lossless_backup(other._lossless_backup),
      _lossless_last_install_dir(std::move(other._lossless_last_install_dir)),
      _lossless_last_exe_path(std::move(other._lossless_last_exe_path)),
      _virtual_display_guid(other._virtual_display_guid),
      _virtual_display_active(other._virtual_display_active)
#endif
  {
#ifdef _WIN32
    _lossless_stop_requested.store(other._lossless_stop_requested.load(std::memory_order_acquire), std::memory_order_release);
    other._lossless_profile_applied = false;
#endif
  }

  proc_t &proc_t::operator=(proc_t &&other) noexcept {
    if (this != &other) {
      std::scoped_lock lk(_apps_mutex, other._apps_mutex);
#ifdef _WIN32
      stop_lossless_scaling_support();
#endif
      _app_id = other._app_id;
      _env = std::move(other._env);
      _apps = std::move(other._apps);
      _app = std::move(other._app);
      _app_launch_time = other._app_launch_time;
      placebo = other.placebo;
      _process = std::move(other._process);
      _process_group = std::move(other._process_group);
      _pipe = std::move(other._pipe);
      _app_prep_it = other._app_prep_it;
      _app_prep_begin = other._app_prep_begin;
#ifdef _WIN32
      _lossless_thread = std::move(other._lossless_thread);
      _lossless_stop_requested.store(other._lossless_stop_requested.load(std::memory_order_acquire), std::memory_order_release);
      _lossless_profile_applied = other._lossless_profile_applied;
      _lossless_backup = other._lossless_backup;
      _lossless_last_install_dir = std::move(other._lossless_last_install_dir);
      _lossless_last_exe_path = std::move(other._lossless_last_exe_path);
      _virtual_display_guid = other._virtual_display_guid;
      _virtual_display_active = other._virtual_display_active;
      other._lossless_profile_applied = false;
#endif
    }
    return *this;
  }

#ifdef _WIN32
  void proc_t::start_lossless_scaling_support(std::unordered_set<DWORD> baseline_pids, const playnite_launcher::lossless::lossless_scaling_app_metadata &metadata, std::string install_dir_hint_utf8, DWORD root_pid) {
    stop_lossless_scaling_support();
    _lossless_stop_requested.store(false, std::memory_order_release);

    _lossless_thread = std::thread([this,
                                    baseline = std::move(baseline_pids),
                                    metadata,
                                    install_dir_hint = std::move(install_dir_hint_utf8),
                                    root_pid]() mutable {
      try {
        std::wstring preferred_directory;
        if (!install_dir_hint.empty()) {
          preferred_directory = platf::dxgi::utf8_to_wide(install_dir_hint);
          preferred_directory = normalize_lowercase_path(preferred_directory);
          while (!preferred_directory.empty() && preferred_directory.back() == L'\\') {
            preferred_directory.pop_back();
          }
        }

        BOOST_LOG(debug) << "Lossless Scaling: monitoring for new processes (baseline=" << baseline.size() << ", root_pid=" << root_pid << ")";

        auto selection = detect_lossless_candidate(baseline, root_pid, preferred_directory, _lossless_stop_requested);
        if (!selection || _lossless_stop_requested.load(std::memory_order_acquire)) {
          BOOST_LOG(debug) << "Lossless Scaling: no candidate detected within bootstrap window";
          return;
        }

        auto options = playnite_launcher::lossless::read_lossless_scaling_options(metadata);
        if (!options.enabled) {
          BOOST_LOG(debug) << "Lossless Scaling: disabled by configuration, skipping auto launch";
          return;
        }

        auto runtime = playnite_launcher::lossless::capture_lossless_scaling_state();
  #ifdef _WIN32
        if (!runtime.exe_path && metadata.configured_path) {
          try {
            runtime.exe_path = metadata.configured_path->wstring();
          } catch (...) {
          }
        }
  #endif
        if (_lossless_stop_requested.load(std::memory_order_acquire)) {
          return;
        }
        if (!runtime.running_pids.empty()) {
          playnite_launcher::lossless::lossless_scaling_stop_processes(runtime);
        }

        playnite_launcher::lossless::lossless_scaling_profile_backup backup;
        std::string install_dir = install_dir_hint.empty() ? selection->directory_utf8 : install_dir_hint;
        bool changed = playnite_launcher::lossless::lossless_scaling_apply_global_profile(options, install_dir, selection->path_utf8, backup);

        {
          std::lock_guard lk(_lossless_mutex);
          _lossless_backup = backup;
          _lossless_profile_applied = backup.valid;
          if (_lossless_profile_applied) {
            _lossless_last_install_dir = install_dir;
            _lossless_last_exe_path = selection->path_utf8;
          } else {
            _lossless_last_install_dir.clear();
            _lossless_last_exe_path.clear();
          }
        }

        if (_lossless_stop_requested.load(std::memory_order_acquire)) {
          return;
        }

        playnite_launcher::lossless::lossless_scaling_restart_foreground(runtime, changed, install_dir, selection->path_utf8, selection->pid);
        BOOST_LOG(info) << "Lossless Scaling: launched helper for PID=" << selection->pid << " (" << selection->path_utf8 << ')';
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Lossless Scaling: exception during auto launch: " << e.what();
      } catch (...) {
        BOOST_LOG(warning) << "Lossless Scaling: unknown exception during auto launch";
      }
    });
  }

  void proc_t::stop_lossless_scaling_support() {
    _lossless_stop_requested.store(true, std::memory_order_release);
    if (_lossless_thread.joinable()) {
      _lossless_thread.join();
    }
    _lossless_stop_requested.store(false, std::memory_order_release);

    bool restore = false;
    playnite_launcher::lossless::lossless_scaling_profile_backup backup;
    {
      std::lock_guard lk(_lossless_mutex);
      if (_lossless_profile_applied) {
        backup = _lossless_backup;
        restore = backup.valid;
        _lossless_profile_applied = false;
      }
      _lossless_last_install_dir.clear();
      _lossless_last_exe_path.clear();
    }

    if (restore) {
      auto runtime = playnite_launcher::lossless::capture_lossless_scaling_state();
      if (!runtime.running_pids.empty()) {
        playnite_launcher::lossless::lossless_scaling_stop_processes(runtime);
      }
      if (playnite_launcher::lossless::lossless_scaling_restore_global_profile(backup)) {
        BOOST_LOG(info) << "Lossless Scaling: restored previous profile";
      }
    }
  }
#endif

  class deinit_t: public platf::deinit_t {
  public:
    ~deinit_t() {
      proc.terminate();
    }
  };

  std::unique_ptr<platf::deinit_t> init() {
    return std::make_unique<deinit_t>();
  }

  void terminate_process_group(boost::process::v1::child &proc, boost::process::v1::group &group, std::chrono::seconds exit_timeout) {
    if (group.valid() && platf::process_group_running((std::uintptr_t) group.native_handle())) {
      if (exit_timeout.count() > 0) {
        // Request processes in the group to exit gracefully
        if (platf::request_process_group_exit((std::uintptr_t) group.native_handle())) {
          // If the request was successful, wait for a little while for them to exit.
          BOOST_LOG(info) << "Successfully requested the app to exit. Waiting up to "sv << exit_timeout.count() << " seconds for it to close."sv;

          // group::wait_for() and similar functions are broken and deprecated, so we use a simple polling loop
          while (platf::process_group_running((std::uintptr_t) group.native_handle()) && (--exit_timeout).count() >= 0) {
            std::this_thread::sleep_for(1s);
          }

          if (exit_timeout.count() < 0) {
            BOOST_LOG(warning) << "App did not fully exit within the timeout. Terminating the app's remaining processes."sv;
          } else {
            BOOST_LOG(info) << "All app processes have successfully exited."sv;
          }
        } else {
          BOOST_LOG(info) << "App did not respond to a graceful termination request. Forcefully terminating the app's processes."sv;
        }
      } else {
        BOOST_LOG(info) << "No graceful exit timeout was specified for this app. Forcefully terminating the app's processes."sv;
      }

      // We always call terminate() even if we waited successfully for all processes above.
      // This ensures the process group state is consistent with the OS in boost.
      std::error_code ec;
      group.terminate(ec);
      group.detach();
    }

    if (proc.valid()) {
      // avoid zombie process
      proc.detach();
    }
  }

  boost::filesystem::path find_working_directory(const std::string &cmd, boost::process::v1::environment &env) {
    // Parse the raw command string into parts to get the actual command portion
#ifdef _WIN32
    auto parts = boost::program_options::split_winmain(cmd);
#else
    auto parts = boost::program_options::split_unix(cmd);
#endif
    if (parts.empty()) {
      BOOST_LOG(error) << "Unable to parse command: "sv << cmd;
      return boost::filesystem::path();
    }

    BOOST_LOG(debug) << "Parsed target ["sv << parts.at(0) << "] from command ["sv << cmd << ']';

    // If the target is a URL, don't parse any further here
    if (parts.at(0).find("://") != std::string::npos) {
      return boost::filesystem::path();
    }

    // If the cmd path is not an absolute path, resolve it using our PATH variable
    boost::filesystem::path cmd_path(parts.at(0));
    if (!cmd_path.is_absolute()) {
      cmd_path = boost::process::v1::search_path(parts.at(0));
      if (cmd_path.empty()) {
        BOOST_LOG(error) << "Unable to find executable ["sv << parts.at(0) << "]. Is it in your PATH?"sv;
        return boost::filesystem::path();
      }
    }

    BOOST_LOG(debug) << "Resolved target ["sv << parts.at(0) << "] to path ["sv << cmd_path << ']';

    // Now that we have a complete path, we can just use parent_path()
    return cmd_path.parent_path();
  }

  int proc_t::execute(int app_id, std::shared_ptr<rtsp_stream::launch_session_t> launch_session) {
    // Ensure starting from a clean slate
    terminate();

#ifdef _WIN32
    std::unordered_set<DWORD> lossless_baseline_pids;
    playnite_launcher::lossless::lossless_scaling_app_metadata lossless_metadata;
    bool should_start_lossless_support = false;
    bool lossless_monitor_started = false;
    std::string lossless_install_dir_hint;
    _virtual_display_active = false;
    _virtual_display_guid = GUID {};
#endif

    auto iter = std::find_if(_apps.begin(), _apps.end(), [&app_id](const auto app) {
      return app.id == std::to_string(app_id);
    });

    if (iter == _apps.end()) {
      BOOST_LOG(error) << "Couldn't find app with ID ["sv << app_id << ']';
      return 404;
    }

    _app_id = app_id;
    _app = *iter;
    launch_session->gen1_framegen_fix = _app.gen1_framegen_fix;
    launch_session->gen2_framegen_fix = _app.gen2_framegen_fix;
    launch_session->lossless_scaling_framegen = _app.lossless_scaling_framegen;
    launch_session->lossless_scaling_target_fps = _app.lossless_scaling_target_fps;
    launch_session->lossless_scaling_rtss_limit = _app.lossless_scaling_rtss_limit;
    launch_session->frame_generation_provider = _app.frame_generation_provider;
    if (launch_session->fps > 0 && (launch_session->gen1_framegen_fix || launch_session->gen2_framegen_fix)) {
      if (launch_session->fps > std::numeric_limits<int>::max() / 2) {
        launch_session->framegen_refresh_rate = std::numeric_limits<int>::max();
      } else {
        launch_session->framegen_refresh_rate = launch_session->fps * 2;
      }
    } else {
      launch_session->framegen_refresh_rate.reset();
    }
    _app_prep_begin = std::begin(_app.prep_cmds);
    _app_prep_it = _app_prep_begin;

    // Add Stream-specific environment variables
    _env["SUNSHINE_APP_ID"] = std::to_string(_app_id);
    _env["SUNSHINE_APP_NAME"] = _app.name;
    _env["SUNSHINE_CLIENT_WIDTH"] = std::to_string(launch_session->width);
    _env["SUNSHINE_CLIENT_HEIGHT"] = std::to_string(launch_session->height);
    _env["SUNSHINE_CLIENT_FPS"] = std::to_string(launch_session->fps);
    _env["SUNSHINE_CLIENT_HDR"] = launch_session->enable_hdr ? "true" : "false";
    _env["SUNSHINE_CLIENT_GCMAP"] = std::to_string(launch_session->gcmap);
    _env["SUNSHINE_CLIENT_HOST_AUDIO"] = launch_session->host_audio ? "true" : "false";
    _env["SUNSHINE_CLIENT_ENABLE_SOPS"] = launch_session->enable_sops ? "true" : "false";
    int channelCount = launch_session->surround_info & 65535;
    switch (channelCount) {
      case 2:
        _env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "2.0";
        break;
      case 6:
        _env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "5.1";
        break;
      case 8:
        _env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "7.1";
        break;
    }
    _env["SUNSHINE_CLIENT_AUDIO_SURROUND_PARAMS"] = launch_session->surround_params;

    try {
      _env["SUNSHINE_LOSSLESS_SCALING_EXE"] = config::lossless_scaling.exe_path;
    } catch (...) {
      _env["SUNSHINE_LOSSLESS_SCALING_EXE"] = "";
    }

#ifdef _WIN32
    if (launch_session->virtual_display) {
      if (launch_session->virtual_display_detach_with_app) {
        const bool has_guid = std::any_of(
          launch_session->virtual_display_guid_bytes.begin(),
          launch_session->virtual_display_guid_bytes.end(),
          [](std::uint8_t b) {
            return b != 0;
          }
        );
        if (has_guid) {
          std::memcpy(&_virtual_display_guid, launch_session->virtual_display_guid_bytes.data(), sizeof(_virtual_display_guid));
          _virtual_display_active = true;
        } else {
          launch_session->virtual_display = false;
          launch_session->virtual_display_detach_with_app = false;
          launch_session->virtual_display_guid_bytes.fill(0);
          launch_session->virtual_display_device_id.clear();
        }
      }
    } else {
      bool request_virtual_display = boost::iequals(config::video.output_name, VDISPLAY::SUDOVDA_VIRTUAL_DISPLAY_SELECTION);
      if (!request_virtual_display && _app.virtual_screen) {
        request_virtual_display = true;
      }
      if (request_virtual_display) {
        if (vDisplayDriverStatus != VDISPLAY::DRIVER_STATUS::OK) {
          initVDisplayDriver();
        }

        if (vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK) {
          if (!config::video.adapter_name.empty()) {
            (void) VDISPLAY::setRenderAdapterByName(platf::from_utf8(config::video.adapter_name));
          }

          uuid_util::uuid_t parsed_uuid {};
          bool parsed = false;
          std::string display_uuid_source = launch_session->unique_id;
          if (!display_uuid_source.empty()) {
            try {
              parsed_uuid = uuid_util::uuid_t::parse(display_uuid_source);
              parsed = true;
            } catch (...) {
              parsed = false;
            }
          }
          if (!parsed) {
            parsed_uuid = uuid_util::uuid_t::generate();
            display_uuid_source = parsed_uuid.string();
            launch_session->unique_id = display_uuid_source;
          }

          std::memcpy(&_virtual_display_guid, parsed_uuid.b8, sizeof(_virtual_display_guid));
          std::copy(parsed_uuid.b8, parsed_uuid.b8 + 16, launch_session->virtual_display_guid_bytes.begin());

          uint32_t vd_width = launch_session->width > 0 ? static_cast<uint32_t>(launch_session->width) : 1920u;
          uint32_t vd_height = launch_session->height > 0 ? static_cast<uint32_t>(launch_session->height) : 1080u;
          uint32_t vd_fps = 0;
          if (launch_session->framegen_refresh_rate && *launch_session->framegen_refresh_rate > 0) {
            vd_fps = static_cast<uint32_t>(*launch_session->framegen_refresh_rate);
          } else if (launch_session->fps > 0) {
            vd_fps = static_cast<uint32_t>(launch_session->fps);
          } else {
            vd_fps = 60000u;
          }
          if (vd_fps < 1000u) {
            vd_fps *= 1000u;
          }

          std::string client_label = !launch_session->device_name.empty() ? launch_session->device_name : config::nvhttp.sunshine_name;
          if (client_label.empty()) {
            client_label = "Sunshine";
          }

          const bool snapshot_ok = display_helper_integration::ensure_session_snapshot();
          if (!snapshot_ok) {
            BOOST_LOG(warning) << "Failed to snapshot display configuration before creating virtual display.";
          }

          auto display_name_wide = VDISPLAY::createVirtualDisplay(
            display_uuid_source.c_str(),
            client_label.c_str(),
            vd_width,
            vd_height,
            vd_fps,
            _virtual_display_guid
          );

          if (!display_name_wide.empty()) {
            _virtual_display_active = true;
            launch_session->virtual_display = true;
            launch_session->virtual_display_detach_with_app = true;
            if (auto resolved_device = VDISPLAY::resolveVirtualDisplayDeviceId(display_name_wide)) {
              launch_session->virtual_display_device_id = *resolved_device;
            } else {
              launch_session->virtual_display_device_id.clear();
            }
            BOOST_LOG(info) << "Virtual display created at " << platf::to_utf8(display_name_wide);
          } else {
            launch_session->virtual_display = false;
            launch_session->virtual_display_detach_with_app = false;
            launch_session->virtual_display_guid_bytes.fill(0);
            std::memset(&_virtual_display_guid, 0, sizeof(_virtual_display_guid));
            launch_session->virtual_display_device_id.clear();
            BOOST_LOG(warning) << "Virtual display creation failed.";
          }
        } else {
          launch_session->virtual_display = false;
          launch_session->virtual_display_detach_with_app = false;
          launch_session->virtual_display_guid_bytes.fill(0);
          launch_session->virtual_display_device_id.clear();
          BOOST_LOG(warning) << "SudoVDA driver unavailable (status=" << static_cast<int>(vDisplayDriverStatus) << ")";
        }
      } else {
        launch_session->virtual_display = false;
        launch_session->virtual_display_detach_with_app = false;
        launch_session->virtual_display_guid_bytes.fill(0);
        launch_session->virtual_display_device_id.clear();
      }
    }
#endif

    auto clear_lossless_runtime_env = [&]() {
      _env[ENV_LOSSLESS_PROFILE] = "";
      _env[ENV_LOSSLESS_CAPTURE_API] = "";
      _env[ENV_LOSSLESS_QUEUE_TARGET] = "";
      _env[ENV_LOSSLESS_HDR] = "";
      _env[ENV_LOSSLESS_FLOW_SCALE] = "";
      _env[ENV_LOSSLESS_PERFORMANCE_MODE] = "";
      _env[ENV_LOSSLESS_RESOLUTION] = "";
      _env[ENV_LOSSLESS_FRAMEGEN_MODE] = "";
      _env[ENV_LOSSLESS_LSFG3_MODE] = "";
      _env[ENV_LOSSLESS_SCALING_TYPE] = "";
      _env[ENV_LOSSLESS_SHARPNESS] = "";
      _env[ENV_LOSSLESS_LS1_SHARPNESS] = "";
      _env[ENV_LOSSLESS_ANIME4K_TYPE] = "";
      _env[ENV_LOSSLESS_ANIME4K_VRS] = "";
    };

    _env["SUNSHINE_FRAME_GENERATION_PROVIDER"] = _app.lossless_scaling_framegen ? _app.frame_generation_provider : "";

    const bool using_lossless_provider = _app.lossless_scaling_framegen &&
                                         boost::iequals(_app.frame_generation_provider, "lossless-scaling");
    if (using_lossless_provider) {
      _env["SUNSHINE_LOSSLESS_SCALING_FRAMEGEN"] = "1";
      if (_app.lossless_scaling_target_fps) {
        _env["SUNSHINE_LOSSLESS_SCALING_TARGET_FPS"] = std::to_string(*_app.lossless_scaling_target_fps);
      } else {
        _env["SUNSHINE_LOSSLESS_SCALING_TARGET_FPS"] = "";
      }
      if (_app.lossless_scaling_rtss_limit) {
        _env["SUNSHINE_LOSSLESS_SCALING_RTSS_LIMIT"] = std::to_string(*_app.lossless_scaling_rtss_limit);
      } else {
        _env["SUNSHINE_LOSSLESS_SCALING_RTSS_LIMIT"] = "";
      }

      const bool wants_lossless_framegen = using_lossless_provider &&
                                           (_app.lossless_scaling_target_fps.has_value() || _app.lossless_scaling_rtss_limit.has_value());
      auto runtime = compute_lossless_runtime(_app, wants_lossless_framegen);
#ifdef _WIN32
      bool has_launch_commands = !_app.cmd.empty() || !_app.detached.empty();
      should_start_lossless_support = has_launch_commands && _app.playnite_id.empty() && !_app.playnite_fullscreen;
      if (should_start_lossless_support) {
        lossless_metadata.enabled = true;
        lossless_metadata.target_fps = _app.lossless_scaling_target_fps;
        lossless_metadata.rtss_limit = _app.lossless_scaling_rtss_limit;
        if (!config::lossless_scaling.exe_path.empty()) {
          lossless_metadata.configured_path = std::filesystem::u8path(config::lossless_scaling.exe_path);
        }
        lossless_metadata.active_profile = runtime.profile;
        lossless_metadata.capture_api = runtime.capture_api;
        lossless_metadata.queue_target = runtime.queue_target;
        lossless_metadata.hdr_enabled = runtime.hdr_enabled;
        lossless_metadata.flow_scale = runtime.flow_scale;
        lossless_metadata.performance_mode = runtime.performance_mode;
        lossless_metadata.resolution_scale_factor = runtime.resolution_scale_factor;
        lossless_metadata.frame_generation_mode = runtime.frame_generation;
        lossless_metadata.lsfg3_mode = runtime.lsfg3_mode;
        lossless_metadata.scaling_type = runtime.scaling_type;
        lossless_metadata.sharpness = runtime.sharpness;
        lossless_metadata.ls1_sharpness = runtime.ls1_sharpness;
        lossless_metadata.anime4k_type = runtime.anime4k_type;
        lossless_metadata.anime4k_vrs = runtime.anime4k_vrs;
      }
#endif

      auto set_string = [&](const char *key, const std::optional<std::string> &value) {
        if (value && !value->empty()) {
          _env[key] = *value;
        } else {
          _env[key] = "";
        }
      };
      auto set_int = [&](const char *key, const std::optional<int> &value) {
        if (value.has_value()) {
          _env[key] = std::to_string(*value);
        } else {
          _env[key] = "";
        }
      };
      auto set_double = [&](const char *key, const std::optional<double> &value) {
        if (value.has_value()) {
          std::ostringstream stream;
          stream.setf(std::ios::fixed);
          stream << std::setprecision(2) << *value;
          _env[key] = stream.str();
        } else {
          _env[key] = "";
        }
      };
      auto set_bool = [&](const char *key, const std::optional<bool> &value) {
        if (value.has_value()) {
          _env[key] = *value ? "1" : "0";
        } else {
          _env[key] = "";
        }
      };

      _env[ENV_LOSSLESS_PROFILE] = runtime.profile;
      set_string(ENV_LOSSLESS_CAPTURE_API, runtime.capture_api);
      set_int(ENV_LOSSLESS_QUEUE_TARGET, runtime.queue_target);
      set_bool(ENV_LOSSLESS_HDR, runtime.hdr_enabled);
      set_int(ENV_LOSSLESS_FLOW_SCALE, runtime.flow_scale);
      set_bool(ENV_LOSSLESS_PERFORMANCE_MODE, runtime.performance_mode);
      set_double(ENV_LOSSLESS_RESOLUTION, runtime.resolution_scale_factor);
      set_string(ENV_LOSSLESS_FRAMEGEN_MODE, runtime.frame_generation);
      set_string(ENV_LOSSLESS_LSFG3_MODE, runtime.lsfg3_mode);
      set_string(ENV_LOSSLESS_SCALING_TYPE, runtime.scaling_type);
      set_int(ENV_LOSSLESS_SHARPNESS, runtime.sharpness);
      set_int(ENV_LOSSLESS_LS1_SHARPNESS, runtime.ls1_sharpness);
      set_string(ENV_LOSSLESS_ANIME4K_TYPE, runtime.anime4k_type);
      set_bool(ENV_LOSSLESS_ANIME4K_VRS, runtime.anime4k_vrs);
    } else {
      _env["SUNSHINE_LOSSLESS_SCALING_FRAMEGEN"] = "";
      _env["SUNSHINE_LOSSLESS_SCALING_TARGET_FPS"] = "";
      _env["SUNSHINE_LOSSLESS_SCALING_RTSS_LIMIT"] = "";
      clear_lossless_runtime_env();
    }

    if (!_app.output.empty() && _app.output != "null"sv) {
#ifdef _WIN32
      // fopen() interprets the filename as an ANSI string on Windows, so we must convert it
      // to UTF-16 and use the wchar_t variants for proper Unicode log file path support.
      auto woutput = platf::from_utf8(_app.output);

      // Use _SH_DENYNO to allow us to open this log file again for writing even if it is
      // still open from a previous execution. This is required to handle the case of a
      // detached process executing again while the previous process is still running.
      _pipe.reset(_wfsopen(woutput.c_str(), L"a", _SH_DENYNO));
#else
      _pipe.reset(fopen(_app.output.c_str(), "a"));
#endif
    }

    std::error_code ec;
    // Executed when returning from function
    auto fg = util::fail_guard([&]() {
      terminate();
    });

    for (; _app_prep_it != std::end(_app.prep_cmds); ++_app_prep_it) {
      auto &cmd = *_app_prep_it;

      // Skip empty commands
      if (cmd.do_cmd.empty()) {
        continue;
      }

      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(cmd.do_cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Executing Do Cmd: ["sv << cmd.do_cmd << ']';
      auto child = platf::run_command(cmd.elevated, true, cmd.do_cmd, working_dir, _env, _pipe.get(), ec, nullptr);

      if (ec) {
        BOOST_LOG(error) << "Couldn't run ["sv << cmd.do_cmd << "]: System: "sv << ec.message();
        // We don't want any prep commands failing launch of the desktop.
        // This is to prevent the issue where users reboot their PC and need to log in with Sunshine.
        // permission_denied is typically returned when the user impersonation fails, which can happen when user is not signed in yet.
        if (!(_app.cmd.empty() && ec == std::errc::permission_denied)) {
          return -1;
        }
      }

      child.wait();
      auto ret = child.exit_code();
      if (ret != 0 && ec != std::errc::permission_denied) {
        BOOST_LOG(error) << '[' << cmd.do_cmd << "] failed with code ["sv << ret << ']';
        return -1;
      }
    }

    for (auto &cmd : _app.detached) {
      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
#ifdef _WIN32
      if (should_start_lossless_support && !lossless_monitor_started && lossless_baseline_pids.empty()) {
        lossless_baseline_pids = capture_process_baseline_for_lossless();
      }
      if (should_start_lossless_support && !lossless_monitor_started && lossless_install_dir_hint.empty()) {
        try {
          lossless_install_dir_hint = platf::dxgi::wide_to_utf8(working_dir.wstring());
        } catch (...) {
          lossless_install_dir_hint.clear();
        }
      }
#endif
      BOOST_LOG(info) << "Spawning ["sv << cmd << "] in ["sv << working_dir << ']';
      auto child = platf::run_command(_app.elevated, true, cmd, working_dir, _env, _pipe.get(), ec, nullptr);
#ifdef _WIN32
      DWORD detached_pid = 0;
      if (!ec) {
        try {
          detached_pid = static_cast<DWORD>(child.id());
        } catch (...) {
          detached_pid = 0;
        }
      }
#endif
      if (ec) {
        BOOST_LOG(warning) << "Couldn't spawn ["sv << cmd << "]: System: "sv << ec.message();
      } else {
        child.detach();
#ifdef _WIN32
        if (should_start_lossless_support && !lossless_monitor_started) {
          if (lossless_baseline_pids.empty()) {
            lossless_baseline_pids = capture_process_baseline_for_lossless();
          }
          start_lossless_scaling_support(std::move(lossless_baseline_pids), lossless_metadata, std::move(lossless_install_dir_hint), detached_pid);
          lossless_monitor_started = true;
        }
#endif
      }
    }

    // Playnite-backed apps: invoke via Playnite and treat as placebo (lifetime managed via Playnite status)
#ifdef _WIN32
    if (!_app.playnite_id.empty() && _app.cmd.empty()) {
      // Auto-update Playnite plugin if an update is available
      try {
        std::string installed_ver, packaged_ver;
        bool have_installed = platf::playnite::get_installed_plugin_version(installed_ver);
        bool have_packaged = platf::playnite::get_packaged_plugin_version(packaged_ver);

        if (have_installed && have_packaged) {
          // Simple version comparison: compare as strings (works for semantic versioning)
          auto normalize_ver = [](std::string s) -> std::string {
            // Strip leading 'v' if present
            if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) {
              s = s.substr(1);
            }
            // Remove whitespace
            s.erase(std::remove_if(s.begin(), s.end(), ::isspace), s.end());
            return s;
          };

          std::string installed_normalized = normalize_ver(installed_ver);
          std::string packaged_normalized = normalize_ver(packaged_ver);

          if (installed_normalized < packaged_normalized) {
            BOOST_LOG(info) << "Playnite plugin update available (" << installed_ver
                            << " -> " << packaged_ver << "), auto-updating before launch";
            std::string install_error;
            if (platf::playnite::install_plugin(install_error)) {
              BOOST_LOG(info) << "Playnite plugin auto-update succeeded";
            } else {
              BOOST_LOG(warning) << "Playnite plugin auto-update failed: " << install_error
                                 << " (continuing with game launch)";
            }
          }
        }
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Exception during Playnite plugin auto-update check: " << e.what()
                           << " (continuing with game launch)";
      } catch (...) {
        BOOST_LOG(warning) << "Unknown exception during Playnite plugin auto-update check (continuing with game launch)";
      }

      BOOST_LOG(info) << "Launching Playnite game via helper, id=" << _app.playnite_id;
      bool launched = false;
      // Resolve launcher alongside sunshine.exe: tools\\playnite-launcher.exe
      try {
        WCHAR exePathW[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePathW, ARRAYSIZE(exePathW));
        std::filesystem::path exeDir = std::filesystem::path(exePathW).parent_path();
        std::filesystem::path launcher = exeDir / L"tools" / L"playnite-launcher.exe";
        std::string lpath = launcher.string();
        std::string cmd = std::string("\"") + lpath + "\" --game-id " + _app.playnite_id;
        // Pass graceful-exit timeout to launcher for cleanup behavior
        try {
          int exit_to = (int) std::max<std::int64_t>(0, _app.exit_timeout.count());
          if (exit_to > 0) {
            cmd += std::string(" --exit-timeout ") + std::to_string(exit_to);
          }
        } catch (...) {}
        // Pass focus attempts from config so the helper can try to bring Playnite/game to foreground
        try {
          if (config::playnite.focus_attempts > 0) {
            cmd += std::string(" --focus-attempts ") + std::to_string(config::playnite.focus_attempts);
          }
          if (config::playnite.focus_timeout_secs > 0) {
            cmd += std::string(" --focus-timeout ") + std::to_string(config::playnite.focus_timeout_secs);
          }
          if (config::playnite.focus_exit_on_first) {
            cmd += std::string(" --focus-exit-on-first");
          }
        } catch (...) {}
        std::error_code fec;
        boost::filesystem::path wd;  // empty wd
        _process = platf::run_command(false, true, cmd, wd, _env, _pipe.get(), fec, &_process_group);
        if (fec) {
          BOOST_LOG(warning) << "Playnite helper launch failed: "sv << fec.message() << "; attempting URI fallback"sv;
        } else {
          BOOST_LOG(info) << "Playnite helper launched and is being monitored";
          try {
            auto pid = static_cast<uint32_t>(_process.id());
            if (!platf::playnite::announce_launcher(pid, _app.playnite_id)) {
              BOOST_LOG(debug) << "Playnite helper: announce_launcher reported inactive IPC";
            }
          } catch (...) {
          }
          launched = true;
        }
      } catch (...) {
        launched = false;
      }
      if (!launched) {
        // Best-effort fallback using Playnite URI protocol
        std::string uri = std::string("playnite://playnite/start/") + _app.playnite_id;
        std::error_code fec;
        boost::filesystem::path wd;  // empty working dir as lvalue
        auto child = platf::run_command(false, true, std::string("cmd /c start \"\" \"") + uri + "\"", wd, _env, _pipe.get(), fec, nullptr);
        if (fec) {
          BOOST_LOG(warning) << "Playnite URI launch failed: "sv << fec.message();
        } else {
          BOOST_LOG(info) << "Playnite URI launch started";
          child.detach();
          launched = true;
        }
      }
      if (!launched) {
        BOOST_LOG(error) << "Failed to launch Playnite game."sv;
        return -1;
      }
      // Track the helper process; when it exits, Sunshine will terminate the stream automatically
      placebo = false;
    } else
#endif
#ifdef _WIN32
      if (_app.playnite_fullscreen) {
      BOOST_LOG(info) << "Launching Playnite in fullscreen via helper";
      bool launched = false;
      try {
        WCHAR exePathW[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePathW, ARRAYSIZE(exePathW));
        std::filesystem::path exeDir = std::filesystem::path(exePathW).parent_path();
        std::filesystem::path launcher = exeDir / L"tools" / L"playnite-launcher.exe";
        std::string lpath = launcher.string();
        std::string cmd = std::string("\"") + lpath + "\" --fullscreen";
        try {
          if (config::playnite.focus_attempts > 0) {
            cmd += std::string(" --focus-attempts ") + std::to_string(config::playnite.focus_attempts);
          }
          if (config::playnite.focus_timeout_secs > 0) {
            cmd += std::string(" --focus-timeout ") + std::to_string(config::playnite.focus_timeout_secs);
          }
          if (config::playnite.focus_exit_on_first) {
            cmd += std::string(" --focus-exit-on-first");
          }
        } catch (...) {}
        std::error_code fec;
        boost::filesystem::path wd;  // empty wd
        _process = platf::run_command(false, true, cmd, wd, _env, _pipe.get(), fec, &_process_group);
        if (fec) {
          BOOST_LOG(warning) << "Playnite fullscreen helper launch failed: "sv << fec.message();
        } else {
          BOOST_LOG(info) << "Playnite fullscreen helper launched";
          try {
            auto pid = static_cast<uint32_t>(_process.id());
            if (!platf::playnite::announce_launcher(pid, std::string())) {
              BOOST_LOG(debug) << "Playnite helper (fullscreen): announce_launcher reported inactive IPC";
            }
          } catch (...) {
          }
          launched = true;
        }
      } catch (...) {
        launched = false;
      }
      if (!launched) {
        BOOST_LOG(error) << "Failed to launch Playnite fullscreen."sv;
        return -1;
      }
      placebo = false;
    } else
#endif
      if (_app.cmd.empty()) {
      BOOST_LOG(info) << "Executing [Desktop]"sv;
      BOOST_LOG(info) << "Playnite launch path complete; treating app as placebo (status-driven).";
      placebo = true;
    } else {
      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(_app.cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
#ifdef _WIN32
      if (should_start_lossless_support && !lossless_monitor_started && lossless_baseline_pids.empty()) {
        lossless_baseline_pids = capture_process_baseline_for_lossless();
      }
      if (should_start_lossless_support && !lossless_monitor_started && lossless_install_dir_hint.empty()) {
        try {
          lossless_install_dir_hint = platf::dxgi::wide_to_utf8(working_dir.wstring());
        } catch (...) {
          lossless_install_dir_hint.clear();
        }
      }
#endif
      BOOST_LOG(info) << "Executing: ["sv << _app.cmd << "] in ["sv << working_dir << ']';
      _process = platf::run_command(_app.elevated, true, _app.cmd, working_dir, _env, _pipe.get(), ec, &_process_group);
      if (ec) {
        BOOST_LOG(warning) << "Couldn't run ["sv << _app.cmd << "]: System: "sv << ec.message();
        return -1;
      }
    }

#ifdef _WIN32
    if (should_start_lossless_support && !lossless_monitor_started) {
      if (lossless_baseline_pids.empty()) {
        lossless_baseline_pids = capture_process_baseline_for_lossless();
      }
      if (lossless_install_dir_hint.empty() && !_app.working_dir.empty()) {
        lossless_install_dir_hint = _app.working_dir;
      }
      if (lossless_baseline_pids.empty()) {
        // still proceed; detection handles empty baseline
      }
      DWORD candidate_pid = 0;
      if (_process) {
        try {
          candidate_pid = static_cast<DWORD>(_process.id());
        } catch (...) {
          candidate_pid = 0;
        }
      }
      start_lossless_scaling_support(std::move(lossless_baseline_pids), lossless_metadata, std::move(lossless_install_dir_hint), candidate_pid);
      lossless_monitor_started = true;
    }
#endif

    _app_launch_time = std::chrono::steady_clock::now();

    fg.disable();

    return 0;
  }

  int proc_t::running() {
#ifndef _WIN32
    // On POSIX OSes, we must periodically wait for our children to avoid
    // them becoming zombies. This must be synchronized carefully with
    // calls to bp::wait() and platf::process_group_running() which both
    // invoke waitpid() under the hood.
    auto reaper = util::fail_guard([]() {
      while (waitpid(-1, nullptr, WNOHANG) > 0);
    });
#endif

    if (placebo) {
      return _app_id;
    } else if (_app.wait_all && _process_group && platf::process_group_running((std::uintptr_t) _process_group.native_handle())) {
      // The app is still running if any process in the group is still running
      return _app_id;
    } else if (_process.running()) {
      // The app is still running only if the initial process launched is still running
      return _app_id;
    } else if (_app.auto_detach && _process.native_exit_code() == 0 &&
               std::chrono::steady_clock::now() - _app_launch_time < 5s) {
      BOOST_LOG(info) << "App exited gracefully within 5 seconds of launch. Treating the app as a detached command."sv;
      BOOST_LOG(info) << "Adjust this behavior in the Applications tab or apps.json if this is not what you want."sv;
      BOOST_LOG(info) << "Playnite launch path complete; treating app as placebo (status-driven).";
      placebo = true;
      return _app_id;
    }

    // Perform cleanup actions now if needed
    if (_process) {
      BOOST_LOG(info) << "[running] _process.running() is false; calling terminate(). App exited with code ["sv << _process.native_exit_code() << "] for app '" << _app.name << "' (id=" << _app_id << ")";
      terminate();
    }

    return 0;
  }

  void proc_t::terminate() {
    std::error_code ec;
    placebo = false;
#ifdef _WIN32
    stop_lossless_scaling_support();
#endif
    // For Playnite-managed apps, request a graceful stop via Playnite first
#ifdef _WIN32
    std::chrono::seconds remaining_timeout = _app.exit_timeout;
    if (!_app.playnite_id.empty()) {
      try {
        // Ask Playnite to stop the game; then wait up to exit-timeout to let it close
        platf::playnite::stop_game(_app.playnite_id);
        while (remaining_timeout.count() > 0 && _process_group && platf::process_group_running((std::uintptr_t) _process_group.native_handle())) {
          std::this_thread::sleep_for(1s);
          remaining_timeout -= 1s;
        }
      } catch (...) {}
    }
#endif
    // Regardless, ensure process group is terminated (graceful then forceful with remaining timeout)
    terminate_process_group(_process, _process_group, remaining_timeout);
    _process = boost::process::v1::child();
    _process_group = boost::process::v1::group();

    for (; _app_prep_it != _app_prep_begin; --_app_prep_it) {
      auto &cmd = *(_app_prep_it - 1);

      if (cmd.undo_cmd.empty()) {
        continue;
      }

      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(cmd.undo_cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Executing Undo Cmd: ["sv << cmd.undo_cmd << ']';
      auto child = platf::run_command(cmd.elevated, true, cmd.undo_cmd, working_dir, _env, _pipe.get(), ec, nullptr);

      if (ec) {
        BOOST_LOG(warning) << "System: "sv << ec.message();
      }

      child.wait();
      auto ret = child.exit_code();

      if (ret != 0) {
        BOOST_LOG(warning) << "Return code ["sv << ret << ']';
      }
    }

    _pipe.reset();

#ifdef _WIN32
    if (_virtual_display_active) {
      if (!VDISPLAY::removeVirtualDisplay(_virtual_display_guid)) {
        BOOST_LOG(warning) << "Failed to remove virtual display.";
      } else {
        BOOST_LOG(info) << "Virtual display removed.";
      }
      std::memset(&_virtual_display_guid, 0, sizeof(_virtual_display_guid));
      _virtual_display_active = false;
    }
#endif

    bool has_run = _app_id > 0;

    // Only show the Stopped notification if we actually have an app to stop
    // Since terminate() is always run when a new app has started
    if (proc::proc.get_last_run_app_name().length() > 0 && has_run) {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_stopped(proc::proc.get_last_run_app_name());
#endif

      if (config::video.dd.config_revert_on_disconnect) {
        display_helper_integration::revert();
      }
    }

    _app_id = -1;
  }

  std::vector<ctx_t> proc_t::get_apps() const {
    std::scoped_lock lk(_apps_mutex);
    return _apps;
  }

  // Gets application image from application list.
  // Returns image from assets directory if found there.
  // Returns default image if image configuration is not set.
  // Returns http content-type header compatible image type.
  std::string proc_t::get_app_image(int app_id) {
    std::scoped_lock lk(_apps_mutex);
    auto iter = std::find_if(_apps.begin(), _apps.end(), [&app_id](const auto app) {
      return app.id == std::to_string(app_id);
    });
    auto app_image_path = iter == _apps.end() ? std::string() : iter->image_path;

    return validate_app_image_path(app_image_path);
  }

  std::string proc_t::get_last_run_app_name() {
    return _app.name;
  }

  bool proc_t::last_run_app_frame_gen_limiter_fix() const {
    return _app.frame_gen_limiter_fix;
  }

  proc_t::~proc_t() {
    // It's not safe to call terminate() here because our proc_t is a static variable
    // that may be destroyed after the Boost loggers have been destroyed. Instead,
    // we return a deinit_t to main() to handle termination when we're exiting.
    // Once we reach this point here, termination must have already happened.
    assert(!placebo);
    assert(!_process.running());
  }

  std::string_view::iterator find_match(std::string_view::iterator begin, std::string_view::iterator end) {
    int stack = 0;

    --begin;
    do {
      ++begin;
      switch (*begin) {
        case '(':
          ++stack;
          break;
        case ')':
          --stack;
      }
    } while (begin != end && stack != 0);

    if (begin == end) {
      throw std::out_of_range("Missing closing bracket \')\'");
    }
    return begin;
  }

  std::string parse_env_val(boost::process::v1::native_environment &env, const std::string_view &val_raw) {
    auto pos = std::begin(val_raw);
    auto dollar = std::find(pos, std::end(val_raw), '$');

    std::stringstream ss;

    while (dollar != std::end(val_raw)) {
      auto next = dollar + 1;
      if (next != std::end(val_raw)) {
        switch (*next) {
          case '(':
            {
              ss.write(pos, (dollar - pos));
              auto var_begin = next + 1;
              auto var_end = find_match(next, std::end(val_raw));
              auto var_name = std::string {var_begin, var_end};

#ifdef _WIN32
              // Windows treats environment variable names in a case-insensitive manner,
              // so we look for a case-insensitive match here. This is critical for
              // correctly appending to PATH on Windows.
              auto itr = std::find_if(env.cbegin(), env.cend(), [&](const auto &e) {
                return boost::iequals(e.get_name(), var_name);
              });
              if (itr != env.cend()) {
                // Use an existing case-insensitive match
                var_name = itr->get_name();
              }
#endif

              ss << env[var_name].to_string();

              pos = var_end + 1;
              next = var_end;

              break;
            }
          case '$':
            ss.write(pos, (next - pos));
            pos = next + 1;
            ++next;
            break;
        }

        dollar = std::find(next, std::end(val_raw), '$');
      } else {
        BOOST_LOG(info) << "Playnite URI launch started";
        dollar = next;
      }
    }

    ss.write(pos, (dollar - pos));

    return ss.str();
  }

  std::string validate_app_image_path(std::string app_image_path) {
    if (app_image_path.empty()) {
      return DEFAULT_APP_IMAGE_PATH;
    }

    // get the image extension and convert it to lowercase
    auto image_extension = std::filesystem::path(app_image_path).extension().string();
    boost::to_lower(image_extension);

    // return the default box image if extension is not "png"
    if (image_extension != ".png") {
      return DEFAULT_APP_IMAGE_PATH;
    }

    // check if image is in assets directory
    auto full_image_path = std::filesystem::path(SUNSHINE_ASSETS_DIR) / app_image_path;
    if (std::filesystem::exists(full_image_path)) {
      return full_image_path.string();
    } else if (app_image_path == "./assets/steam.png") {
      // handle old default steam image definition
      return SUNSHINE_ASSETS_DIR "/steam.png";
    }

    // check if specified image exists
    std::error_code code;
    if (!std::filesystem::exists(app_image_path, code)) {
      // return default box image if image does not exist
      BOOST_LOG(warning) << "Couldn't find app image at path ["sv << app_image_path << ']';
      return DEFAULT_APP_IMAGE_PATH;
    }

    // image is a png, and not in assets directory
    // return only "content-type" http header compatible image type
    return app_image_path;
  }

  std::optional<std::string> calculate_sha256(const std::string &filename) {
    crypto::md_ctx_t ctx {EVP_MD_CTX_create()};
    if (!ctx) {
      return std::nullopt;
    }

    if (!EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr)) {
      return std::nullopt;
    }

    // Read file and update calculated SHA
    char buf[1024 * 16];
    std::ifstream file(filename, std::ifstream::binary);
    while (file.good()) {
      file.read(buf, sizeof(buf));
      if (!EVP_DigestUpdate(ctx.get(), buf, file.gcount())) {
        return std::nullopt;
      }
    }
    file.close();

    unsigned char result[SHA256_DIGEST_LENGTH];
    if (!EVP_DigestFinal_ex(ctx.get(), result, nullptr)) {
      return std::nullopt;
    }

    // Transform byte-array to string
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (const auto &byte : result) {
      ss << std::setw(2) << (int) byte;
    }
    return ss.str();
  }

  uint32_t calculate_crc32(const std::string &input) {
    boost::crc_32_type result;
    result.process_bytes(input.data(), input.length());
    return result.checksum();
  }

  std::tuple<std::string, std::string> calculate_app_id(const std::string &app_name, std::string app_image_path, int index) {
    // Generate id by hashing name with image data if present
    std::vector<std::string> to_hash;
    to_hash.push_back(app_name);
    auto file_path = validate_app_image_path(app_image_path);
    if (file_path != DEFAULT_APP_IMAGE_PATH) {
      auto file_hash = calculate_sha256(file_path);
      if (file_hash) {
        to_hash.push_back(file_hash.value());
      } else {
        BOOST_LOG(info) << "Playnite URI launch started";
        // Fallback to just hashing image path
        to_hash.push_back(file_path);
      }
    }

    // Create combined strings for hash
    std::stringstream ss;
    for_each(to_hash.begin(), to_hash.end(), [&ss](const std::string &s) {
      ss << s;
    });
    auto input_no_index = ss.str();
    ss << index;
    auto input_with_index = ss.str();

    // CRC32 then truncate to signed 32-bit range due to client limitations
    auto id_no_index = std::to_string(abs((int32_t) calculate_crc32(input_no_index)));
    auto id_with_index = std::to_string(abs((int32_t) calculate_crc32(input_with_index)));

    return std::make_tuple(id_no_index, id_with_index);
  }

  std::optional<proc::proc_t> parse(const std::string &file_name) {
    pt::ptree tree;

    try {
      pt::read_json(file_name, tree);

      auto &apps_node = tree.get_child("apps"s);
      auto &env_vars = tree.get_child("env"s);

      auto this_env = boost::this_process::environment();

      for (auto &[name, val] : env_vars) {
        this_env[name] = parse_env_val(this_env, val.get_value<std::string>());
      }

      std::set<std::string> ids;
      std::vector<proc::ctx_t> apps;
      int i = 0;
      for (auto &[_, app_node] : apps_node) {
        proc::ctx_t ctx;

        auto prep_nodes_opt = app_node.get_child_optional("prep-cmd"s);
        auto detached_nodes_opt = app_node.get_child_optional("detached"s);
        auto exclude_global_prep = app_node.get_optional<bool>("exclude-global-prep-cmd"s);
        auto output = app_node.get_optional<std::string>("output"s);
        auto name = parse_env_val(this_env, app_node.get<std::string>("name"s));
        auto cmd = app_node.get_optional<std::string>("cmd"s);
        auto image_path = app_node.get_optional<std::string>("image-path"s);
        auto working_dir = app_node.get_optional<std::string>("working-dir"s);
        auto playnite_id = app_node.get_optional<std::string>("playnite-id"s);
        auto elevated = app_node.get_optional<bool>("elevated"s);
        auto auto_detach = app_node.get_optional<bool>("auto-detach"s);
        auto wait_all = app_node.get_optional<bool>("wait-all"s);
        auto exit_timeout = app_node.get_optional<int>("exit-timeout"s);
        auto gen1_framegen_fix = app_node.get_optional<bool>("gen1-framegen-fix"s);
        auto gen2_framegen_fix = app_node.get_optional<bool>("gen2-framegen-fix"s);
        // Backward compatibility: check old field name if new one not present
        if (!gen1_framegen_fix) {
          gen1_framegen_fix = app_node.get_optional<bool>("dlss-framegen-capture-fix"s);
        }
        auto lossless_scaling_framegen = app_node.get_optional<bool>("lossless-scaling-framegen"s);
        auto frame_generation_provider = app_node.get_optional<std::string>("frame-generation-provider"s);

        ctx.lossless_scaling_framegen = lossless_scaling_framegen.value_or(false);
        ctx.frame_generation_provider = frame_generation_provider ? normalize_frame_generation_provider(*frame_generation_provider) : "lossless-scaling";
        ctx.lossless_scaling_target_fps.reset();
        ctx.lossless_scaling_rtss_limit.reset();
        if (auto ls_target = app_node.get_optional<int>("lossless-scaling-target-fps"s)) {
          if (*ls_target > 0) {
            ctx.lossless_scaling_target_fps = *ls_target;
          }
        }
        if (auto ls_rtss = app_node.get_optional<int>("lossless-scaling-rtss-limit"s)) {
          if (*ls_rtss > 0) {
            ctx.lossless_scaling_rtss_limit = *ls_rtss;
          }
        }

        ctx.lossless_scaling_profile = LOSSLESS_PROFILE_CUSTOM;
        if (auto profile = app_node.get_optional<std::string>("lossless-scaling-profile"s)) {
          if (boost::iequals(*profile, LOSSLESS_PROFILE_RECOMMENDED)) {
            ctx.lossless_scaling_profile = LOSSLESS_PROFILE_RECOMMENDED;
          }
        }
        if (auto recommended_node = app_node.get_child_optional("lossless-scaling-recommended"s)) {
          populate_lossless_overrides(*recommended_node, ctx.lossless_scaling_recommended);
        }
        if (auto custom_node = app_node.get_child_optional("lossless-scaling-custom"s)) {
          populate_lossless_overrides(*custom_node, ctx.lossless_scaling_custom);
        }

        std::vector<proc::cmd_t> prep_cmds;
        if (!exclude_global_prep.value_or(false)) {
          prep_cmds.reserve(config::sunshine.prep_cmds.size());
          for (auto &prep_cmd : config::sunshine.prep_cmds) {
            auto do_cmd = parse_env_val(this_env, prep_cmd.do_cmd);
            auto undo_cmd = parse_env_val(this_env, prep_cmd.undo_cmd);

            prep_cmds.emplace_back(
              std::move(do_cmd),
              std::move(undo_cmd),
              std::move(prep_cmd.elevated)
            );
          }
        }

        if (prep_nodes_opt) {
          auto &prep_nodes = *prep_nodes_opt;

          prep_cmds.reserve(prep_cmds.size() + prep_nodes.size());
          for (auto &[_, prep_node] : prep_nodes) {
            auto do_cmd = prep_node.get_optional<std::string>("do"s);
            auto undo_cmd = prep_node.get_optional<std::string>("undo"s);
            auto elevated = prep_node.get_optional<bool>("elevated");

            prep_cmds.emplace_back(
              parse_env_val(this_env, do_cmd.value_or("")),
              parse_env_val(this_env, undo_cmd.value_or("")),
              std::move(elevated.value_or(false))
            );
          }
        }

        std::vector<std::string> detached;
        if (detached_nodes_opt) {
          auto &detached_nodes = *detached_nodes_opt;

          detached.reserve(detached_nodes.size());
          for (auto &[_, detached_val] : detached_nodes) {
            detached.emplace_back(parse_env_val(this_env, detached_val.get_value<std::string>()));
          }
        }

        if (output) {
          ctx.output = parse_env_val(this_env, *output);
        }

        if (cmd) {
          ctx.cmd = parse_env_val(this_env, *cmd);
        }

        if (working_dir) {
          ctx.working_dir = parse_env_val(this_env, *working_dir);
#ifdef _WIN32
          // The working directory, unlike the command itself, should not be quoted
          // when it contains spaces. Unlike POSIX, Windows forbids quotes in paths,
          // so we can safely strip them all out here to avoid confusing the user.
          boost::erase_all(ctx.working_dir, "\"");
#endif
        }

        if (image_path) {
          ctx.image_path = parse_env_val(this_env, *image_path);
        }

        if (playnite_id) {
          ctx.playnite_id = parse_env_val(this_env, *playnite_id);
        }
        // Optional Playnite fullscreen launcher flag
        try {
          auto pfs = app_node.get_optional<bool>("playnite-fullscreen"s);
          ctx.playnite_fullscreen = pfs.value_or(false);
        } catch (...) {
          ctx.playnite_fullscreen = false;
        }

        try {
          auto fgfix = app_node.get_optional<bool>("frame-gen-limiter-fix"s);
          ctx.frame_gen_limiter_fix = fgfix.value_or(false);
        } catch (...) {
          ctx.frame_gen_limiter_fix = false;
        }

        ctx.elevated = elevated.value_or(false);
        ctx.virtual_screen = app_node.get_optional<bool>("virtual-screen"s).value_or(false);
        ctx.auto_detach = auto_detach.value_or(true);
        ctx.wait_all = wait_all.value_or(true);
        // Default graceful-exit timeout: 10s (Playnite-managed apps are written with this value)
        ctx.exit_timeout = std::chrono::seconds {exit_timeout.value_or(10)};
        ctx.gen1_framegen_fix = gen1_framegen_fix.value_or(false);
        ctx.gen2_framegen_fix = gen2_framegen_fix.value_or(false);
        if (!ctx.lossless_scaling_framegen) {
          ctx.lossless_scaling_target_fps.reset();
          ctx.lossless_scaling_rtss_limit.reset();
        }

        auto possible_ids = calculate_app_id(name, ctx.image_path, i++);
        if (ids.count(std::get<0>(possible_ids)) == 0) {
          // Avoid using index to generate id if possible
          ctx.id = std::get<0>(possible_ids);
        } else {
          BOOST_LOG(info) << "Playnite URI launch started";
          // Fallback to include index on collision
          ctx.id = std::get<1>(possible_ids);
        }
        ids.insert(ctx.id);

        ctx.name = std::move(name);
        ctx.prep_cmds = std::move(prep_cmds);
        ctx.detached = std::move(detached);

        apps.emplace_back(std::move(ctx));
      }

      return std::optional<proc::proc_t>(std::in_place, std::move(this_env), std::move(apps));
    } catch (std::exception &e) {
      BOOST_LOG(error) << e.what();
    }

    return std::nullopt;
  }

  void refresh(const std::string &file_name) {
    auto proc_opt = proc::parse(file_name);

    if (!proc_opt) {
      return;
    }

    // If an app is currently running, do not replace the entire proc_t instance.
    // Replacing it would drop tracking state and cause the active stream loop
    // to think no app is running, prematurely terminating the session.
    // Instead, update only the applications list to reflect the latest config.
    if (proc.running() > 0) {
      // Move the parsed apps list and environment into the existing proc instance
      // Use proc.update_apps(...) which safely replaces the app list and env
      proc.update_apps(proc_opt->release_apps(), proc_opt->release_env());

    } else {
      // No app running: safe to refresh full state (env + apps)
      proc = std::move(*proc_opt);
    }
  }

  void proc_t::update_apps(std::vector<ctx_t> &&apps, boost::process::v1::environment &&env) {
    // Replace app list and environment while keeping current running app intact
    {
      std::scoped_lock lk(_apps_mutex);
      _apps = std::move(apps);
      _env = std::move(env);
    }
  }

  std::vector<ctx_t> proc_t::release_apps() {
    return std::move(_apps);
  }

  boost::process::v1::environment proc_t::release_env() {
    return std::move(_env);
  }
}  // namespace proc
