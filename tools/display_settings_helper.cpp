/**
 * @file tools/display_settings_helper.cpp
 * @brief Detached helper to apply/revert Windows display settings via IPC.
 */

#ifdef _WIN32

  // standard
  #include <algorithm>
  #include <atomic>
  #include <chrono>
  #include <condition_variable>
  #include <cstdint>
  #include <cctype>
  #include <array>
  #include <cstdio>
  #include <cstdlib>
  #include <cstring>
  #include <cwchar>
  #include <cmath>
  #include <filesystem>
  #include <functional>
  #include <memory>
  #include <mutex>
  #include <optional>
  #include <set>
  #include <span>
  #include <stop_token>
  #include <string>
  #include <thread>
  #include <unordered_map>
  #include <utility>
  #include <vector>

// third-party (libdisplaydevice)
  #include "src/logging.h"
  #include "src/platform/windows/ipc/pipes.h"

  #include <display_device/json.h>
  #include <display_device/logging.h>
  #include <display_device/noop_audio_context.h>
  #include <display_device/noop_settings_persistence.h>
  #include <display_device/windows/settings_manager.h>
  #include <display_device/windows/settings_utils.h>
  #include <display_device/windows/win_api_layer.h>
  #include <display_device/windows/win_api_utils.h>
  #include <display_device/windows/win_display_device.h>
  #include <nlohmann/json.hpp>

  // platform
  #ifndef SECURITY_WIN32
    #define SECURITY_WIN32
  #endif

  #include <comdef.h>
  #include <dbt.h>
  #include <devguid.h>
  #include <lmcons.h>
  #include <powrprof.h>
  #include <secext.h>
  #include <shlobj.h>
  #include <taskschd.h>
  #include <windows.h>
  #include <winerror.h>
  #include <wtsapi32.h>

namespace {
  static const GUID kMonitorInterfaceGuid = {0xe6f07b5f, 0xee97, 0x4a90, {0xb0, 0x76, 0x33, 0xf5, 0x7b, 0xf4, 0xea, 0xa7}};
}

using namespace std::chrono_literals;
namespace bl = boost::log;

namespace {

  constexpr DWORD kInvalidSessionId = static_cast<DWORD>(-1);

  std::wstring query_session_account(DWORD session_id) {
    if (session_id == kInvalidSessionId) {
      return {};
    }

    auto fetch_session_string = [&](WTS_INFO_CLASS info_class) -> std::wstring {
      LPWSTR buffer = nullptr;
      DWORD bytes = 0;
      if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session_id, info_class, &buffer, &bytes)) {
        return {};
      }

      std::wstring value;
      if (buffer && *buffer != L'\0') {
        value.assign(buffer);
      }

      if (buffer) {
        WTSFreeMemory(buffer);
      }

      return value;
    };

    std::wstring user = fetch_session_string(WTSUserName);
    if (user.empty()) {
      return {};
    }

    std::wstring domain = fetch_session_string(WTSDomainName);
    if (!domain.empty()) {
      return domain + L"\\" + user;
    }

    return user;
  }

  std::wstring build_restore_task_name(const std::wstring &username) {
    return L"VibeshineDisplayRestore";
  }

  // Trigger a more robust Explorer/shell refresh so that desktop/taskbar icons
  // and other shell-controlled UI elements pick up DPI/metrics changes that
  // can occur after monitor topology/primary swaps. Avoids wrong-sized icons
  // without restarting Explorer.
  inline void refresh_shell_after_display_change() {
    // 1) Ask the shell to refresh associations/images and flush notifications.
    //    SHCNF_FLUSHNOWAIT avoids blocking if the shell is busy.
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);

    // 2) Force a reload of system icons. This does not change user settings
    //    but prompts Explorer to re-query default icons and sizes.
    SystemParametersInfoW(SPI_SETICONS, 0, nullptr, SPIF_SENDCHANGE);

    // Helper to safely broadcast a message with a short timeout so we don't hang
    // if any app stops responding.
    auto broadcast = [](UINT msg, WPARAM wParam, LPARAM lParam) {
      DWORD_PTR result = 0;
      SendMessageTimeoutW(HWND_BROADCAST, msg, wParam, lParam, SMTO_ABORTIFHUNG | SMTO_NORMAL, 100, &result);
    };

    // 3) Broadcast targeted setting changes that commonly trigger Explorer to
    //    refresh icon metrics and shell state.
    static const wchar_t kShellState[] = L"ShellState";
    static const wchar_t kIconMetrics[] = L"IconMetrics";
    broadcast(WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(kShellState));
    broadcast(WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(kIconMetrics));

    // 4) Broadcast a display change with current depth and resolution to nudge
    //    windows that cache DPI-dependent icon resources.
    HDC hdc = GetDC(nullptr);
    int bpp = 32;
    if (hdc) {
      const int planes = GetDeviceCaps(hdc, PLANES);
      const int bits = GetDeviceCaps(hdc, BITSPIXEL);
      if (planes > 0 && bits > 0) {
        bpp = planes * bits;
      }
      ReleaseDC(nullptr, hdc);
    }
    const LPARAM res = MAKELPARAM(GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    broadcast(WM_DISPLAYCHANGE, static_cast<WPARAM>(bpp), res);
  }

  // Simple framed protocol: [u32 length][u8 type][payload...]
  enum class MsgType : uint8_t {
    Apply = 1,  // payload: JSON SingleDisplayConfiguration
    Revert = 2,  // no payload
    Reset = 3,  // clear persistence (best-effort)
    ExportGolden = 4,  // no payload; export current settings snapshot as golden restore
    ApplyResult = 6,  // payload: [u8 success][optional message...]
    Disarm = 7,  // cancel any pending restore requests/watchdogs
    SnapshotCurrent = 8,  // snapshot current session state (rotate current->previous) without applying
    Ping = 0xFE,  // no payload, reply with Pong
    Stop = 0xFF  // no payload, terminate process
  };

  inline void send_framed_content(platf::dxgi::AsyncNamedPipe &pipe, MsgType type, std::span<const uint8_t> payload = {}) {
    std::vector<uint8_t> out;
    out.reserve(1 + payload.size());
    out.push_back(static_cast<uint8_t>(type));
    out.insert(out.end(), payload.begin(), payload.end());
    pipe.send(out);
  }

  // Wrap SettingsManager for easy use in this helper
  class DisplayController {
  public:
    DisplayController() = default;

    static std::string ascii_lower(std::string s) {
      for (char &ch : s) {
        if (ch >= 'A' && ch <= 'Z') {
          ch = static_cast<char>(ch - 'A' + 'a');
        }
      }
      return s;
    }

    static std::vector<std::string> flatten_topology_device_ids(const display_device::ActiveTopology &topology) {
      std::vector<std::string> ids;
      for (const auto &group : topology) {
        for (const auto &id : group) {
          if (!id.empty()) {
            ids.push_back(id);
          }
        }
      }
      std::sort(ids.begin(), ids.end());
      ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
      return ids;
    }

    std::vector<std::string> missing_devices_for_topology(const display_device::ActiveTopology &topology) const {
      const auto topo_ids = flatten_topology_device_ids(topology);
      if (topo_ids.empty()) {
        return {};
      }

      const auto current_ids = enum_all_device_ids();
      std::set<std::string> current_norm;
      for (const auto &id : current_ids) {
        current_norm.insert(ascii_lower(id));
      }

      std::vector<std::string> missing;
      for (const auto &id : topo_ids) {
        if (current_norm.find(ascii_lower(id)) == current_norm.end()) {
          missing.push_back(id);
        }
      }
      return missing;
    }

    // Enumerate all currently available display device IDs (active or inactive).
    std::set<std::string> enum_all_device_ids() const {
      std::set<std::string> ids;
      for (const auto &d : enumerate_devices(display_device::DeviceEnumerationDetail::Minimal)) {
        const auto id = d.m_device_id.empty() ? d.m_display_name : d.m_device_id;
        if (!id.empty()) {
          ids.insert(id);
        }
      }
      return ids;
    }

    // Validate whether a snapshot's topology is currently applicable.
    bool is_topology_valid(const display_device::DisplaySettingsSnapshot &snap) const {
      if (!ensure_initialized()) {
        return false;
      }
      try {
        return m_dd->isTopologyValid(snap.m_topology);
      } catch (...) {
        return false;
      }
    }

    bool apply(
      const display_device::SingleDisplayConfiguration &cfg,
      const std::optional<display_device::ActiveTopology> &base_topology
    ) {
      if (!ensure_initialized()) {
        return false;
      }
      try {
        if (base_topology && m_dd->isTopologyValid(*base_topology)) {
          (void) m_dd->setTopology(*base_topology);
        }
      } catch (...) {
      }
      using enum display_device::SettingsManagerInterface::ApplyResult;
      const auto res = m_sm->applySettings(cfg);
      BOOST_LOG(info) << "ApplySettings result: " << static_cast<int>(res);
      return res == Ok;
    }

    bool apply(const display_device::SingleDisplayConfiguration &cfg) {
      return apply(cfg, std::nullopt);
    }

    // Revert display configuration; returns whether reverted OK.
    bool revert() {
      if (!ensure_initialized()) {
        return false;
      }
      using enum display_device::SettingsManagerInterface::RevertResult;
      const auto res = m_sm->revertSettings();
      BOOST_LOG(info) << "RevertSettings result: " << static_cast<int>(res);
      return res == Ok;
    }

    // Reset persistence file; best-effort noop persistence returns true.
    bool reset_persistence() {
      if (!ensure_initialized()) {
        return false;
      }
      return m_sm->resetPersistence();
    }

    bool set_display_origin(const std::string &device_id, const display_device::Point &origin) {
      if (!ensure_initialized()) {
        return false;
      }
      try {
        return m_dd->setDisplayOrigin(device_id, origin);
      } catch (...) {
        return false;
      }
    }

    bool configuration_matches_current_state(const display_device::SingleDisplayConfiguration &cfg) const {
      if (!ensure_initialized()) {
        return false;
      }
      if (cfg.m_device_id.empty()) {
        return false;
      }

      try {
        // Use targeted APIs instead of enumAvailableDevices() which enumerates all devices.
        // getCurrentDisplayModes/getCurrentHdrStates use queryDisplayConfig() to get active
        // config and search for specific devices - much faster than full enumeration.
        const std::set<std::string> device_ids {cfg.m_device_id};

        // Check resolution and refresh rate if specified in config
        if (cfg.m_resolution || cfg.m_refresh_rate) {
          auto modes = m_dd->getCurrentDisplayModes(device_ids);
          auto it = modes.find(cfg.m_device_id);
          if (it == modes.end()) {
            return false;  // Device not active or not found
          }
          const auto &mode = it->second;

          if (cfg.m_resolution) {
            if (mode.m_resolution.m_width != cfg.m_resolution->m_width ||
                mode.m_resolution.m_height != cfg.m_resolution->m_height) {
              return false;
            }
          }

          if (cfg.m_refresh_rate) {
            auto desired = floating_to_double(*cfg.m_refresh_rate);
            display_device::FloatingPoint actual_fp = mode.m_refresh_rate;
            auto actual = floating_to_double(actual_fp);
            if (!desired || !actual || !nearly_equal(*desired, *actual)) {
              return false;
            }
          }
        }

        // Check HDR state if specified in config
        if (cfg.m_hdr_state) {
          auto hdr_states = m_dd->getCurrentHdrStates(device_ids);
          auto it = hdr_states.find(cfg.m_device_id);
          if (it == hdr_states.end() || !it->second || *it->second != *cfg.m_hdr_state) {
            return false;
          }
        }

        return true;
      } catch (...) {
        return false;
      }
    }

    // Capture a full snapshot of current settings.
    display_device::DisplaySettingsSnapshot snapshot() const {
      display_device::DisplaySettingsSnapshot snap;
      if (!ensure_initialized()) {
        return snap;
      }
      try {
        // Topology - snapshot is taken before virtual displays are created,
        // so no filtering is needed.
        snap.m_topology = m_dd->getCurrentTopology();

        // Flatten device ids present in topology
        std::set<std::string> device_ids;
        for (const auto &grp : snap.m_topology) {
          device_ids.insert(grp.begin(), grp.end());
        }
        // Fall back to all enumerated devices if needed
        if (device_ids.empty()) {
          collect_all_device_ids(device_ids);
        }

        // Modes and HDR
        snap.m_modes = m_dd->getCurrentDisplayModes(device_ids);
        snap.m_hdr_states = m_dd->getCurrentHdrStates(device_ids);

        // Primary device
        const auto primary = find_primary_in_set(device_ids);
        if (primary) {
          snap.m_primary_device = *primary;
        }
      } catch (...) {
        // best-effort snapshot
      }
      return snap;
    }

    // Validate whether a proposed topology is acceptable by the OS using SDC_VALIDATE.
    bool validate_topology_with_os(const display_device::ActiveTopology &topo) const {
      if (!ensure_initialized()) {
        return false;
      }
      try {
        if (!m_dd->isTopologyValid(topo)) {
          return false;
        }
        const auto original_data = m_wapi->queryDisplayConfig(display_device::QueryType::All);
        if (!original_data) {
          return false;
        }
        const auto path_data = display_device::win_utils::collectSourceDataForMatchingPaths(*m_wapi, original_data->m_paths);
        if (path_data.empty()) {
          return false;
        }
        auto paths = display_device::win_utils::makePathsForNewTopology(topo, path_data, original_data->m_paths);
        if (paths.empty()) {
          return false;
        }
        UINT32 flags = SDC_VALIDATE | SDC_TOPOLOGY_SUPPLIED | SDC_ALLOW_PATH_ORDER_CHANGES | SDC_VIRTUAL_MODE_AWARE;
        LONG result = m_wapi->setDisplayConfig(paths, {}, flags);
        if (result == ERROR_GEN_FAILURE) {
          flags = SDC_VALIDATE | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_VIRTUAL_MODE_AWARE;
          result = m_wapi->setDisplayConfig(paths, {}, flags);
        }
        if (result != ERROR_SUCCESS) {
          BOOST_LOG(warning) << "Topology validation failed: " << result;
          return false;
        }
        return true;
      } catch (...) {
        return false;
      }
    }

    bool soft_test_display_settings(
      const display_device::SingleDisplayConfiguration &cfg,
      const std::optional<display_device::ActiveTopology> &base_topology
    ) const {
      if (!ensure_initialized()) {
        return false;
      }
      try {
        auto topo_before = base_topology.value_or(m_dd->getCurrentTopology());
        if (!m_dd->isTopologyValid(topo_before)) {
          return false;
        }
        const auto devices = enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
        auto initial = display_device::win_utils::computeInitialState(std::nullopt, topo_before, devices);
        if (!initial) {
          return false;
        }
        const auto [new_topology, device_to_configure, additional_devices] = display_device::win_utils::computeNewTopologyAndMetadata(
          cfg.m_device_prep,
          cfg.m_device_id,
          *initial
        );

        if (m_dd->isTopologyTheSame(topo_before, new_topology)) {
          return true;
        }
        return validate_topology_with_os(new_topology);
      } catch (...) {
        return false;
      }
    }

    bool soft_test_display_settings(const display_device::SingleDisplayConfiguration &cfg) const {
      return soft_test_display_settings(cfg, std::nullopt);
    }

    std::optional<display_device::ActiveTopology> compute_expected_topology(
      const display_device::SingleDisplayConfiguration &cfg,
      const std::optional<display_device::ActiveTopology> &base_topology
    ) const {
      if (!ensure_initialized()) {
        return std::nullopt;
      }
      try {
        auto topo_before = base_topology.value_or(m_dd->getCurrentTopology());
        if (!m_dd->isTopologyValid(topo_before)) {
          return std::nullopt;
        }
        const auto devices = enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
        auto initial = display_device::win_utils::computeInitialState(std::nullopt, topo_before, devices);
        if (!initial) {
          return std::nullopt;
        }
        const auto [new_topology, device_to_configure, additional_devices] = display_device::win_utils::computeNewTopologyAndMetadata(
          cfg.m_device_prep,
          cfg.m_device_id,
          *initial
        );
        return new_topology;
      } catch (...) {
        return std::nullopt;
      }
    }

    std::optional<display_device::ActiveTopology> compute_expected_topology(const display_device::SingleDisplayConfiguration &cfg) const {
      return compute_expected_topology(cfg, std::nullopt);
    }

    bool is_topology_the_same(const display_device::ActiveTopology &a, const display_device::ActiveTopology &b) const {
      if (!ensure_initialized()) {
        return false;
      }
      try {
        return m_dd->isTopologyTheSame(a, b);
      } catch (...) {
        return false;
      }
    }

    // Apply the HDR blank workaround synchronously (call from a background thread)
    void blank_hdr_states(std::chrono::milliseconds delay) {
      if (!ensure_initialized()) {
        return;
      }
      try {
        display_device::win_utils::blankHdrStates(*m_dd, delay);
      } catch (...) {
        // ignore errors; best effort
      }
    }

    // Compute a simple signature string from snapshot for change detection/logging.
    std::string signature(const display_device::DisplaySettingsSnapshot &snap) const {
      // Build a stable textual representation
      std::string s;
      s.reserve(1024);
      // Topology
      s += "T:";
      for (auto grp : snap.m_topology) {
        std::sort(grp.begin(), grp.end());
        s += "[";
        for (const auto &id : grp) {
          s += id;
          s += ",";
        }
        s += "]";
      }
      // Modes
      s += ";M:";
      for (const auto &kv : snap.m_modes) {
        s += kv.first;
        s += "=";
        s += std::to_string(kv.second.m_resolution.m_width);
        s += "x";
        s += std::to_string(kv.second.m_resolution.m_height);
        s += "@";
        s += std::to_string(kv.second.m_refresh_rate.m_numerator);
        s += "/";
        s += std::to_string(kv.second.m_refresh_rate.m_denominator);
        s += ";";
      }
      // HDR
      s += ";H:";
      for (const auto &kh : snap.m_hdr_states) {
        s += kh.first;
        s += "=";
        // Avoid ambiguous null; use explicit string for readability
        if (!kh.second.has_value()) {
          s += "unknown";
        } else {
          s += (*kh.second == display_device::HdrState::Enabled) ? "on" : "off";
        }
        s += ";";
      }
      // Primary
      s += ";P:";
      s += snap.m_primary_device;
      return s;
    }

    // Convenience: current topology signature for change detection watchers.
    std::string current_topology_signature() const {
      return signature(snapshot());
    }

    // Save snapshot to file as JSON-like format.
    bool save_display_settings_snapshot_to_file(const std::filesystem::path &path) const {
      auto snap = snapshot();
      const auto snapshot_exclusions = snapshot_exclusions_copy();
      auto is_excluded = [&](const std::string &device_id) {
        if (snapshot_exclusions.empty()) {
          return false;
        }
        const auto norm = normalize_device_id(device_id);
        return std::find(snapshot_exclusions.begin(), snapshot_exclusions.end(), norm) != snapshot_exclusions.end();
      };
      if (!is_topology_valid(snap)) {
        BOOST_LOG(warning) << "Skipping display snapshot save; topology is invalid or empty for path="
                           << path.string();
        return false;
      }
      if (snap.m_modes.empty()) {
        BOOST_LOG(warning) << "Skipping display snapshot save; mode set is empty for path=" << path.string();
        return false;
      }
      // Filter out devices without display_name (e.g., dummy plugs not properly attached to Windows).
      // These should not be saved as restore targets.
      {
        auto devices = enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
        std::set<std::string> valid_device_ids;
        for (const auto &d : devices) {
          if (!d.m_display_name.empty()) {
            const auto id = d.m_device_id.empty() ? d.m_display_name : d.m_device_id;
            valid_device_ids.insert(id);
          }
        }

        if (!snapshot_exclusions.empty()) {
          std::set<std::string> filtered_ids;
          std::vector<std::string> excluded_now;
          for (const auto &id : valid_device_ids) {
            if (is_excluded(id)) {
              excluded_now.push_back(id);
              continue;
            }
            filtered_ids.insert(id);
          }
          if (!excluded_now.empty()) {
            std::string joined;
            for (size_t i = 0; i < excluded_now.size(); ++i) {
              if (i > 0) {
                joined += ", ";
              }
              joined += excluded_now[i];
            }
            BOOST_LOG(info) << "Display snapshot: excluding devices from snapshot: [" << joined << "]";
          }
          valid_device_ids.swap(filtered_ids);
          if (valid_device_ids.empty()) {
            BOOST_LOG(warning) << "Skipping display snapshot save; all devices are excluded for path="
                               << path.string();
            return false;
          }
        }

        // Filter topology groups, removing devices without valid display_name
        display_device::ActiveTopology filtered_topology;
        for (const auto &grp : snap.m_topology) {
          std::vector<std::string> filtered_grp;
          for (const auto &device_id : grp) {
            if (valid_device_ids.count(device_id)) {
              filtered_grp.push_back(device_id);
            }
          }
          if (!filtered_grp.empty()) {
            filtered_topology.push_back(std::move(filtered_grp));
          }
        }

        if (filtered_topology.empty()) {
          BOOST_LOG(warning) << "Skipping display snapshot save; no devices with valid display_name for path="
                             << path.string();
          return false;
        }

        // Update snapshot with filtered data
        snap.m_topology = std::move(filtered_topology);

        // Filter modes and hdr_states to only include valid devices
        for (auto it = snap.m_modes.begin(); it != snap.m_modes.end();) {
          if (!valid_device_ids.count(it->first)) {
            it = snap.m_modes.erase(it);
          } else {
            ++it;
          }
        }
        for (auto it = snap.m_hdr_states.begin(); it != snap.m_hdr_states.end();) {
          if (!valid_device_ids.count(it->first)) {
            it = snap.m_hdr_states.erase(it);
          } else {
            ++it;
          }
        }

        // Clear primary if it was filtered out
        if (!valid_device_ids.count(snap.m_primary_device)) {
          snap.m_primary_device.clear();
        }
      }
      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);
      FILE *f = _wfopen(path.wstring().c_str(), L"wb");
      if (!f) {
        return false;
      }
      auto guard = std::unique_ptr<FILE, int (*)(FILE *)>(f, fclose);
      std::string out;
      out += "{\n  \"topology\": [";
      for (size_t i = 0; i < snap.m_topology.size(); ++i) {
        const auto &grp = snap.m_topology[i];
        out += "[";
        for (size_t j = 0; j < grp.size(); ++j) {
          out += "\"" + grp[j] + "\"";
          if (j + 1 < grp.size()) {
            out += ",";
          }
        }
        out += "]";
        if (i + 1 < snap.m_topology.size()) {
          out += ",";
        }
      }
      out += "],\n  \"modes\": {";
      size_t k = 0;
      for (const auto &kv : snap.m_modes) {
        out += "\n    \"" + kv.first + "\": { \"w\": " + std::to_string(kv.second.m_resolution.m_width) + ", \"h\": " + std::to_string(kv.second.m_resolution.m_height) + ", \"num\": " + std::to_string(kv.second.m_refresh_rate.m_numerator) + ", \"den\": " + std::to_string(kv.second.m_refresh_rate.m_denominator) + " }";
        if (++k < snap.m_modes.size()) {
          out += ",";
        }
      }
      out += "\n  },\n  \"hdr\": {";
      k = 0;
      for (const auto &kh : snap.m_hdr_states) {
        out += "\n    \"" + kh.first + "\": ";
        if (!kh.second.has_value()) {
          out += "null";
        } else {
          out += (*kh.second == display_device::HdrState::Enabled) ? "\"on\"" : "\"off\"";
        }
        if (++k < snap.m_hdr_states.size()) {
          out += ",";
        }
      }
      out += "\n  },\n  \"primary\": \"" + snap.m_primary_device + "\"\n}";
      const auto written = fwrite(out.data(), 1, out.size(), f);
      return written == out.size();
    }

    // Save a provided snapshot to file (without validation/filtering).
    bool save_snapshot_to_file(const display_device::DisplaySettingsSnapshot &snap, const std::filesystem::path &path) const {
      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);
      FILE *f = _wfopen(path.wstring().c_str(), L"wb");
      if (!f) {
        return false;
      }
      auto guard = std::unique_ptr<FILE, int (*)(FILE *)>(f, fclose);
      std::string out;
      out += "{\n  \"topology\": [";
      for (size_t i = 0; i < snap.m_topology.size(); ++i) {
        const auto &grp = snap.m_topology[i];
        out += "[";
        for (size_t j = 0; j < grp.size(); ++j) {
          out += "\"" + grp[j] + "\"";
          if (j + 1 < grp.size()) {
            out += ",";
          }
        }
        out += "]";
        if (i + 1 < snap.m_topology.size()) {
          out += ",";
        }
      }
      out += "],\n  \"modes\": {";
      size_t k = 0;
      for (const auto &kv : snap.m_modes) {
        out += "\n    \"" + kv.first + "\": { \"w\": " + std::to_string(kv.second.m_resolution.m_width) + ", \"h\": " + std::to_string(kv.second.m_resolution.m_height) + ", \"num\": " + std::to_string(kv.second.m_refresh_rate.m_numerator) + ", \"den\": " + std::to_string(kv.second.m_refresh_rate.m_denominator) + " }";
        if (++k < snap.m_modes.size()) {
          out += ",";
        }
      }
      out += "\n  },\n  \"hdr\": {";
      k = 0;
      for (const auto &kh : snap.m_hdr_states) {
        out += "\n    \"" + kh.first + "\": ";
        if (!kh.second.has_value()) {
          out += "null";
        } else {
          out += (*kh.second == display_device::HdrState::Enabled) ? "\"on\"" : "\"off\"";
        }
        if (++k < snap.m_hdr_states.size()) {
          out += ",";
        }
      }
      out += "\n  },\n  \"primary\": \"" + snap.m_primary_device + "\"\n}";
      const auto written = fwrite(out.data(), 1, out.size(), f);
      return written == out.size();
    }

    // Load snapshot from file.
    std::optional<display_device::DisplaySettingsSnapshot> load_display_settings_snapshot(const std::filesystem::path &path) const {
      std::error_code ec;
      if (!std::filesystem::exists(path, ec)) {
        return std::nullopt;
      }
      FILE *f = _wfopen(path.wstring().c_str(), L"rb");
      if (!f) {
        return std::nullopt;
      }
      auto guard = std::unique_ptr<FILE, int (*)(FILE *)>(f, fclose);
      std::string data;
      char buf[4096];
      while (size_t n = fread(buf, 1, sizeof(buf), f)) {
        data.append(buf, n);
      }

      display_device::DisplaySettingsSnapshot snap;
      const auto prim = find_str_section(data, "primary");
      const auto topo_s = find_str_section(data, "topology");
      const auto modes_s = find_str_section(data, "modes");
      const auto hdr_s = find_str_section(data, "hdr");
      parse_primary_field(prim, snap);
      parse_topology_field(topo_s, snap);
      parse_modes_field(modes_s, snap);
      parse_hdr_field(hdr_s, snap);

      // Filter snapshot using current exclusion list and currently enumerated devices.
      // Note: `m_display_name` is only populated for active displays in libdisplaydevice, so
      // using it here would incorrectly treat inactive-but-connected monitors as missing.
      // For loading/restore, we only require a matching device id (display_name is not required).
      const auto join = [](const auto &items) {
        std::string out;
        bool first = true;
        for (const auto &item : items) {
          if (!first) {
            out += ", ";
          }
          first = false;
          out += item;
        }
        return out;
      };
      std::set<std::string> valid_devices_norm;
      std::vector<std::string> filtered_out_excluded;
      std::vector<std::string> enumerated_devices;
      const auto exclusions = snapshot_exclusions_copy();
      std::set<std::string> exclusions_norm;
      for (auto id : exclusions) {
        exclusions_norm.insert(normalize_device_id(std::move(id)));
      }

      for (const auto &d : enumerate_devices(display_device::DeviceEnumerationDetail::Minimal)) {
        auto id = d.m_device_id.empty() ? d.m_display_name : d.m_device_id;
        if (id.empty()) {
          continue;
        }
        enumerated_devices.push_back(id);
        auto norm = normalize_device_id(id);
        if (!exclusions_norm.empty() && exclusions_norm.count(norm)) {
          filtered_out_excluded.push_back(id);
          continue;
        }
        valid_devices_norm.insert(std::move(norm));
      }

      if (valid_devices_norm.empty()) {
        BOOST_LOG(warning) << "Snapshot load rejected: no valid devices available for path=" << path.string();
        BOOST_LOG(debug) << "Snapshot load rejected details: enumerated_devices=[" << join(enumerated_devices)
                         << "], exclusions=[" << join(exclusions_norm) << "]";
        return std::nullopt;
      }

      auto is_allowed = [&](const std::string &device_id) {
        const auto norm = normalize_device_id(device_id);
        if (!valid_devices_norm.count(norm)) {
          return false;
        }
        return exclusions_norm.empty() || !exclusions_norm.count(norm);
      };

      display_device::ActiveTopology filtered_topology;
      for (const auto &grp : snap.m_topology) {
        std::vector<std::string> filtered_grp;
        for (const auto &device_id : grp) {
          if (is_allowed(device_id)) {
            filtered_grp.push_back(device_id);
          } else if (!exclusions_norm.empty() && exclusions_norm.count(normalize_device_id(device_id))) {
            filtered_out_excluded.push_back(device_id);
          }
        }
        if (!filtered_grp.empty()) {
          filtered_topology.push_back(std::move(filtered_grp));
        }
      }

      if (filtered_topology.empty()) {
        BOOST_LOG(warning) << "Snapshot load rejected: all devices filtered for path=" << path.string();
        std::vector<std::string> snapshot_devices;
        for (const auto &grp : snap.m_topology) {
          snapshot_devices.insert(snapshot_devices.end(), grp.begin(), grp.end());
        }
        std::sort(snapshot_devices.begin(), snapshot_devices.end());
        snapshot_devices.erase(std::unique(snapshot_devices.begin(), snapshot_devices.end()), snapshot_devices.end());
        BOOST_LOG(debug) << "Snapshot load rejected details: snapshot_devices=[" << join(snapshot_devices)
                         << "], present_devices=[" << join(valid_devices_norm)
                         << "], exclusions=[" << join(exclusions_norm) << "]";
        return std::nullopt;
      }

      snap.m_topology = std::move(filtered_topology);

      for (auto it = snap.m_modes.begin(); it != snap.m_modes.end();) {
        if (!is_allowed(it->first)) {
          it = snap.m_modes.erase(it);
        } else {
          ++it;
        }
      }
      for (auto it = snap.m_hdr_states.begin(); it != snap.m_hdr_states.end();) {
        if (!is_allowed(it->first)) {
          it = snap.m_hdr_states.erase(it);
        } else {
          ++it;
        }
      }
      if (!snap.m_primary_device.empty() && !is_allowed(snap.m_primary_device)) {
        snap.m_primary_device.clear();
      }

      if (!filtered_out_excluded.empty()) {
        std::sort(filtered_out_excluded.begin(), filtered_out_excluded.end());
        filtered_out_excluded.erase(std::unique(filtered_out_excluded.begin(), filtered_out_excluded.end()), filtered_out_excluded.end());
        std::string joined;
        for (size_t i = 0; i < filtered_out_excluded.size(); ++i) {
          if (i > 0) {
            joined += ", ";
          }
          joined += filtered_out_excluded[i];
        }
        BOOST_LOG(info) << "Snapshot load: excluded devices filtered from " << path.string() << ": [" << joined << "]";
      }

      return snap;
    }

    // Apply snapshot best-effort.
    bool apply_snapshot(const display_device::DisplaySettingsSnapshot &snap) {
      if (!ensure_initialized()) {
        return false;
      }
      try {
        (void) m_dd->setTopology(snap.m_topology);
        (void) m_dd->setDisplayModesTemporary(snap.m_modes);
        (void) m_dd->setHdrStates(snap.m_hdr_states);
        if (!snap.m_primary_device.empty()) {
          (void) m_dd->setAsPrimary(snap.m_primary_device);
        }
        return true;
      } catch (...) {
        return false;
      }
    }

    void set_snapshot_exclusions(const std::vector<std::string> &ids) {
      std::lock_guard<std::mutex> lock(snapshot_exclude_mutex_);
      snapshot_exclude_devices_.clear();
      std::set<std::string> unique;
      for (auto id : ids) {
        id = normalize_device_id(std::move(id));
        if (!id.empty()) {
          unique.insert(std::move(id));
        }
      }
      snapshot_exclude_devices_.assign(unique.begin(), unique.end());
    }

    std::vector<std::string> snapshot_exclusions_copy_public() const {
      return snapshot_exclusions_copy();
    }

  private:
    enum class InitState : uint8_t {
      Uninitialized,
      Ready,
      Failed
    };

    bool ensure_initialized() const {
      auto state = m_init_state.load(std::memory_order_acquire);
      if (state == InitState::Ready) {
        return true;
      }
      if (state == InitState::Failed) {
        return false;
      }

      std::call_once(m_init_once, [this]() noexcept {
        try {
          auto wapi = std::make_shared<display_device::WinApiLayer>();
          auto dd = std::make_shared<display_device::WinDisplayDevice>(wapi);
          auto sm = std::make_unique<display_device::SettingsManager>(
            dd,
            std::make_shared<display_device::NoopAudioContext>(),
            std::make_unique<display_device::PersistentState>(std::make_shared<display_device::NoopSettingsPersistence>()),
            display_device::WinWorkarounds {}
          );
          m_wapi = std::move(wapi);
          m_dd = std::move(dd);
          m_sm = std::move(sm);
          m_init_state.store(InitState::Ready, std::memory_order_release);
        } catch (...) {
          BOOST_LOG(error) << "Display helper: failed to initialize display controller stack.";
          m_init_state.store(InitState::Failed, std::memory_order_release);
        }
      });

      return m_init_state.load(std::memory_order_acquire) == InitState::Ready;
    }

    mutable std::once_flag m_init_once;
    mutable std::atomic<InitState> m_init_state {InitState::Uninitialized};
    mutable std::shared_ptr<display_device::WinApiLayer> m_wapi;
    mutable std::shared_ptr<display_device::WinDisplayDevice> m_dd;
    mutable std::unique_ptr<display_device::SettingsManager> m_sm;
    mutable std::mutex snapshot_exclude_mutex_;
    std::vector<std::string> snapshot_exclude_devices_;

    static std::string normalize_device_id(std::string id) {
      id.erase(id.begin(), std::find_if(id.begin(), id.end(), [](unsigned char ch) { return !std::isspace(ch); }));
      id.erase(std::find_if(id.rbegin(), id.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), id.end());
      std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return id;
    }

    std::vector<std::string> snapshot_exclusions_copy() const {
      std::lock_guard<std::mutex> lock(snapshot_exclude_mutex_);
      return snapshot_exclude_devices_;
    }

    void collect_all_device_ids(std::set<std::string> &out) const {
      for (const auto &d : enumerate_devices(display_device::DeviceEnumerationDetail::Minimal)) {
        const auto id = d.m_device_id.empty() ? d.m_display_name : d.m_device_id;
        if (!id.empty()) {
          out.insert(id);
        }
      }
    }

  public:
    display_device::EnumeratedDeviceList enumerate_devices(display_device::DeviceEnumerationDetail detail) const {
      if (!ensure_initialized()) {
        return {};
      }
      try {
        return m_dd->enumAvailableDevices(detail);
      } catch (...) {
        return {};
      }
    }

  private:
    std::optional<std::string> find_primary_in_set(const std::set<std::string> &ids) const {
      if (!ensure_initialized()) {
        return std::nullopt;
      }
      for (const auto &id : ids) {
        if (m_dd->isPrimary(id)) {
          return id;
        }
      }
      return std::nullopt;
    }

    // Parsing helpers to keep cyclomatic complexity low.
    static std::string find_str_section(const std::string &data, const std::string &key) {
      auto p = data.find("\"" + key + "\"");
      if (p == std::string::npos) {
        return {};
      }
      p = data.find(':', p);
      if (p == std::string::npos) {
        return {};
      }
      return data.substr(p + 1);
    }

    static std::optional<double> floating_to_double(const display_device::FloatingPoint &value) {
      if (std::holds_alternative<double>(value)) {
        return std::get<double>(value);
      }
      const auto &rat = std::get<display_device::Rational>(value);
      if (rat.m_denominator == 0) {
        return std::nullopt;
      }
      return static_cast<double>(rat.m_numerator) / static_cast<double>(rat.m_denominator);
    }

    static bool nearly_equal(double lhs, double rhs) {
      const double diff = std::abs(lhs - rhs);
      const double scale = std::max({1.0, std::abs(lhs), std::abs(rhs)});
      return diff <= scale * 1e-4;
    }

    std::optional<display_device::EnumeratedDevice::Info> get_device_info_minimal(const std::string &device_id) const {
      if (!ensure_initialized()) {
        return std::nullopt;
      }
      try {
        auto devices = m_dd->enumAvailableDevices(display_device::DeviceEnumerationDetail::Minimal);
        for (const auto &device : devices) {
          if (device.m_device_id == device_id && device.m_info) {
            return device.m_info;
          }
        }
      } catch (...) {
      }
      return std::nullopt;
    }

    bool info_matches_config(
      const display_device::EnumeratedDevice::Info &info,
      const display_device::SingleDisplayConfiguration &cfg
    ) const {
      if (cfg.m_resolution) {
        if (info.m_resolution.m_width != cfg.m_resolution->m_width ||
            info.m_resolution.m_height != cfg.m_resolution->m_height) {
          return false;
        }
      }

      if (cfg.m_refresh_rate) {
        auto desired = floating_to_double(*cfg.m_refresh_rate);
        auto actual = floating_to_double(info.m_refresh_rate);
        if (!desired || !actual || !nearly_equal(*desired, *actual)) {
          return false;
        }
      }

      if (cfg.m_hdr_state) {
        if (!info.m_hdr_state || *info.m_hdr_state != *cfg.m_hdr_state) {
          return false;
        }
      }

      return true;
    }

    static void parse_primary_field(const std::string &prim, display_device::DisplaySettingsSnapshot &snap) {
      auto q1 = prim.find('"');
      auto q2 = prim.find('"', q1 == std::string::npos ? 0 : q1 + 1);
      if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1) {
        snap.m_primary_device = prim.substr(q1 + 1, q2 - q1 - 1);
      }
    }

    static void parse_topology_field(const std::string &topo_s, display_device::DisplaySettingsSnapshot &snap) {
      snap.m_topology.clear();
      size_t i = topo_s.find('[');
      if (i == std::string::npos) {
        return;
      }
      ++i;  // skip [
      while (i < topo_s.size() && topo_s[i] != ']') {
        while (i < topo_s.size() && topo_s[i] != '[' && topo_s[i] != ']') {
          ++i;
        }
        if (i >= topo_s.size() || topo_s[i] == ']') {
          break;
        }
        ++i;  // skip [
        std::vector<std::string> grp;
        while (i < topo_s.size() && topo_s[i] != ']') {
          while (i < topo_s.size() && topo_s[i] != '"' && topo_s[i] != ']') {
            ++i;
          }
          if (i >= topo_s.size() || topo_s[i] == ']') {
            break;
          }
          auto q1 = i + 1;
          auto q2 = topo_s.find('"', q1);
          if (q2 == std::string::npos) {
            break;
          }
          grp.emplace_back(topo_s.substr(q1, q2 - q1));
          i = q2 + 1;
        }
        while (i < topo_s.size() && topo_s[i] != ']') {
          ++i;
        }
        if (i < topo_s.size() && topo_s[i] == ']') {
          ++i;  // skip ]
        }
        snap.m_topology.emplace_back(std::move(grp));
      }
    }

    static unsigned int parse_num_field(const std::string &obj, const char *key) {
      auto p = obj.find(key);
      if (p == std::string::npos) {
        return 0;
      }
      p = obj.find(':', p);
      if (p == std::string::npos) {
        return 0;
      }
      return static_cast<unsigned int>(std::stoul(obj.substr(p + 1)));
    }

    static void parse_modes_field(const std::string &modes_s, display_device::DisplaySettingsSnapshot &snap) {
      snap.m_modes.clear();
      size_t i = modes_s.find('{');
      if (i == std::string::npos) {
        return;
      }
      ++i;
      while (i < modes_s.size() && modes_s[i] != '}') {
        while (i < modes_s.size() && modes_s[i] != '"' && modes_s[i] != '}') {
          ++i;
        }
        if (i >= modes_s.size() || modes_s[i] == '}') {
          break;
        }
        auto q1 = i + 1;
        auto q2 = modes_s.find('"', q1);
        if (q2 == std::string::npos) {
          break;
        }
        std::string id = modes_s.substr(q1, q2 - q1);
        i = modes_s.find('{', q2);
        if (i == std::string::npos) {
          break;
        }
        auto end = modes_s.find('}', i);
        if (end == std::string::npos) {
          break;
        }
        auto obj = modes_s.substr(i, end - i);
        display_device::DisplayMode dm;
        dm.m_resolution.m_width = parse_num_field(obj, "\"w\"");
        dm.m_resolution.m_height = parse_num_field(obj, "\"h\"");
        dm.m_refresh_rate.m_numerator = parse_num_field(obj, "\"num\"");
        dm.m_refresh_rate.m_denominator = parse_num_field(obj, "\"den\"");
        snap.m_modes.emplace(id, dm);
        i = end + 1;
      }
    }

    static void parse_hdr_field(const std::string &hdr_s, display_device::DisplaySettingsSnapshot &snap) {
      snap.m_hdr_states.clear();
      size_t i = hdr_s.find('{');
      if (i == std::string::npos) {
        return;
      }
      ++i;
      while (i < hdr_s.size() && hdr_s[i] != '}') {
        while (i < hdr_s.size() && hdr_s[i] != '"' && hdr_s[i] != '}') {
          ++i;
        }
        if (i >= hdr_s.size() || hdr_s[i] == '}') {
          break;
        }
        auto q1 = i + 1;
        auto q2 = hdr_s.find('"', q1);
        if (q2 == std::string::npos) {
          break;
        }
        std::string id = hdr_s.substr(q1, q2 - q1);
        i = hdr_s.find(':', q2);
        if (i == std::string::npos) {
          break;
        }
        ++i;
        while (i < hdr_s.size() && (hdr_s[i] == ' ' || hdr_s[i] == '"')) {
          ++i;
        }
        std::optional<display_device::HdrState> val;
        if (hdr_s.compare(i, 2, "on") == 0) {
          val = display_device::HdrState::Enabled;
        } else if (hdr_s.compare(i, 3, "off") == 0) {
          val = display_device::HdrState::Disabled;
        } else {
          val = std::nullopt;
        }
        snap.m_hdr_states.emplace(id, val);
        while (i < hdr_s.size() && hdr_s[i] != ',' && hdr_s[i] != '}') {
          ++i;
        }
        if (i < hdr_s.size() && hdr_s[i] == ',') {
          ++i;
        }
      }
    }
  };

  constexpr std::chrono::milliseconds kApplyDisconnectGrace {5000};

  class DisplayDeviceLogBridge {
  public:
    DisplayDeviceLogBridge() = default;

    void install() {
      display_device::Logger::get().setCustomCallback(
        [this](display_device::Logger::LogLevel level, std::string message) {
          handle_log(level, std::move(message));
        }
      );
    }

  private:
    void handle_log(display_device::Logger::LogLevel level, std::string message) {
      const auto now = std::chrono::steady_clock::now();
      const std::string key = std::to_string(static_cast<int>(level)) + "|" + message;

      {
        std::lock_guard lk(mutex_);
        auto it = last_emit_.find(key);
        if (it != last_emit_.end()) {
          if ((now - it->second) < throttle_window_) {
            return;
          }
          it->second = now;
        } else {
          if (last_emit_.size() >= max_entries_) {
            prune(now);
          }
          last_emit_.emplace(key, now);
        }
      }

      forward(level, message);
    }

    void prune(std::chrono::steady_clock::time_point now) {
      for (auto it = last_emit_.begin(); it != last_emit_.end();) {
        if ((now - it->second) > prune_window_) {
          it = last_emit_.erase(it);
        } else {
          ++it;
        }
      }
      if (last_emit_.size() >= max_entries_) {
        last_emit_.clear();
      }
    }

    void forward(display_device::Logger::LogLevel level, const std::string &message) {
      const auto prefixed = std::string("display_device: ") + message;
      switch (level) {
        case display_device::Logger::LogLevel::verbose:
        case display_device::Logger::LogLevel::debug:
          BOOST_LOG(debug) << prefixed;
          break;
        case display_device::Logger::LogLevel::info:
          BOOST_LOG(info) << prefixed;
          break;
        case display_device::Logger::LogLevel::warning:
          BOOST_LOG(warning) << prefixed;
          break;
        case display_device::Logger::LogLevel::error:
          BOOST_LOG(error) << prefixed;
          break;
        case display_device::Logger::LogLevel::fatal:
          BOOST_LOG(fatal) << prefixed;
          break;
      }
    }

    std::mutex mutex_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_emit_;
    static constexpr std::chrono::seconds throttle_window_ {15};
    static constexpr std::chrono::seconds prune_window_ {60};
    static constexpr size_t max_entries_ {256};
  };

  DisplayDeviceLogBridge &dd_log_bridge() {
    static DisplayDeviceLogBridge bridge;
    return bridge;
  }

  class DisplayEventPump {
  public:
    using Callback = std::function<void(const char *)>;

    void start(Callback cb) {
      stop();
      callback_ = std::move(cb);
      worker_ = std::jthread(&DisplayEventPump::thread_proc, this);
    }

    void stop() {
      if (worker_.joinable()) {
        if (HWND hwnd = hwnd_.load(std::memory_order_acquire)) {
          PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        worker_.request_stop();
        worker_.join();
      }
      callback_ = nullptr;
      hwnd_.store(nullptr, std::memory_order_release);
    }

    ~DisplayEventPump() {
      stop();
    }

  private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
      if (msg == WM_NCCREATE) {
        auto *create = reinterpret_cast<CREATESTRUCTW *>(lParam);
        auto *self = static_cast<DisplayEventPump *>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_.store(hwnd, std::memory_order_release);
        return TRUE;
      }

      auto *self = reinterpret_cast<DisplayEventPump *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
      if (!self) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
      }

      switch (msg) {
        case WM_DISPLAYCHANGE:
          self->signal("wm_displaychange");
          break;
        case WM_DEVICECHANGE:
          if (wParam == DBT_DEVNODES_CHANGED || wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) {
            self->signal("wm_devicechange");
          }
          break;
        case WM_POWERBROADCAST:
          if (wParam == PBT_POWERSETTINGCHANGE) {
            const auto *ps = reinterpret_cast<const POWERBROADCAST_SETTING *>(lParam);
            if (ps && ps->PowerSetting == GUID_MONITOR_POWER_ON) {
              if (ps->DataLength == sizeof(DWORD)) {
                const DWORD state = *reinterpret_cast<const DWORD *>(ps->Data);
                if (state != 0) {
                  self->signal("power_monitor_on");
                }
              }
            }
          }
          break;
        case WM_DESTROY:
          self->cleanup_notifications();
          PostQuitMessage(0);
          break;
        default:
          break;
      }
      return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    void signal(const char *reason) {
      auto cb = callback_;
      if (cb) {
        try {
          cb(reason);
        } catch (...) {}
      }
    }

    void cleanup_notifications() {
      if (power_cookie_) {
        UnregisterPowerSettingNotification(power_cookie_);
        power_cookie_ = nullptr;
      }
      if (device_cookie_) {
        UnregisterDeviceNotification(device_cookie_);
        device_cookie_ = nullptr;
      }
    }

    void thread_proc(std::stop_token st) {
      const auto hinst = GetModuleHandleW(nullptr);
      const wchar_t *klass = L"SunshineDisplayEventWindow";

      WNDCLASSEXW wc = {};
      wc.cbSize = sizeof(wc);
      wc.lpfnWndProc = &DisplayEventPump::wnd_proc;
      wc.hInstance = hinst;
      wc.lpszClassName = klass;
      RegisterClassExW(&wc);

      HWND hwnd = CreateWindowExW(0, klass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hinst, this);
      if (!hwnd) {
        return;
      }

      power_cookie_ = RegisterPowerSettingNotification(hwnd, &GUID_MONITOR_POWER_ON, DEVICE_NOTIFY_WINDOW_HANDLE);

      DEV_BROADCAST_DEVICEINTERFACE_W dbi = {};
      dbi.dbcc_size = sizeof(dbi);
      dbi.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
      dbi.dbcc_classguid = kMonitorInterfaceGuid;
      device_cookie_ = RegisterDeviceNotificationW(hwnd, &dbi, DEVICE_NOTIFY_WINDOW_HANDLE);

      MSG msg;
      while (!st.stop_requested()) {
        const BOOL res = GetMessageW(&msg, nullptr, 0, 0);
        if (res == -1) {
          break;
        }
        if (res == 0) {
          break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }

      cleanup_notifications();
      if (hwnd) {
        DestroyWindow(hwnd);
      }
      hwnd_.store(nullptr, std::memory_order_release);
      UnregisterClassW(klass, hinst);
    }

    std::jthread worker_;
    Callback callback_;
    std::atomic<HWND> hwnd_ {nullptr};
    HPOWERNOTIFY power_cookie_ {nullptr};
    HDEVNOTIFY device_cookie_ {nullptr};
  };

  bool create_restore_scheduled_task();
  bool delete_restore_scheduled_task();

  struct ServiceState {
    enum class RestoreWindow {
      Primary,
      Event
    };

    DisplayController controller;
    DisplayEventPump event_pump;
    std::atomic<bool> event_pump_running {false};
    std::mutex restore_event_mutex;
    std::condition_variable restore_event_cv;
    bool restore_event_flag {false};
    std::atomic<long long> restore_active_until_ms {0};
    std::atomic<long long> last_restore_event_ms {0};
    std::atomic<bool> restore_stage_running {false};
    std::atomic<RestoreWindow> restore_active_window {RestoreWindow::Event};
    std::atomic<bool> retry_apply_on_topology {false};
    std::atomic<bool> retry_revert_on_topology {false};
    std::optional<display_device::SingleDisplayConfiguration> last_cfg;
    std::atomic<bool> exit_after_revert {false};
    std::atomic<bool> *running_flag {nullptr};
    std::jthread delayed_reapply_thread;  // Best-effort re-apply timer
    std::jthread hdr_blank_thread;  // Async HDR workaround thread (one-shot)
    std::jthread post_apply_thread;  // Async post-apply tasks (shell refresh, re-apply, HDR blank)
    std::filesystem::path golden_path;  // file to store golden snapshot
    std::filesystem::path session_path;  // legacy single-file path (migration only)
    std::filesystem::path session_current_path;  // file to store current session baseline snapshot (first apply)
    std::filesystem::path session_previous_path;  // file to persist last known-good baseline across runs
    std::atomic<bool> session_saved {false};
    // Track last APPLY to suppress revert-on-topology within a grace window
    std::atomic<long long> last_apply_ms {0};
    // If a REVERT was requested directly by Sunshine, bypass grace
    std::atomic<bool> direct_revert_bypass_grace {false};
    // Track whether a revert/restore is currently pending
    std::atomic<bool> restore_requested {false};
    std::atomic<uint64_t> restore_cancel_generation {0};
    // Guard: if a session restore succeeded recently, suppress Golden for a cooldown
    std::atomic<long long> last_session_restore_success_ms {0};
    // When true, prefer golden snapshot over session snapshots during restore (reduces stuck virtual screens)
    std::atomic<bool> always_restore_from_golden {false};

    // Polling-based restore loop state (replaces topology-change-triggered retries)
    std::jthread restore_poll_thread;
    std::atomic<bool> restore_poll_active {false};
    std::atomic<uint64_t> next_connection_epoch {1};
    std::atomic<uint64_t> active_connection_epoch {0};
    std::atomic<uint64_t> restore_origin_epoch {0};
    std::atomic<bool> heartbeat_monitor_active {false};
    std::atomic<long long> heartbeat_optional_until_ms {0};
    std::atomic<long long> last_heartbeat_ms {0};
    std::atomic<bool> heartbeat_revert_armed {false};
    std::atomic<long long> heartbeat_revert_deadline_ms {0};

    static constexpr auto kRestoreWindowPrimary = std::chrono::minutes(2);
    static constexpr auto kRestoreWindowEvent = std::chrono::seconds(30);
    static constexpr auto kRestoreEventDebounce = std::chrono::milliseconds(500);
    static constexpr auto kHeartbeatOptionalWindow = std::chrono::seconds(30);
    static constexpr auto kHeartbeatMissWindow = std::chrono::seconds(30);
    static constexpr auto kHeartbeatRecoveryWindow = std::chrono::minutes(2);
    static constexpr auto kVerificationSettleDelay = std::chrono::milliseconds(250);
    std::atomic<size_t> restore_backoff_index {0};
    std::atomic<long long> restore_next_allowed_ms {0};
    static constexpr std::array<std::chrono::seconds, 8> kRestoreBackoffProfile {
      std::chrono::seconds(0),
      std::chrono::seconds(1),
      std::chrono::seconds(3),
      std::chrono::seconds(5),
      std::chrono::seconds(10),
      std::chrono::seconds(15),
      std::chrono::seconds(20),
      std::chrono::seconds(30)
    };
    // IPC command queue to decouple pipe reads from heavy display operations
    std::mutex command_queue_mutex;
    std::condition_variable command_queue_cv;
    std::deque<std::vector<uint8_t>> command_queue;
    std::atomic<bool> command_worker_stop {false};
    std::jthread command_worker;
    std::atomic<uint64_t> command_worker_epoch {0};
    std::mutex async_join_mutex;  // Guards async joiners used to avoid blocking the command loop
    std::vector<std::jthread> async_join_threads;

    static long long steady_now_ms() {
      return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()
      )
        .count();
    }

    void begin_heartbeat_monitoring() {
      const auto now = steady_now_ms();
      heartbeat_monitor_active.store(true, std::memory_order_release);
      last_heartbeat_ms.store(now, std::memory_order_release);
      heartbeat_optional_until_ms.store(
        now + std::chrono::duration_cast<std::chrono::milliseconds>(kHeartbeatOptionalWindow).count(),
        std::memory_order_release
      );
      heartbeat_revert_armed.store(false, std::memory_order_release);
      heartbeat_revert_deadline_ms.store(0, std::memory_order_release);
    }

    void end_heartbeat_monitoring() {
      heartbeat_monitor_active.store(false, std::memory_order_release);
      heartbeat_revert_armed.store(false, std::memory_order_release);
      heartbeat_optional_until_ms.store(0, std::memory_order_release);
      heartbeat_revert_deadline_ms.store(0, std::memory_order_release);
      last_heartbeat_ms.store(0, std::memory_order_release);
    }

    void record_heartbeat_ping() {
      if (!heartbeat_monitor_active.load(std::memory_order_acquire)) {
        return;
      }
      const auto now = steady_now_ms();
      last_heartbeat_ms.store(now, std::memory_order_release);
      if (heartbeat_revert_armed.exchange(false, std::memory_order_acq_rel)) {
        heartbeat_revert_deadline_ms.store(0, std::memory_order_release);
        BOOST_LOG(info) << "Heartbeat restored; cancelling pending revert countdown.";
      }
    }

    bool check_heartbeat_timeout() {
      if (!heartbeat_monitor_active.load(std::memory_order_acquire)) {
        return false;
      }
      const auto now = steady_now_ms();
      const auto optional_until = heartbeat_optional_until_ms.load(std::memory_order_acquire);
      if (optional_until > 0 && now < optional_until) {
        return false;
      }
      const auto last_ping = last_heartbeat_ms.load(std::memory_order_acquire);
      const auto since_last = now - last_ping;
      const auto miss_threshold = std::chrono::duration_cast<std::chrono::milliseconds>(kHeartbeatMissWindow).count();
      if (!heartbeat_revert_armed.load(std::memory_order_acquire)) {
        if (since_last < miss_threshold) {
          return false;
        }
        const auto recovery_ms = std::chrono::duration_cast<std::chrono::milliseconds>(kHeartbeatRecoveryWindow).count();
        heartbeat_revert_deadline_ms.store(now + recovery_ms, std::memory_order_release);
        heartbeat_revert_armed.store(true, std::memory_order_release);
        BOOST_LOG(warning) << "Heartbeat missing for " << (since_last / 1000.0)
                           << "s; allowing up to " << (recovery_ms / 1000.0)
                           << "s for Sunshine to reconnect before restoring display configuration.";
        return false;
      }
      const auto deadline = heartbeat_revert_deadline_ms.load(std::memory_order_acquire);
      if (deadline != 0 && now >= deadline) {
        heartbeat_monitor_active.store(false, std::memory_order_release);
        heartbeat_revert_armed.store(false, std::memory_order_release);
        heartbeat_revert_deadline_ms.store(0, std::memory_order_release);
        return true;
      }
      return false;
    }

    void reset_restore_backoff() {
      restore_backoff_index.store(0, std::memory_order_release);
      restore_next_allowed_ms.store(0, std::memory_order_release);
    }

    void arm_restore_grace(std::chrono::milliseconds delay, const char *reason) {
      if (delay <= std::chrono::milliseconds::zero()) {
        return;
      }
      const auto now = steady_now_ms();
      const auto target = now + delay.count();
      const auto existing = restore_next_allowed_ms.load(std::memory_order_acquire);
      if (existing != 0 && existing >= target) {
        return;
      }
      restore_next_allowed_ms.store(target, std::memory_order_release);
      BOOST_LOG(debug) << "Restore grace armed for " << delay.count() << "ms"
                       << (reason ? std::string(" (") + reason + ")" : "");
    }

    void request_restore_cancel() {
      restore_cancel_generation.fetch_add(1, std::memory_order_acq_rel);
      signal_restore_event(nullptr);
    }

    void register_restore_failure() {
      size_t idx = restore_backoff_index.load(std::memory_order_acquire);
      if (idx + 1 < kRestoreBackoffProfile.size()) {
        ++idx;
      }
      const auto delay = kRestoreBackoffProfile[idx];
      const auto now = steady_now_ms();
      restore_backoff_index.store(idx, std::memory_order_release);
      restore_next_allowed_ms.store(
        now + std::chrono::duration_cast<std::chrono::milliseconds>(delay).count(),
        std::memory_order_release
      );
      if (delay.count() > 0) {
        BOOST_LOG(info) << "Restore polling: scheduling next attempt in " << delay.count() << "s.";
      }
    }

    bool await_restore_backoff(std::stop_token st) {
      constexpr auto kStep = std::chrono::milliseconds(200);
      while (!st.stop_requested()) {
        if (!restore_requested.load(std::memory_order_acquire)) {
          return false;
        }
        const auto allowed = restore_next_allowed_ms.load(std::memory_order_acquire);
        if (allowed == 0) {
          return true;
        }
        const auto now = steady_now_ms();
        if (now >= allowed) {
          return true;
        }
        const auto remaining = allowed - now;
        const auto sleep_ms = std::clamp<long long>(remaining, 1, kStep.count());
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
      }
      return false;
    }

    // Move the current session snapshot to the previous slot (overwrite) so we keep
    // one level of history for the restore chain.
    bool promote_current_snapshot_to_previous(const char *reason = nullptr) {
      std::error_code ec_exist;
      if (!std::filesystem::exists(session_current_path, ec_exist) || ec_exist) {
        return false;
      }

      std::error_code ec_dir;
      std::filesystem::create_directories(session_previous_path.parent_path(), ec_dir);
      (void) ec_dir;

      std::error_code ec_rm_prev;
      std::filesystem::remove(session_previous_path, ec_rm_prev);
      (void) ec_rm_prev;

      std::error_code ec_move;
      std::filesystem::rename(session_current_path, session_previous_path, ec_move);
      bool ok = !ec_move;
      if (ec_move) {
        std::error_code ec_copy;
        std::filesystem::copy_file(
          session_current_path,
          session_previous_path,
          std::filesystem::copy_options::overwrite_existing,
          ec_copy
        );
        ok = !ec_copy;
        if (ok) {
          std::error_code ec_rm_cur;
          std::filesystem::remove(session_current_path, ec_rm_cur);
          (void) ec_rm_cur;
        }
      }

      const char *why = reason ? reason : "rotation";
      BOOST_LOG(ok ? info : warning) << "Session snapshot promotion (" << why
                                     << ") current->previous result=" << (ok ? "true" : "false");
      return ok;
    }

    // Capture the current display state to the "current" snapshot slot.
    bool capture_current_snapshot(const char *reason = nullptr) {
      const bool saved = controller.save_display_settings_snapshot_to_file(session_current_path);
      session_saved.store(saved, std::memory_order_release);
      const char *why = reason ? reason : "apply";
      BOOST_LOG(info) << "Saved current session snapshot (" << why << "): " << (saved ? "true" : "false");
      return saved;
    }

    void prepare_session_topology() {
      if (session_saved.load(std::memory_order_acquire)) {
        return;
      }
      std::error_code ec_exist;
      const bool exists = std::filesystem::exists(session_path, ec_exist);
      if (exists && !ec_exist) {
        session_saved.store(true, std::memory_order_release);
        BOOST_LOG(info) << "Session baseline already exists; preserving existing snapshot: "
                        << session_path.string();
        return;
      }
      const bool saved = controller.save_display_settings_snapshot_to_file(session_path);
      session_saved.store(saved, std::memory_order_release);
      BOOST_LOG(info) << "Saved session baseline snapshot to file: "
                      << (saved ? "true" : "false");
    }

    void ensure_session_state(const display_device::ActiveTopology &expected_topology) {
      if (session_saved.load(std::memory_order_acquire)) {
        return;
      }
      std::error_code ec_exist;
      if (std::filesystem::exists(session_path, ec_exist) && !ec_exist) {
        session_saved.store(true, std::memory_order_release);
        return;
      }

      const auto actual = controller.snapshot().m_topology;
      const bool matches_expected = controller.is_topology_the_same(actual, expected_topology);

      std::error_code ec_prev;
      const bool has_prev = std::filesystem::exists(session_previous_path, ec_prev) && !ec_prev;
      if (has_prev && matches_expected) {
        auto prev = controller.load_display_settings_snapshot(session_previous_path);
        if (prev && !controller.is_topology_the_same(prev->m_topology, expected_topology)) {
          std::error_code ec_copy;
          std::filesystem::copy_file(session_previous_path, session_path, std::filesystem::copy_options::overwrite_existing, ec_copy);
          if (!ec_copy) {
            BOOST_LOG(info) << "Promoted previous session snapshot to current.";
            session_saved.store(true, std::memory_order_release);
            return;
          }
          BOOST_LOG(warning) << "Failed to promote previous ΓåÆ current (copy error); will snapshot current instead.";
        }
      }

      const bool saved = controller.save_display_settings_snapshot_to_file(session_path);
      session_saved.store(saved, std::memory_order_release);
      BOOST_LOG(info) << "Saved session baseline snapshot (fresh) to file: " << (saved ? "true" : "false");
    }

    // Read a stable snapshot: two identical consecutive reads within the deadline
    bool read_stable_snapshot(
      display_device::DisplaySettingsSnapshot &out,
      std::chrono::milliseconds deadline = 2000ms,
      std::chrono::milliseconds interval = 150ms,
      std::stop_token st = {}
    ) {
      auto t0 = std::chrono::steady_clock::now();
      auto have_last = false;
      display_device::DisplaySettingsSnapshot last;
      while (std::chrono::steady_clock::now() - t0 < deadline) {
        if (st.stop_possible() && st.stop_requested()) {
          return false;
        }
        auto cur = controller.snapshot();
        // Heuristic: treat completely empty topology+modes as transient
        const bool emptyish = cur.m_topology.empty() && cur.m_modes.empty();
        if (have_last && !emptyish && (cur == last)) {
          out = std::move(cur);
          return true;
        }
        last = std::move(cur);
        have_last = true;
        if (st.stop_possible() && st.stop_requested()) {
          return false;
        }
        std::this_thread::sleep_for(interval);
      }
      return false;
    }

    void schedule_hdr_blank_if_needed(bool enabled) {
      cancel_hdr_blank();
      if (!enabled) {
        return;
      }
      hdr_blank_thread = std::jthread(&ServiceState::hdr_blank_proc, this);
    }

    void cancel_hdr_blank() {
      if (hdr_blank_thread.joinable()) {
        hdr_blank_thread.request_stop();
        hdr_blank_thread.join();
      }
    }

    static void hdr_blank_proc(std::stop_token st, ServiceState *self) {
      using namespace std::chrono_literals;
      // Fire soon after apply; delay is baked into blank_hdr_states
      if (st.stop_requested()) {
        return;
      }
      // Use fixed 1 second delay per requirements
      self->controller.blank_hdr_states(1000ms);
    }

    // Strict comparator: require full structural equality; allow Unknown==Unknown for HDR
    static bool equal_snapshots_strict(const display_device::DisplaySettingsSnapshot &a, const display_device::DisplaySettingsSnapshot &b) {
      return a == b;
    }

    static std::set<std::string> snapshot_device_set(const display_device::DisplaySettingsSnapshot &s) {
      std::set<std::string> out;
      for (const auto &grp : s.m_topology) {
        for (const auto &id : grp) {
          out.insert(id);
        }
      }
      if (out.empty()) {
        for (const auto &kv : s.m_modes) {
          out.insert(kv.first);
        }
      }
      return out;
    }

    static std::set<std::string> topology_device_set(const display_device::ActiveTopology &topology) {
      std::set<std::string> out;
      for (const auto &grp : topology) {
        out.insert(grp.begin(), grp.end());
      }
      return out;
    }

    bool should_skip_session_snapshot(
      const display_device::SingleDisplayConfiguration &cfg,
      const display_device::DisplaySettingsSnapshot &snap
    ) {
      using Prep = display_device::SingleDisplayConfiguration::DevicePreparation;
      if (cfg.m_device_prep != Prep::EnsureOnlyDisplay) {
        return false;
      }
      auto expected_topology = controller.compute_expected_topology(cfg);
      if (!expected_topology) {
        return false;
      }
      if (!controller.is_topology_the_same(snap.m_topology, *expected_topology)) {
        return false;
      }
      const auto expected_devices = topology_device_set(*expected_topology);
      if (expected_devices.empty()) {
        return false;
      }
      const auto snap_devices = snapshot_device_set(snap);
      if (snap_devices != expected_devices) {
        return false;
      }
      const auto all_devices = controller.enum_all_device_ids();
      for (const auto &id : all_devices) {
        if (!expected_devices.contains(id)) {
          return true;
        }
      }
      return false;
    }

    static bool equal_monitors_only(const display_device::DisplaySettingsSnapshot &a, const display_device::DisplaySettingsSnapshot &b) {
      return snapshot_device_set(a) == snapshot_device_set(b);
    }

    // Quiet period: ensure no changes for the specified duration
    bool quiet_period(
      std::chrono::milliseconds duration = 750ms,
      std::chrono::milliseconds interval = 150ms,
      std::stop_token st = {}
    ) {
      display_device::DisplaySettingsSnapshot base;
      if (!read_stable_snapshot(base, 2000ms, 150ms, st)) {
        return false;
      }
      auto t0 = std::chrono::steady_clock::now();
      while (std::chrono::steady_clock::now() - t0 < duration) {
        if (st.stop_possible() && st.stop_requested()) {
          return false;
        }
        display_device::DisplaySettingsSnapshot cur;
        if (!read_stable_snapshot(cur, 2000ms, 150ms, st)) {
          return false;
        }
        if (!(cur == base)) {
          // topology changed during quiet period
          return false;
        }
        if (st.stop_possible() && st.stop_requested()) {
          return false;
        }
        std::this_thread::sleep_for(interval);
      }
      return true;
    }

    void signal_restore_event(
      const char *reason = nullptr,
      RestoreWindow window = RestoreWindow::Event,
      bool force_start = false
    ) {
      if (!restore_requested.load(std::memory_order_acquire)) {
        return;
      }

      if (force_start || reason) {
        reset_restore_backoff();
      }

      if (!force_start && reason && restore_stage_running.load(std::memory_order_acquire)) {
        BOOST_LOG(debug) << "Dropping restore event while stage loop active: " << reason;
        return;
      }

      const auto now = std::chrono::steady_clock::now();
      const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
      const auto debounce_window_ms = std::chrono::duration_cast<std::chrono::milliseconds>(kRestoreEventDebounce).count();
      const auto window_duration = (window == RestoreWindow::Primary) ? kRestoreWindowPrimary : kRestoreWindowEvent;
      const auto desired_until = now + window_duration;
      const auto desired_until_ms = std::chrono::duration_cast<std::chrono::milliseconds>(desired_until.time_since_epoch()).count();

      bool should_signal = true;

      if (force_start) {
        restore_active_until_ms.store(desired_until_ms, std::memory_order_release);
        restore_active_window.store(window, std::memory_order_release);
        last_restore_event_ms.store(now_ms, std::memory_order_release);
        if (reason) {
          BOOST_LOG(info) << "Restore event signalled: " << reason;
        }
      } else if (reason) {
        const auto last_event = last_restore_event_ms.load(std::memory_order_acquire);
        if (last_event != 0 && (now_ms - last_event) < debounce_window_ms) {
          should_signal = false;
        } else {
          last_restore_event_ms.store(now_ms, std::memory_order_release);
          BOOST_LOG(info) << "Restore event signalled: " << reason;
          const auto current_until_ms = restore_active_until_ms.load(std::memory_order_acquire);
          if (current_until_ms == 0 || now_ms >= current_until_ms || desired_until_ms > current_until_ms) {
            restore_active_until_ms.store(desired_until_ms, std::memory_order_release);
            restore_active_window.store(window, std::memory_order_release);
          }
        }
      }

      if (!should_signal) {
        return;
      }

      {
        std::lock_guard lk(restore_event_mutex);
        restore_event_flag = true;
      }
      restore_event_cv.notify_all();
    }

    bool wait_for_restore_event(std::stop_token st, std::chrono::milliseconds fallback) {
      std::unique_lock lk(restore_event_mutex);
      auto pred = [&]() {
        return restore_event_flag || st.stop_requested();
      };
      if (!restore_event_flag) {
        restore_event_cv.wait_for(lk, fallback, pred);
      }
      if (restore_event_flag) {
        restore_event_flag = false;
        return true;
      }
      return false;
    }

    // Helper to access known-present devices: union of active (modes keys)
    // and all enumerated devices (captures inactive but connected displays).
    std::set<std::string> known_present_devices() {
      std::set<std::string> result;
      try {
        // Active devices (have modes)
        const auto snap = controller.snapshot();
        for (const auto &kv : snap.m_modes) {
          result.insert(kv.first);
        }
        // Enumerated devices (active or inactive)
        const auto all = controller.enum_all_device_ids();
        result.insert(all.begin(), all.end());
        // Fallback to topology flatten if the above produced nothing
        if (result.empty()) {
          for (const auto &grp : snap.m_topology) {
            result.insert(grp.begin(), grp.end());
          }
        }
      } catch (...) {}
      return result;
    }

    // Golden cooldown and device presence pre-checks
    bool should_skip_golden(const display_device::DisplaySettingsSnapshot &golden) {
      const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()
      )
                            .count();
      const auto last_ok = last_session_restore_success_ms.load(std::memory_order_acquire);
      if (last_ok != 0 && (now_ms - last_ok) < 60'000) {
        BOOST_LOG(info) << "Skipping golden: recent session restore success guard active.";
        return true;
      }
      // Ensure all devices in golden exist now
      std::set<std::string> golden_devices;
      for (const auto &grp : golden.m_topology) {
        for (const auto &id : grp) {
          golden_devices.insert(id);
        }
      }
      if (golden_devices.empty()) {
        // be conservative if snapshot malformed
        return true;
      }
      const auto present = known_present_devices();
      for (const auto &id : golden_devices) {
        if (!present.contains(id)) {
          BOOST_LOG(info) << "Skipping golden: device not present: " << id;
          return true;
        }
      }
      return false;
    }

    void clear_session_restore_snapshots_after_golden() {
      std::error_code ec_cur;
      const bool removed_current = std::filesystem::remove(session_current_path, ec_cur);
      std::error_code ec_prev;
      const bool removed_previous = std::filesystem::remove(session_previous_path, ec_prev);
      session_saved.store(false, std::memory_order_release);

      BOOST_LOG(info) << "Golden restore cleanup: removed current=" << (removed_current && !ec_cur ? "true" : "false")
                      << ", previous=" << (removed_previous && !ec_prev ? "true" : "false");

      if (ec_cur) {
        BOOST_LOG(warning) << "Golden restore cleanup: failed to remove current session snapshot '"
                           << session_current_path.string() << "' (ec=" << ec_cur.value() << ")";
      }
      if (ec_prev) {
        BOOST_LOG(warning) << "Golden restore cleanup: failed to remove previous session snapshot '"
                           << session_previous_path.string() << "' (ec=" << ec_prev.value() << ")";
      }
    }

    // Apply the golden snapshot (if available) and verify the system now matches it.
    // Performs up to two attempts (initial + one retry) with short pauses to allow
    // Windows to settle. Returns true only if the post-apply signature exactly
    // matches the golden snapshot signature.
    bool apply_golden_and_confirm(std::stop_token st, uint64_t guard_generation) {
      auto golden = controller.load_display_settings_snapshot(golden_path);
      if (!golden) {
        BOOST_LOG(warning) << "Golden restore snapshot not found; cannot perform revert.";
        return false;
      }
      if (should_skip_golden(*golden)) {
        return false;
      }

      const auto before_sig = controller.signature(controller.snapshot());

      const auto should_cancel = [&]() {
        if (restore_cancel_generation.load(std::memory_order_acquire) != guard_generation) {
          return true;
        }
        if (!restore_requested.load(std::memory_order_acquire)) {
          return true;
        }
        return st.stop_possible() && st.stop_requested();
      };

      // Attempt 1
      if (should_cancel()) {
        return false;
      }
      (void) controller.apply_snapshot(*golden);
      display_device::DisplaySettingsSnapshot cur;
      const bool got_stable = read_stable_snapshot(cur, 2000ms, 150ms, st);
      if (should_cancel()) {
        return false;
      }
      bool ok = got_stable && equal_snapshots_strict(cur, *golden) && quiet_period(750ms, 150ms, st);
      BOOST_LOG(info) << "Golden restore attempt #1: before_sig=" << before_sig
                      << ", current_sig=" << controller.signature(cur)
                      << ", golden_sig=" << controller.signature(*golden)
                      << ", match=" << (ok ? "true" : "false");
      if (ok) {
        BOOST_LOG(info) << "Golden restore confirmed; clearing session restore snapshots.";
        clear_session_restore_snapshots_after_golden();
        return true;
      }

      // Attempt 2 (double-check) after a short delay
      if (should_cancel()) {
        return false;
      }
      if (!wait_with_cancel(st, 700ms, should_cancel)) {
        return false;
      }
      if (should_cancel()) {
        return false;
      }
      (void) controller.apply_snapshot(*golden);
      display_device::DisplaySettingsSnapshot cur2;
      const bool got_stable2 = read_stable_snapshot(cur2, 2000ms, 150ms, st);
      if (should_cancel()) {
        return false;
      }
      ok = got_stable2 && equal_snapshots_strict(cur2, *golden) && quiet_period(750ms, 150ms, st);
      BOOST_LOG(info) << "Golden restore attempt #2: current_sig=" << controller.signature(cur2)
                      << ", golden_sig=" << controller.signature(*golden)
                      << ", match=" << (ok ? "true" : "false");
      if (ok) {
        BOOST_LOG(info) << "Golden restore confirmed (retry); clearing session restore snapshots.";
        clear_session_restore_snapshots_after_golden();
      }
      return ok;
    }

    // Apply a session snapshot (current/previous) and verify the system now matches it.
    bool apply_session_snapshot_from_path(
      const std::filesystem::path &path,
      const char *label,
      std::stop_token st,
      uint64_t guard_generation,
      bool &attempted
    ) {
      attempted = false;
      auto base = controller.load_display_settings_snapshot(path);
      if (!base) {
        BOOST_LOG(info) << (label ? label : "session") << " snapshot not available.";
        return false;
      }
      attempted = true;
      if (auto missing = controller.missing_devices_for_topology(base->m_topology); !missing.empty()) {
        std::string joined;
        for (size_t i = 0; i < missing.size(); ++i) {
          if (i > 0) {
            joined += ", ";
          }
          joined += missing[i];
        }
        BOOST_LOG(info) << (label ? label : "session") << " snapshot skipped (missing devices): [" << joined << "]";
        return false;
      }
      if (!controller.is_topology_valid(*base)) {
        BOOST_LOG(info) << (label ? label : "session") << " snapshot rejected due to invalid topology.";
        return false;
      }

      const auto before_sig = controller.signature(controller.snapshot());

      const auto should_cancel = [&]() {
        if (restore_cancel_generation.load(std::memory_order_acquire) != guard_generation) {
          return true;
        }
        if (!restore_requested.load(std::memory_order_acquire)) {
          return true;
        }
        return st.stop_possible() && st.stop_requested();
      };

      if (should_cancel()) {
        return false;
      }
      (void) controller.apply_snapshot(*base);
      display_device::DisplaySettingsSnapshot cur;
      const bool got_stable = read_stable_snapshot(cur, 2000ms, 150ms, st);
      if (should_cancel()) {
        return false;
      }
      bool ok = got_stable && equal_snapshots_strict(cur, *base) && quiet_period(750ms, 150ms, st);
      BOOST_LOG(info) << "Session restore (" << (label ? label : "session") << ") attempt #1: before_sig="
                      << before_sig << ", current_sig=" << controller.signature(cur)
                      << ", baseline_sig=" << controller.signature(*base)
                      << ", match=" << (ok ? "true" : "false");
      if (!ok) {
        if (should_cancel()) {
          return false;
        }
        if (!wait_with_cancel(st, 700ms, should_cancel)) {
          return false;
        }
        if (should_cancel()) {
          return false;
        }
        (void) controller.apply_snapshot(*base);
        display_device::DisplaySettingsSnapshot cur2;
        const bool got_stable2 = read_stable_snapshot(cur2, 2000ms, 150ms, st);
        if (should_cancel()) {
          return false;
        }
        ok = got_stable2 && equal_snapshots_strict(cur2, *base) && quiet_period(750ms, 150ms, st);
        BOOST_LOG(info) << "Session restore (" << (label ? label : "session")
                        << ") attempt #2: current_sig=" << controller.signature(cur2)
                        << ", baseline_sig=" << controller.signature(*base) << ", match=" << (ok ? "true" : "false");
      }

      if (ok) {
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch()
        )
                              .count();
        last_session_restore_success_ms.store(now_ms, std::memory_order_release);
      }
      return ok;
    }

    // Attempt a restore once if a valid topology is present. Returns true on
    // confirmed success, false otherwise.
    // When always_restore_from_golden is true, golden snapshot is preferred with session
    // snapshots as fallback. Otherwise, prefers session baseline chain, then golden.
    bool try_restore_once_if_valid(std::stop_token st, uint64_t guard_generation) {
      const auto cancelled = [&]() {
        if (restore_cancel_generation.load(std::memory_order_acquire) != guard_generation) {
          return true;
        }
        if (!restore_requested.load(std::memory_order_acquire)) {
          return true;
        }
        return st.stop_possible() && st.stop_requested();
      };

      if (cancelled()) {
        return false;
      }

      const bool golden_first = always_restore_from_golden.load(std::memory_order_acquire);

      // Lambda to try golden restore
      auto try_golden = [&]() -> bool {
        if (cancelled()) {
          return false;
        }
        if (auto golden = controller.load_display_settings_snapshot(golden_path)) {
          if (cancelled()) {
            return false;
          }
          if (auto missing = controller.missing_devices_for_topology(golden->m_topology); !missing.empty()) {
            std::string joined;
            for (size_t i = 0; i < missing.size(); ++i) {
              if (i > 0) {
                joined += ", ";
              }
              joined += missing[i];
            }
            BOOST_LOG(info) << "Golden snapshot skipped (missing devices): [" << joined << "]";
            return false;
          }
          if (controller.validate_topology_with_os(golden->m_topology)) {
            if (apply_golden_and_confirm(st, guard_generation)) {
              return true;
            }
          }
        }
        return false;
      };

      // Lambda to try session snapshots (current then previous)
      auto try_session_snapshots = [&]() -> bool {
        bool attempted_current = false;
        const bool restored_current = apply_session_snapshot_from_path(
          session_current_path,
          "current",
          st,
          guard_generation,
          attempted_current
        );
        if (restored_current) {
          (void) promote_current_snapshot_to_previous("restore success");
          return true;
        }

        bool attempted_previous = false;
        const bool restored_previous = apply_session_snapshot_from_path(
          session_previous_path,
          "previous",
          st,
          guard_generation,
          attempted_previous
        );
        if (restored_previous) {
          if (attempted_current) {
            std::error_code ec_rm_bad;
            (void) std::filesystem::remove(session_current_path, ec_rm_bad);
          }
          return true;
        }
        (void) attempted_previous;
        return false;
      };

      if (golden_first) {
        // Prefer golden snapshot, fallback to session snapshots
        BOOST_LOG(info) << "Restore: using golden-first strategy (always_restore_from_golden=true)";
        if (try_golden()) {
          return true;
        }
        // Golden failed, try session snapshots as fallback
        return try_session_snapshots();
      } else {
        // Default: prefer session snapshots, fallback to golden
        if (try_session_snapshots()) {
          return true;
        }
        return try_golden();
      }
    }

    // Start a background polling loop that checks every ~3s whether the
    // requested restore topology is valid; if so, perform the restore and
    // confirm success. Logging is throttled (~15 minutes) to avoid noise.
    void ensure_restore_polling(
      RestoreWindow window = RestoreWindow::Primary,
      const char *reason = "initial",
      bool force_start = true
    ) {
      if (!restore_requested.load(std::memory_order_acquire)) {
        return;
      }

      bool pump_expected = false;
      if (event_pump_running.compare_exchange_strong(pump_expected, true, std::memory_order_acq_rel)) {
        event_pump.start([this](const char *event_reason) {
          if (!restore_requested.load(std::memory_order_acquire)) {
            return;
          }
          const char *why = event_reason ? event_reason : "event";
          if (!restore_poll_active.load(std::memory_order_acquire)) {
            ensure_restore_polling(RestoreWindow::Event, why, true);
          } else {
            signal_restore_event(why, RestoreWindow::Event);
          }
        });
      }

      bool expected = false;
      if (!restore_poll_active.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        const char *label = reason ? reason : ((window == RestoreWindow::Primary) ? "initial" : "event");
        signal_restore_event(label, window, force_start);
        BOOST_LOG(debug) << "Restore loop already active; window updated to "
                         << ((window == RestoreWindow::Primary) ? "primary" : "event");
        return;
      }

      const char *label = reason ? reason : ((window == RestoreWindow::Primary) ? "initial" : "event");
      signal_restore_event(label, window, force_start);
      restore_poll_thread = std::jthread(&ServiceState::restore_poll_proc, this);
    }

    void stop_restore_polling() {
      restore_poll_active.store(false, std::memory_order_release);
      request_restore_cancel();
      event_pump.stop();
      event_pump_running.store(false, std::memory_order_release);
      reset_restore_backoff();
      restore_active_until_ms.store(0, std::memory_order_release);
      last_restore_event_ms.store(0, std::memory_order_release);
      restore_active_window.store(RestoreWindow::Event, std::memory_order_release);
      restore_stage_running.store(false, std::memory_order_release);
      stop_and_join(restore_poll_thread, "restore-poll");
      restore_requested.store(false, std::memory_order_release);
      restore_origin_epoch.store(0, std::memory_order_release);
    }

    void disarm_restore_requests(const char *reason = nullptr) {
      const bool had_pending = restore_requested.load(std::memory_order_acquire);
      stop_restore_polling();
      cancel_delayed_reapply();
      cancel_post_apply_tasks();
      delete_restore_scheduled_task();
      direct_revert_bypass_grace.store(false, std::memory_order_release);
      exit_after_revert.store(false, std::memory_order_release);
      retry_apply_on_topology.store(false, std::memory_order_release);
      retry_revert_on_topology.store(false, std::memory_order_release);
      if (reason) {
        BOOST_LOG(info) << reason << " (pending_restore=" << (had_pending ? "true" : "false") << ")";
      } else if (had_pending) {
        BOOST_LOG(info) << "Restore requests disarmed.";
      }
    }

    uint64_t begin_connection_epoch() {
      const auto epoch = next_connection_epoch.fetch_add(1, std::memory_order_acq_rel);
      active_connection_epoch.store(epoch, std::memory_order_release);
      return epoch;
    }

    uint64_t current_connection_epoch() const {
      return active_connection_epoch.load(std::memory_order_acquire);
    }

    bool is_connection_epoch_current(uint64_t epoch) const {
      return current_connection_epoch() == epoch;
    }

    void clear_restore_origin() {
      restore_origin_epoch.store(0, std::memory_order_release);
    }

    bool should_exit_after_restore() const {
      const auto origin = restore_origin_epoch.load(std::memory_order_acquire);
      if (origin == 0) {
        return true;
      }
      return origin == current_connection_epoch();
    }

    static void restore_poll_proc(std::stop_token st, ServiceState *self) {
      using namespace std::chrono_literals;
      const auto kPoll = 3s;
      const auto kLogThrottle = std::chrono::minutes(15);
      auto last_log = std::chrono::steady_clock::now() - kLogThrottle;  // allow immediate log
      const auto guard_generation = self->restore_cancel_generation.load(std::memory_order_acquire);
      auto cancelled = [&]() {
        if (st.stop_requested()) {
          return true;
        }
        if (self->restore_cancel_generation.load(std::memory_order_acquire) != guard_generation) {
          return true;
        }
        if (!self->restore_requested.load(std::memory_order_acquire)) {
          return true;
        }
        return false;
      };

      auto run_restore_cleanup = [&](const char *context) {
        bool allow_cleanup = !cancelled();
        if (allow_cleanup) {
          refresh_shell_after_display_change();
          allow_cleanup = !cancelled();
        }
        if (allow_cleanup) {
          delete_restore_scheduled_task();
        } else {
          BOOST_LOG(debug) << "Restore cleanup skipped"
                           << (context ? std::string(" (") + context + ")" : "")
                           << " due to cancellation.";
        }
      };

      // If there is no session or golden snapshot, there is nothing to restore.
      try {
        std::error_code ec1, ec2;
        const bool has_session = std::filesystem::exists(self->session_current_path, ec1);
        std::error_code ec_prev;
        const bool has_previous = std::filesystem::exists(self->session_previous_path, ec_prev);
        const bool has_golden = std::filesystem::exists(self->golden_path, ec2);
        if (!has_session && !has_previous && !has_golden) {
          BOOST_LOG(info) << "Restore polling: no session/previous or golden snapshot present; exiting helper.";
          if (self->running_flag) {
            self->running_flag->store(false, std::memory_order_release);
          }
          self->event_pump.stop();
          self->event_pump_running.store(false, std::memory_order_release);
          self->restore_poll_active.store(false, std::memory_order_release);
          self->restore_requested.store(false, std::memory_order_release);
          self->clear_restore_origin();
          return;
        }
    } catch (...) {
      // fall through
    }

    if (cancelled()) {
      self->restore_stage_running.store(false, std::memory_order_release);
      self->restore_poll_active.store(false, std::memory_order_release);
      return;
    }

    // Initial one-shot attempt before entering the loop
    bool initial_attempted = false;
    bool initial_success = false;
    try {
      if (!cancelled() && self->await_restore_backoff(st) && !cancelled()) {
        initial_attempted = true;
        initial_success = self->try_restore_once_if_valid(st, guard_generation);
      }
    } catch (...) {}

    if (initial_success) {
      if (cancelled()) {
        self->event_pump.stop();
        self->event_pump_running.store(false, std::memory_order_release);
        self->restore_poll_active.store(false, std::memory_order_release);
        self->restore_active_until_ms.store(0, std::memory_order_release);
        self->restore_active_window.store(RestoreWindow::Event, std::memory_order_release);
        self->last_restore_event_ms.store(0, std::memory_order_release);
        self->restore_requested.store(false, std::memory_order_release);
        self->clear_restore_origin();
        return;
      }
      self->reset_restore_backoff();
      self->retry_revert_on_topology.store(false, std::memory_order_release);
      run_restore_cleanup("initial attempt");

      if (cancelled()) {
        self->event_pump.stop();
        self->event_pump_running.store(false, std::memory_order_release);
        self->restore_poll_active.store(false, std::memory_order_release);
        self->restore_active_until_ms.store(0, std::memory_order_release);
        self->restore_active_window.store(RestoreWindow::Event, std::memory_order_release);
        self->last_restore_event_ms.store(0, std::memory_order_release);
        self->restore_requested.store(false, std::memory_order_release);
        self->clear_restore_origin();
        return;
      }

      const bool exit_helper = self->should_exit_after_restore();
      if (exit_helper && self->running_flag) {
        BOOST_LOG(info) << "Restore confirmed (initial attempt); exiting helper.";
        self->running_flag->store(false, std::memory_order_release);
      } else if (!exit_helper) {
        BOOST_LOG(info) << "Restore confirmed (initial attempt); keeping helper alive for newer connection.";
      }
      self->event_pump.stop();
      self->event_pump_running.store(false, std::memory_order_release);
      self->restore_poll_active.store(false, std::memory_order_release);
      self->restore_active_until_ms.store(0, std::memory_order_release);
      self->restore_active_window.store(RestoreWindow::Event, std::memory_order_release);
      self->last_restore_event_ms.store(0, std::memory_order_release);
      self->restore_requested.store(false, std::memory_order_release);
      self->clear_restore_origin();
      return;
    }

      if (initial_attempted && !initial_success) {
        self->register_restore_failure();
      }

      bool exit_due_to_timeout = false;
      while (!cancelled()) {
        const auto now = std::chrono::steady_clock::now();
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        auto active_until_ms = self->restore_active_until_ms.load(std::memory_order_acquire);
        const auto active_window_kind = self->restore_active_window.load(std::memory_order_acquire);
        bool active_window = (active_until_ms != 0 && now_ms <= active_until_ms);
        bool window_expired = false;
        if (!active_window && active_until_ms != 0 && now_ms > active_until_ms) {
          window_expired = true;
          self->restore_active_until_ms.store(0, std::memory_order_release);
          self->restore_active_window.store(RestoreWindow::Event, std::memory_order_release);
        }

        const auto wait_timeout = active_window ? 500ms : kPoll;

        bool triggered = false;
        try {
          triggered = self->wait_for_restore_event(st, wait_timeout);
        } catch (...) {}
        if (!triggered && active_window && active_window_kind == RestoreWindow::Primary) {
          triggered = true;
        }
        if (cancelled()) {
          break;
        }
        if (!triggered) {
          if (window_expired) {
            const char *window_label = (active_window_kind == RestoreWindow::Primary) ? "primary" : "event";
            BOOST_LOG(info) << "Restore polling: " << window_label
                            << " window exhausted; pausing attempts until next event.";
            exit_due_to_timeout = true;
            break;
          }
          const auto now2 = std::chrono::steady_clock::now();
          if (now2 - last_log >= kLogThrottle) {
            last_log = now2;
            BOOST_LOG(info) << "Restore polling: waiting for event-driven topology changes.";
          }
          continue;
        }

        if (!self->await_restore_backoff(st)) {
          break;
        }
        if (cancelled()) {
          break;
        }

        const auto window_deadline_ms = self->restore_active_until_ms.load(std::memory_order_acquire);
        self->restore_stage_running.store(true, std::memory_order_release);
        bool success = false;
        try {
          success = self->try_restore_once_if_valid(st, guard_generation);
        } catch (...) {
          self->restore_stage_running.store(false, std::memory_order_release);
          throw;
        }

        self->restore_stage_running.store(false, std::memory_order_release);
        if (cancelled()) {
          break;
        }

        if (success) {
          if (cancelled()) {
            self->event_pump.stop();
            self->event_pump_running.store(false, std::memory_order_release);
            self->restore_poll_active.store(false, std::memory_order_release);
            self->restore_active_until_ms.store(0, std::memory_order_release);
            self->restore_active_window.store(RestoreWindow::Event, std::memory_order_release);
            self->last_restore_event_ms.store(0, std::memory_order_release);
            self->restore_requested.store(false, std::memory_order_release);
            self->clear_restore_origin();
            return;
          }
          self->reset_restore_backoff();
          self->retry_revert_on_topology.store(false, std::memory_order_release);
          run_restore_cleanup("polling attempt");

          if (cancelled()) {
            self->event_pump.stop();
            self->event_pump_running.store(false, std::memory_order_release);
            self->restore_poll_active.store(false, std::memory_order_release);
            self->restore_active_until_ms.store(0, std::memory_order_release);
            self->restore_active_window.store(RestoreWindow::Event, std::memory_order_release);
            self->last_restore_event_ms.store(0, std::memory_order_release);
            self->restore_requested.store(false, std::memory_order_release);
            self->clear_restore_origin();
            return;
          }

          const bool exit_helper = self->should_exit_after_restore();
          if (exit_helper && self->running_flag) {
            BOOST_LOG(info) << "Restore confirmed; exiting helper.";
            self->running_flag->store(false, std::memory_order_release);
          } else if (!exit_helper) {
            BOOST_LOG(info) << "Restore confirmed while newer connection active; helper remains running.";
          }
          self->restore_poll_active.store(false, std::memory_order_release);
          self->event_pump.stop();
          self->event_pump_running.store(false, std::memory_order_release);
          self->restore_active_until_ms.store(0, std::memory_order_release);
          self->restore_active_window.store(RestoreWindow::Event, std::memory_order_release);
          self->last_restore_event_ms.store(0, std::memory_order_release);
          self->restore_requested.store(false, std::memory_order_release);
          self->clear_restore_origin();
          return;
        }

        self->register_restore_failure();

        const auto post_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch()
        )
                               .count();
        if (window_deadline_ms != 0 && post_ms > window_deadline_ms) {
          self->restore_active_until_ms.store(0, std::memory_order_release);
          self->restore_active_window.store(RestoreWindow::Event, std::memory_order_release);
        }

      }
      self->restore_stage_running.store(false, std::memory_order_release);
      self->restore_poll_active.store(false, std::memory_order_release);
      self->restore_active_until_ms.store(0, std::memory_order_release);
      self->restore_active_window.store(RestoreWindow::Event, std::memory_order_release);
      self->last_restore_event_ms.store(0, std::memory_order_release);
      self->reset_restore_backoff();

      if (exit_due_to_timeout) {
        return;
      }

      self->event_pump.stop();
      self->event_pump_running.store(false, std::memory_order_release);
      self->restore_requested.store(false, std::memory_order_release);
      self->clear_restore_origin();
    }

    void on_topology_changed() {
      // Re-apply path
      if (retry_apply_on_topology.load(std::memory_order_acquire) && last_cfg) {
        BOOST_LOG(info) << "Topology changed: reattempting apply";
        if (controller.apply(*last_cfg)) {
          retry_apply_on_topology.store(false, std::memory_order_release);
          refresh_shell_after_display_change();
        }
        return;
      }

      // Revert/restore path is handled by restore polling loop now.
      (void) 0;
    }

    // Schedule delayed re-apply attempts to work around Windows sometimes forcing native
    // resolution immediately after activating a display. The provided delays represent
    // the windows (relative to now) when verification/re-apply should be attempted.
    void schedule_delayed_reapply(std::vector<std::chrono::milliseconds> delays = {250ms, 750ms}) {
      if (delayed_reapply_thread.joinable()) {
        delayed_reapply_thread.request_stop();
        delayed_reapply_thread.join();
      }
      if (!last_cfg || delays.empty()) {
        return;
      }
      delayed_reapply_thread = std::jthread(&ServiceState::delayed_reapply_proc, this, std::move(delays));
    }

    void cancel_delayed_reapply() {
      if (delayed_reapply_thread.joinable()) {
        delayed_reapply_thread.request_stop();
        delayed_reapply_thread.join();
      }
    }

    void stop_and_async_join(std::jthread &thread, const char *label) {
      if (!thread.joinable()) {
        return;
      }
      thread.request_stop();
      std::jthread joiner([label, t = std::move(thread)]() mutable {
        const auto start = std::chrono::steady_clock::now();
        if (t.joinable()) {
          t.join();
          const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start
          );
          BOOST_LOG(debug) << "Async join completed for " << (label ? label : "thread")
                           << " after " << elapsed.count() << "ms";
        }
      });
      {
        std::lock_guard<std::mutex> lg(async_join_mutex);
        async_join_threads.emplace_back(std::move(joiner));
      }
    }

    void stop_and_join(std::jthread &thread, const char *label) {
      if (!thread.joinable()) {
        return;
      }
      thread.request_stop();
      const auto start = std::chrono::steady_clock::now();
      thread.join();
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start
      );
      BOOST_LOG(debug) << "Join completed for " << (label ? label : "thread")
                       << " after " << elapsed.count() << "ms";
    }

    static bool wait_with_stop(std::stop_token st, std::chrono::milliseconds duration) {
      using namespace std::chrono_literals;
      constexpr auto step = 50ms;
      auto remaining = duration;
      while (remaining > std::chrono::milliseconds::zero()) {
        if (st.stop_requested()) {
          return false;
        }
        const auto slice = remaining > step ? step : remaining;
        std::this_thread::sleep_for(slice);
        remaining -= slice;
      }
      return !st.stop_requested();
    }

    template <typename CancelPredicate>
    static bool wait_with_cancel(std::stop_token st, std::chrono::milliseconds duration, CancelPredicate cancelled) {
      using namespace std::chrono_literals;
      constexpr auto step = 50ms;
      auto remaining = duration;
      while (remaining > std::chrono::milliseconds::zero()) {
        if (st.stop_requested() || cancelled()) {
          return false;
        }
        const auto slice = remaining > step ? step : remaining;
        std::this_thread::sleep_for(slice);
        remaining -= slice;
      }
      return !(st.stop_requested() || cancelled());
    }

    static void delayed_reapply_proc(std::stop_token st, ServiceState *self, std::vector<std::chrono::milliseconds> delays) {
      for (auto delay : delays) {
        if (!wait_with_stop(st, delay)) {
          return;
        }
        if (self->verify_last_configuration_sticky(kVerificationSettleDelay, st)) {
          continue;
        }
        BOOST_LOG(info) << "Delayed re-apply attempt after activation 213Q902";
        self->best_effort_apply_last_cfg();
      }
    }

    void best_effort_apply_last_cfg() {
      try {
        if (last_cfg) {
          (void) controller.apply(*last_cfg);
          refresh_shell_after_display_change();
        }
      } catch (...) {}
    }

    bool verify_last_configuration_sticky(std::chrono::milliseconds settle_delay = kVerificationSettleDelay, std::stop_token st = {}) {
      if (!last_cfg) {
        return true;
      }
      auto matches = [&]() {
        return controller.configuration_matches_current_state(*last_cfg);
      };
      if (!matches()) {
        return false;
      }
      if (settle_delay > std::chrono::milliseconds::zero()) {
        if (!wait_with_stop(st, settle_delay)) {
          return false;
        }
        return matches();
      }
      return true;
    }

    bool configuration_matches_last() const {
      if (!last_cfg) {
        return true;
      }
      return controller.configuration_matches_current_state(*last_cfg);
    }

    void cancel_post_apply_tasks() {
      stop_and_join(post_apply_thread, "post-apply");
    }

    void schedule_post_apply_tasks(
      bool enforce_snapshot,
      std::optional<std::string> before_sig,
      bool wa_hdr_toggle,
      std::optional<std::string> requested_virtual_layout,
      std::vector<std::pair<std::string, display_device::Point>> monitor_position_overrides,
      std::vector<std::chrono::milliseconds> reapply_delays
    ) {
      cancel_post_apply_tasks();
      post_apply_thread = std::jthread(
        [this,
        enforce_snapshot,
         before_sig = std::move(before_sig),
         wa_hdr_toggle,
         requested_virtual_layout = std::move(requested_virtual_layout),
         monitor_position_overrides = std::move(monitor_position_overrides),
         reapply_delays = std::move(reapply_delays)
        ](std::stop_token st) mutable {
          const auto apply_epoch = current_connection_epoch();
          auto cancelled = [&]() {
            return st.stop_requested() || !is_connection_epoch_current(apply_epoch);
          };
          if (cancelled()) {
            return;
          }

          if (enforce_snapshot && before_sig) {
            display_device::DisplaySettingsSnapshot cur;
            const bool got_stable = read_stable_snapshot(
              cur,
              std::chrono::milliseconds(600),
              std::chrono::milliseconds(75),
              st
            );
            (void) got_stable;
          }

          if (cancelled()) {
            return;
          }
          retry_apply_on_topology.store(false, std::memory_order_release);
          if (!reapply_delays.empty()) {
            if (cancelled()) {
              return;
            }
            schedule_delayed_reapply(std::move(reapply_delays));
          }
          if (cancelled()) {
            return;
          }
          refresh_shell_after_display_change();
          if (cancelled()) {
            return;
          }
          schedule_hdr_blank_if_needed(wa_hdr_toggle);
          if (cancelled()) {
            return;
          }

          if (requested_virtual_layout) {
            BOOST_LOG(info) << "Display helper: requested virtual display layout=" << *requested_virtual_layout;
          }

          if (cancelled()) {
            return;
          }
          if (!monitor_position_overrides.empty()) {
            bool reposition_result = true;
            for (const auto &[device_id, origin] : monitor_position_overrides) {
              if (cancelled()) {
                break;
              }
              if (device_id.empty()) {
                continue;
              }
              const bool ok_origin = controller.set_display_origin(device_id, origin);
              reposition_result = reposition_result && ok_origin;
            }
            if (cancelled()) {
              return;
            }
            BOOST_LOG(info) << "Display helper: monitor position overrides applied result=" << (reposition_result ? "true" : "false");
          }
        }
      );
    }
  };

}  // namespace

// Utilities to reduce main() complexity
namespace {
  HANDLE make_named_mutex(const wchar_t *name) {
    SECURITY_ATTRIBUTES sa {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    return CreateMutexW(&sa, FALSE, name);
  }

  bool ensure_single_instance(HANDLE &out_handle) {
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

  std::filesystem::path compute_log_dir() {
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

  bool create_restore_scheduled_task() {
    BOOST_LOG(info) << "Attempting to create scheduled task 'VibeshineDisplayRestore'...";

    const DWORD active_session_id = WTSGetActiveConsoleSessionId();

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to initialize COM for Task Scheduler: 0x" << std::hex << hr;
      return false;
    }

    ITaskService *service = nullptr;
    hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService, (void **) &service);
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to create Task Scheduler service instance: 0x" << std::hex << hr;
      CoUninitialize();
      return false;
    }

    hr = service->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t());
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to connect to Task Scheduler service: 0x" << std::hex << hr;
      service->Release();
      CoUninitialize();
      return false;
    }

    ITaskFolder *root_folder = nullptr;
    hr = service->GetFolder(_bstr_t(L"\\"), &root_folder);
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to get root task folder: 0x" << std::hex << hr;
      service->Release();
      CoUninitialize();
      return false;
    }

    ITaskDefinition *task = nullptr;
    hr = service->NewTask(0, &task);
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to create new task definition: 0x" << std::hex << hr;
      root_folder->Release();
      service->Release();
      CoUninitialize();
      return false;
    }

    IRegistrationInfo *reg_info = nullptr;
    hr = task->get_RegistrationInfo(&reg_info);
    if (SUCCEEDED(hr)) {
      reg_info->put_Author(_bstr_t(L"Sunshine Display Helper"));
      reg_info->put_Description(_bstr_t(L"Automatically restores display settings after reboot"));
      reg_info->Release();
    }

    ITaskSettings *settings = nullptr;
    hr = task->get_Settings(&settings);
    if (SUCCEEDED(hr)) {
      settings->put_StartWhenAvailable(VARIANT_TRUE);
      settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
      settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
      settings->put_ExecutionTimeLimit(_bstr_t(L"PT0S"));
      settings->put_Hidden(VARIANT_TRUE);
      settings->Release();
    }

    std::wstring username = query_session_account(active_session_id);

    if (username.empty()) {
      DWORD sam_required = 0;
      if (!GetUserNameExW(NameSamCompatible, nullptr, &sam_required) && GetLastError() == ERROR_MORE_DATA && sam_required > 0) {
        std::wstring sam_name;
        sam_name.resize(sam_required);
        DWORD sam_size = sam_required;
        if (GetUserNameExW(NameSamCompatible, sam_name.data(), &sam_size)) {
          sam_name.resize(sam_size);
          username = std::move(sam_name);
        }
      }
    }

    if (username.empty()) {
      wchar_t fallback[UNLEN + 1] = {0};
      DWORD fallback_len = UNLEN + 1;
      if (GetUserNameW(fallback, &fallback_len) && fallback_len > 0) {
        username.assign(fallback);
      }
    }

    bool has_username = !username.empty();
    if (has_username) {
      if (_wcsicmp(username.c_str(), L"SYSTEM") == 0 || _wcsicmp(username.c_str(), L"NT AUTHORITY\\SYSTEM") == 0) {
        BOOST_LOG(warning) << "Resolved session identity is SYSTEM; skipping per-user task registration";
        has_username = false;
      }
    } else {
      BOOST_LOG(warning) << "Failed to get current username, using empty user for task";
    }

    const std::wstring task_name = build_restore_task_name(has_username ? username : std::wstring {});

    wchar_t exe_path[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe_path, MAX_PATH)) {
      BOOST_LOG(error) << "Failed to get current executable path";
      task->Release();
      root_folder->Release();
      service->Release();
      CoUninitialize();
      return false;
    }

    ITriggerCollection *trigger_collection = nullptr;
    hr = task->get_Triggers(&trigger_collection);
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to get trigger collection: " << std::hex << hr;
      task->Release();
      root_folder->Release();
      service->Release();
      CoUninitialize();
      return false;
    }

    ITrigger *trigger = nullptr;
    hr = trigger_collection->Create(TASK_TRIGGER_LOGON, &trigger);
    trigger_collection->Release();
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to create logon trigger: " << std::hex << hr;
      task->Release();
      root_folder->Release();
      service->Release();
      CoUninitialize();
      return false;
    }

    ILogonTrigger *logon_trigger = nullptr;
    hr = trigger->QueryInterface(IID_ILogonTrigger, (void **) &logon_trigger);
    trigger->Release();
    if (SUCCEEDED(hr)) {
      logon_trigger->put_Id(_bstr_t(L"SunshineDisplayHelperLogonTrigger"));
      logon_trigger->put_Enabled(VARIANT_TRUE);
      if (has_username) {
        logon_trigger->put_UserId(_bstr_t(username.c_str()));
      }
      logon_trigger->Release();
    }

    IActionCollection *action_collection = nullptr;
    hr = task->get_Actions(&action_collection);
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to get action collection: " << std::hex << hr;
      task->Release();
      root_folder->Release();
      service->Release();
      CoUninitialize();
      return false;
    }

    IAction *action = nullptr;
    hr = action_collection->Create(TASK_ACTION_EXEC, &action);
    action_collection->Release();
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to create exec action: " << std::hex << hr;
      task->Release();
      root_folder->Release();
      service->Release();
      CoUninitialize();
      return false;
    }

    IExecAction *exec_action = nullptr;
    hr = action->QueryInterface(IID_IExecAction, (void **) &exec_action);
    action->Release();
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to query IExecAction interface: " << std::hex << hr;
      task->Release();
      root_folder->Release();
      service->Release();
      CoUninitialize();
      return false;
    }

    exec_action->put_Path(_bstr_t(exe_path));
    exec_action->put_Arguments(_bstr_t(L"--restore"));
    exec_action->Release();

    IPrincipal *principal = nullptr;
    hr = task->get_Principal(&principal);
    if (SUCCEEDED(hr)) {
      principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
      principal->put_RunLevel(TASK_RUNLEVEL_LUA);
      principal->Release();
    }

    IRegisteredTask *registered_task = nullptr;
    HRESULT registration_hr = root_folder->RegisterTaskDefinition(
      _bstr_t(task_name.c_str()),
      task,
      TASK_CREATE_OR_UPDATE,
      _variant_t(),
      _variant_t(),
      TASK_LOGON_INTERACTIVE_TOKEN,
      _variant_t(L""),
      &registered_task
    );

    if (registered_task) {
      registered_task->Release();
    }

    task->Release();
    root_folder->Release();
    service->Release();

    if (FAILED(registration_hr)) {
      BOOST_LOG(error) << "Failed to register scheduled task: " << std::hex << registration_hr;
      CoUninitialize();
      return false;
    }

    BOOST_LOG(info) << "Successfully created scheduled task '" << std::string(task_name.begin(), task_name.end()) << "'";
    CoUninitialize();
    return true;
  }

  bool delete_restore_scheduled_task() {
    BOOST_LOG(info) << "Attempting to delete restore helper scheduled tasks";

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to initialize COM for Task Scheduler deletion: 0x" << std::hex << hr;
      return false;
    }

    ITaskService *service = nullptr;
    hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService, (void **) &service);
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to create Task Scheduler service instance for deletion: 0x" << std::hex << hr;
      CoUninitialize();
      return false;
    }

    hr = service->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t());
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to connect to Task Scheduler service for deletion: 0x" << std::hex << hr;
      service->Release();
      CoUninitialize();
      return false;
    }

    ITaskFolder *root_folder = nullptr;
    hr = service->GetFolder(_bstr_t(L"\\"), &root_folder);
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to get root task folder for deletion: " << std::hex << hr;
      service->Release();
      CoUninitialize();
      return false;
    }

    const DWORD active_session_id = WTSGetActiveConsoleSessionId();
    std::wstring username = query_session_account(active_session_id);

    if (username.empty()) {
      DWORD sam_required = 0;
      if (!GetUserNameExW(NameSamCompatible, nullptr, &sam_required) && GetLastError() == ERROR_MORE_DATA && sam_required > 0) {
        std::wstring sam_name;
        sam_name.resize(sam_required);
        DWORD sam_size = sam_required;
        if (GetUserNameExW(NameSamCompatible, sam_name.data(), &sam_size)) {
          sam_name.resize(sam_size);
          username = std::move(sam_name);
        }
      }
    }

    if (username.empty()) {
      wchar_t fallback[UNLEN + 1] = {0};
      DWORD fallback_len = UNLEN + 1;
      if (GetUserNameW(fallback, &fallback_len) && fallback_len > 0) {
        username.assign(fallback);
      }
    }

    std::vector<std::wstring> task_names;
    task_names.push_back(build_restore_task_name({}));

    if (!username.empty()) {
      if (_wcsicmp(username.c_str(), L"SYSTEM") != 0 && _wcsicmp(username.c_str(), L"NT AUTHORITY\\SYSTEM") != 0) {
        task_names.push_back(build_restore_task_name(username));
      }
    }

    bool success = true;
    for (const auto &name : task_names) {
      const HRESULT delete_hr = root_folder->DeleteTask(_bstr_t(name.c_str()), 0);
      if (SUCCEEDED(delete_hr)) {
        BOOST_LOG(info) << "Removed scheduled task '" << std::string(name.begin(), name.end()) << "'";
        continue;
      }

      if (delete_hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
        BOOST_LOG(debug) << "Scheduled task '" << std::string(name.begin(), name.end()) << "' not found";
        continue;
      }

      BOOST_LOG(error) << "Failed to delete scheduled task '" << std::string(name.begin(), name.end())
                       << "': 0x" << std::hex << delete_hr;
      success = false;
    }

    root_folder->Release();
    service->Release();
    CoUninitialize();

    return success;
  }

  void hide_console_window() {
    HWND console = GetConsoleWindow();
    if (console) {
      ShowWindow(console, SW_HIDE);
    }
  }

  bool validate_session_snapshot(ServiceState &state, const std::filesystem::path &path) {
    auto snap = state.controller.load_display_settings_snapshot(path);
    if (!snap) {
      BOOST_LOG(warning) << "Existing session snapshot could not be parsed; removing path=" << path.string();
    } else if (!state.controller.is_topology_valid(*snap) || snap->m_modes.empty()) {
      BOOST_LOG(warning) << "Existing session snapshot is invalid (topology or modes missing); removing path="
                         << path.string();
      } else {
      // Filter out devices not currently enumerated and devices on the exclusion list.
      auto devices = state.controller.enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
      std::set<std::string> valid_device_ids_norm;
      std::set<std::string> exclusions_norm;
      auto normalize = [](std::string id) {
        id.erase(id.begin(), std::find_if(id.begin(), id.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        id.erase(std::find_if(id.rbegin(), id.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), id.end());
        std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return id;
      };
      for (auto id : state.controller.snapshot_exclusions_copy_public()) {
        exclusions_norm.insert(normalize(std::move(id)));
      }
      for (const auto &d : devices) {
        const auto id = d.m_device_id.empty() ? d.m_display_name : d.m_device_id;
        if (!id.empty()) {
          valid_device_ids_norm.insert(normalize(id));
        }
      }

      auto is_allowed = [&](const std::string &device_id) {
        const auto norm = normalize(device_id);
        if (!valid_device_ids_norm.count(norm)) {
          return false;
        }
        return exclusions_norm.empty() || !exclusions_norm.count(norm);
      };

      // Filter topology groups, removing devices that are not currently enumerated or are excluded.
      display_device::ActiveTopology filtered_topology;
      std::vector<std::string> filtered_out_excluded;
      for (const auto &grp : snap->m_topology) {
        std::vector<std::string> filtered_grp;
        for (const auto &device_id : grp) {
          if (is_allowed(device_id)) {
            filtered_grp.push_back(device_id);
          } else if (!exclusions_norm.empty() && exclusions_norm.count(normalize(device_id))) {
            filtered_out_excluded.push_back(device_id);
          }
        }
        if (!filtered_grp.empty()) {
          filtered_topology.push_back(std::move(filtered_grp));
        }
      }

      if (filtered_topology.empty()) {
        BOOST_LOG(warning) << "Existing session snapshot rejected: no applicable devices remain after filtering; removing path="
                           << path.string();
      } else {
        // Check if filtering changed anything
        if (filtered_topology != snap->m_topology || !filtered_out_excluded.empty()) {
          BOOST_LOG(info) << "Filtering devices from session snapshot: " << path.string();

          // Update snapshot with filtered data
          snap->m_topology = std::move(filtered_topology);

          // Filter modes and hdr_states to only include allowed devices
          for (auto it = snap->m_modes.begin(); it != snap->m_modes.end();) {
            if (!is_allowed(it->first)) {
              it = snap->m_modes.erase(it);
            } else {
              ++it;
            }
          }
          for (auto it = snap->m_hdr_states.begin(); it != snap->m_hdr_states.end();) {
            if (!is_allowed(it->first)) {
              it = snap->m_hdr_states.erase(it);
            } else {
              ++it;
            }
          }

          // Clear primary if it was filtered out
          if (!snap->m_primary_device.empty() && !is_allowed(snap->m_primary_device)) {
            snap->m_primary_device.clear();
          }

          if (!filtered_out_excluded.empty()) {
            std::sort(filtered_out_excluded.begin(), filtered_out_excluded.end());
            filtered_out_excluded.erase(std::unique(filtered_out_excluded.begin(), filtered_out_excluded.end()), filtered_out_excluded.end());
            std::string joined;
            for (size_t i = 0; i < filtered_out_excluded.size(); ++i) {
              if (i > 0) {
                joined += ", ";
              }
              joined += filtered_out_excluded[i];
            }
            BOOST_LOG(info) << "Excluded devices removed from session snapshot: [" << joined << "]";
          }

          // Re-save filtered snapshot (best effort, don't fail validation if save fails)
          (void) state.controller.save_snapshot_to_file(*snap, path);
        }
        return true;
      }
    }

    std::error_code ec_rm;
    std::filesystem::remove(path, ec_rm);
    return false;
  }

  std::vector<std::string> parse_snapshot_exclude_json_node(const nlohmann::json &node) {
    std::vector<std::string> ids;
    const nlohmann::json *arr = &node;
    nlohmann::json nested;
    if (node.is_object()) {
      if (node.contains("exclude_devices")) {
        nested = node["exclude_devices"];
        arr = &nested;
      } else if (node.contains("devices")) {
        nested = node["devices"];
        arr = &nested;
      }
    }
    if (!arr->is_array()) {
      return ids;
    }
    for (const auto &el : *arr) {
      if (el.is_string()) {
        ids.push_back(el.get<std::string>());
      } else if (el.is_object()) {
        if (el.contains("device_id") && el["device_id"].is_string()) {
          ids.push_back(el["device_id"].get<std::string>());
        } else if (el.contains("id") && el["id"].is_string()) {
          ids.push_back(el["id"].get<std::string>());
        }
      }
    }
    return ids;
  }

  std::optional<std::vector<std::string>> parse_snapshot_exclude_payload(std::span<const uint8_t> payload) {
    if (payload.empty()) {
      return std::nullopt;
    }
    try {
      std::string raw(reinterpret_cast<const char *>(payload.data()), payload.size());
      if (raw.empty()) {
        return std::vector<std::string> {};
      }
      auto j = nlohmann::json::parse(raw, nullptr, false);
      if (j.is_discarded()) {
        return std::nullopt;
      }
      return parse_snapshot_exclude_json_node(j);
    } catch (...) {
      return std::nullopt;
    }
  }

  /**
   * @brief Load snapshot exclusion devices from vibeshine_state.json.
   *
   * This reads the exclusion list that Sunshine persists to the state file,
   * allowing the display helper to know which devices to exclude without
   * depending on IPC from Sunshine.
   *
   * @param path Path to vibeshine_state.json
   * @param ids_out Output vector for device IDs
   * @return true if loaded successfully, false otherwise
   */
  bool load_vibeshine_snapshot_exclusions(const std::filesystem::path &path, std::vector<std::string> &ids_out) {
    ids_out.clear();
    if (path.empty()) {
      return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
      return false;
    }
    try {
      FILE *f = _wfopen(path.wstring().c_str(), L"rb");
      if (!f) {
        return false;
      }
      auto guard = std::unique_ptr<FILE, int (*)(FILE *)>(f, fclose);
      std::string data;
      char buf[4096];
      while (size_t n = fread(buf, 1, sizeof(buf), f)) {
        data.append(buf, n);
      }
      auto j = nlohmann::json::parse(data, nullptr, false);
      if (j.is_discarded()) {
        return false;
      }
      // vibeshine_state.json format: { "root": { "snapshot_exclude_devices": [...] } }
      if (j.is_object() && j.contains("root")) {
        const auto &root = j["root"];
        if (root.is_object() && root.contains("snapshot_exclude_devices")) {
          ids_out = parse_snapshot_exclude_json_node(root["snapshot_exclude_devices"]);
          return !ids_out.empty() || root["snapshot_exclude_devices"].is_array();
        }
      }
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "Failed to parse vibeshine_state.json for snapshot exclusions: " << e.what();
    } catch (...) {
    }
    return false;
  }

  bool handle_apply(ServiceState &state, std::span<const uint8_t> payload, std::string &error_msg) {
    // Cancel any ongoing restore activity since a new APPLY supersedes it
    state.stop_restore_polling();
    state.cancel_delayed_reapply();
    state.cancel_post_apply_tasks();
    state.exit_after_revert.store(false, std::memory_order_release);

    std::string json(reinterpret_cast<const char *>(payload.data()), payload.size());
    bool wa_hdr_toggle = false;
    std::optional<std::string> requested_virtual_layout;
    std::vector<std::pair<std::string, display_device::Point>> monitor_position_overrides;
    std::optional<display_device::ActiveTopology> sunshine_topology;
    std::optional<std::vector<std::string>> snapshot_exclude_devices;
    std::string sanitized_json = json;
    try {
      auto j = nlohmann::json::parse(json);
      if (j.is_object()) {
        if (j.contains("wa_hdr_toggle")) {
          wa_hdr_toggle = j["wa_hdr_toggle"].get<bool>();
          j.erase("wa_hdr_toggle");
        }
        if (j.contains("sunshine_virtual_layout") && j["sunshine_virtual_layout"].is_string()) {
          requested_virtual_layout = j["sunshine_virtual_layout"].get<std::string>();
          j.erase("sunshine_virtual_layout");
        }
        if (j.contains("sunshine_monitor_positions") && j["sunshine_monitor_positions"].is_object()) {
          for (auto it = j["sunshine_monitor_positions"].begin(); it != j["sunshine_monitor_positions"].end(); ++it) {
            const auto &node = it.value();
            if (!node.is_object()) {
              continue;
            }
            auto x_it = node.find("x");
            auto y_it = node.find("y");
            if (x_it == node.end() || y_it == node.end() || !x_it->is_number_integer() || !y_it->is_number_integer()) {
              continue;
            }
            monitor_position_overrides.emplace_back(
              it.key(),
              display_device::Point {x_it->get<int>(), y_it->get<int>()}
            );
          }
          j.erase("sunshine_monitor_positions");
        }
        if (j.contains("sunshine_snapshot_exclude_devices")) {
          snapshot_exclude_devices = parse_snapshot_exclude_json_node(j["sunshine_snapshot_exclude_devices"]);
          j.erase("sunshine_snapshot_exclude_devices");
        }
        if (j.contains("sunshine_topology") && j["sunshine_topology"].is_array()) {
          display_device::ActiveTopology topo;
          for (const auto &grp_node : j["sunshine_topology"]) {
            if (!grp_node.is_array()) {
              continue;
            }
            std::vector<std::string> grp;
            for (const auto &id_node : grp_node) {
              if (!id_node.is_string()) {
                continue;
              }
              grp.push_back(id_node.get<std::string>());
            }
            if (!grp.empty()) {
              topo.push_back(std::move(grp));
            }
          }
          if (!topo.empty()) {
            sunshine_topology = std::move(topo);
          }
          j.erase("sunshine_topology");
        }
        if (j.contains("sunshine_always_restore_from_golden") && j["sunshine_always_restore_from_golden"].is_boolean()) {
          state.always_restore_from_golden.store(j["sunshine_always_restore_from_golden"].get<bool>(), std::memory_order_release);
          j.erase("sunshine_always_restore_from_golden");
        }
        sanitized_json = j.dump();
      }
    } catch (...) {
    }

    if (snapshot_exclude_devices.has_value()) {
      state.controller.set_snapshot_exclusions(*snapshot_exclude_devices);
    }

    display_device::SingleDisplayConfiguration cfg {};
    std::string err;
    if (!display_device::fromJson(sanitized_json, cfg, &err)) {
      BOOST_LOG(error) << "Failed to parse SingleDisplayConfiguration JSON: " << err;
      error_msg = "Invalid display configuration payload";
      return false;
    }
    state.last_apply_ms.store(
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
      )
        .count(),
      std::memory_order_release
    );
    state.last_cfg = cfg;
    // Snapshot is taken earlier via SnapshotCurrent before any display enumeration
    // that might activate external dummy plugs.
    state.retry_revert_on_topology.store(false, std::memory_order_release);
    state.exit_after_revert.store(false, std::memory_order_release);

    if (state.controller.soft_test_display_settings(cfg, sunshine_topology)) {
      BOOST_LOG(info) << "Display configuration validated, creating scheduled task before applying settings";
      const bool task_created = create_restore_scheduled_task();
      BOOST_LOG(info) << "Scheduled task creation result: " << (task_created ? "SUCCESS" : "FAILED");

      if (!state.controller.apply(cfg, sunshine_topology)) {
        error_msg = "Helper failed to apply requested display configuration";
        return false;
      }

      constexpr int kMaxSyncVerifyAttempts = 2;
      bool verified_sync = false;
      std::vector<std::chrono::milliseconds> reapply_delays {750ms};
      if (cfg.m_hdr_state) {
        // HDR state can be (re)applied asynchronously by Windows shortly after topology/mode changes,
        // especially for virtual displays. Schedule a few extra best-effort re-apply attempts to
        // enforce the requested HDR state.
        reapply_delays = {750ms, 2500ms, 5500ms};
      }

      for (int attempt = 1; attempt <= kMaxSyncVerifyAttempts; ++attempt) {
        if (state.verify_last_configuration_sticky(ServiceState::kVerificationSettleDelay)) {
          verified_sync = true;
          if (attempt > 1) {
            BOOST_LOG(info) << "Display helper: verification succeeded on attempt #" << attempt << " after re-apply.";
          }
          break;
        }
        BOOST_LOG(warning) << "Display helper: verification attempt #" << attempt
                           << " did not stick; "
                           << (attempt < kMaxSyncVerifyAttempts ? "retrying synchronously." : "deferring to async retry.");
        state.best_effort_apply_last_cfg();
      }
      if (verified_sync) {
        BOOST_LOG(debug) << "Display helper: synchronous verification succeeded; scheduling follow-up check.";
      } else {
        BOOST_LOG(warning) << "Display helper: synchronous verification failed; scheduling async fallback.";
      }

      state.retry_apply_on_topology.store(false, std::memory_order_release);
      state.schedule_post_apply_tasks(
        false,
        std::nullopt,
        wa_hdr_toggle,
        requested_virtual_layout,
        std::move(monitor_position_overrides),
        std::move(reapply_delays)
      );
    } else {
      BOOST_LOG(error) << "Display helper: configuration failed SDC_VALIDATE soft-test; not applying.";
      error_msg = "Display configuration failed validation";
      return false;
    }
    error_msg.clear();
    return true;
  }

  void handle_revert(ServiceState &state, std::atomic<bool> &running) {
    BOOST_LOG(info) << "REVERT command received - initiating display settings restoration";
    state.retry_apply_on_topology.store(false, std::memory_order_release);
    state.direct_revert_bypass_grace.store(true, std::memory_order_release);
    state.exit_after_revert.store(true, std::memory_order_release);
    state.restore_requested.store(true, std::memory_order_release);
    state.restore_origin_epoch.store(state.current_connection_epoch(), std::memory_order_release);

    // Give Sunshine a short window to immediately start a new session and DISARM,
    // avoiding costly restore/apply thrash during fast client switching.
    state.arm_restore_grace(5000ms, "revert");
    state.ensure_restore_polling(ServiceState::RestoreWindow::Primary);
  }

  void handle_misc(ServiceState &state, platf::dxgi::AsyncNamedPipe &async_pipe, MsgType type, std::span<const uint8_t> payload) {
    if (auto exclusions = parse_snapshot_exclude_payload(payload)) {
      state.controller.set_snapshot_exclusions(*exclusions);
    }
    if (type == MsgType::ExportGolden) {
      const bool saved = state.controller.save_display_settings_snapshot_to_file(state.golden_path);
      BOOST_LOG(info) << "Export golden restore snapshot result=" << (saved ? "true" : "false");
    } else if (type == MsgType::Reset) {
      (void) state.controller.reset_persistence();
      state.retry_apply_on_topology.store(false, std::memory_order_release);
      state.retry_revert_on_topology.store(false, std::memory_order_release);
    } else if (type == MsgType::Disarm) {
      state.disarm_restore_requests("DISARM command received");
    } else if (type == MsgType::SnapshotCurrent) {
      (void) state.promote_current_snapshot_to_previous("snapshot-only");
      (void) state.capture_current_snapshot("snapshot-only");
    } else if (type == MsgType::Ping) {
      state.record_heartbeat_ping();
      send_framed_content(async_pipe, MsgType::Ping);
    } else {
      BOOST_LOG(warning) << "Unknown message type: " << static_cast<int>(type);
    }
  }

  void handle_frame(ServiceState &state, platf::dxgi::AsyncNamedPipe &async_pipe, MsgType type, std::span<const uint8_t> payload, std::atomic<bool> &running) {
    if (type == MsgType::Apply) {
      std::string error_msg;
      bool success = handle_apply(state, payload, error_msg);
      std::vector<uint8_t> result_payload;
      result_payload.push_back(success ? 1u : 0u);
      if (!error_msg.empty()) {
        const auto *begin = reinterpret_cast<const uint8_t *>(error_msg.data());
        result_payload.insert(result_payload.end(), begin, begin + error_msg.size());
      }
      send_framed_content(async_pipe, MsgType::ApplyResult, result_payload);
    } else if (type == MsgType::Revert) {
      handle_revert(state, running);
    } else if (type == MsgType::Stop) {
      running.store(false, std::memory_order_release);
    } else {
      handle_misc(state, async_pipe, type, payload);
    }
  }

  void attempt_revert_after_disconnect(ServiceState &state, std::atomic<bool> &running, uint64_t connection_epoch) {
    if (!state.is_connection_epoch_current(connection_epoch)) {
      BOOST_LOG(info) << "Ignoring disconnect event from stale connection (epoch=" << connection_epoch
                      << ", current=" << state.current_connection_epoch() << ")";
      return;
    }
    auto still_current = [&]() {
      return state.is_connection_epoch_current(connection_epoch);
    };
    // Pipe broken -> Sunshine might have crashed. Begin autonomous restore.
    state.retry_apply_on_topology.store(false, std::memory_order_release);
    state.cancel_delayed_reapply();
    const bool potentially_modified = state.last_cfg.has_value() ||
                                      state.exit_after_revert.load(std::memory_order_acquire);
    if (!potentially_modified) {
      state.restore_requested.store(false, std::memory_order_release);
      running.store(false, std::memory_order_release);
      return;
    }

    if (!state.direct_revert_bypass_grace.load(std::memory_order_acquire)) {
      const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()
      )
                            .count();
      const auto last_apply = state.last_apply_ms.load(std::memory_order_acquire);
      if (last_apply > 0 && now_ms >= last_apply) {
        const auto delta_ms = now_ms - last_apply;
        if (delta_ms <= kApplyDisconnectGrace.count()) {
          BOOST_LOG(info)
            << "Client disconnected " << delta_ms
            << "ms after APPLY; deferring restore to avoid thrash.";
          state.schedule_delayed_reapply();
          state.restore_requested.store(false, std::memory_order_release);
          return;
        }
      }
    }

    if (!still_current()) {
      BOOST_LOG(info) << "Skipping restore after disconnect because a newer connection is active (epoch="
                      << connection_epoch << ", current=" << state.current_connection_epoch() << ")";
      return;
    }

    BOOST_LOG(info) << "Client disconnected; entering restore polling loop (3s interval) until successful.";
    state.exit_after_revert.store(true, std::memory_order_release);
    state.restore_requested.store(true, std::memory_order_release);
    state.restore_origin_epoch.store(connection_epoch, std::memory_order_release);
    state.arm_restore_grace(5000ms, "disconnect");
    state.ensure_restore_polling(ServiceState::RestoreWindow::Primary);
  }

  void process_incoming_frame(ServiceState &state, platf::dxgi::AsyncNamedPipe &async_pipe, std::span<const uint8_t> frame, std::atomic<bool> &running) {
    if (frame.empty()) {
      return;
    }
    MsgType type {};
    std::span<const uint8_t> payload;
    if (frame.size() >= 5) {
      uint32_t len = 0;
      std::memcpy(&len, frame.data(), 4);
      if (len > 0 && frame.size() >= 4u + len) {
        type = static_cast<MsgType>(frame[4]);
        if (len > 1) {
          payload = std::span<const uint8_t>(frame.data() + 5, len - 1);
        } else {
          payload = {};
        }
      } else {
        type = static_cast<MsgType>(frame[0]);
        payload = frame.subspan(1);
      }
    } else {
      type = static_cast<MsgType>(frame[0]);
      payload = frame.subspan(1);
    }
    handle_frame(state, async_pipe, type, payload, running);
  }
}  // namespace

  int main(int argc, char *argv[]) {
  bool restore_mode = false;
  if (argc > 1) {
    for (int i = 1; i < argc; ++i) {
      if (std::strcmp(argv[i], "--restore") == 0) {
        restore_mode = true;
      } else if (std::strcmp(argv[i], "--no-startup-restore") == 0) {
        BOOST_LOG(info) << "--no-startup-restore is deprecated and ignored.";
      }
    }
  }

  if (restore_mode) {
    FreeConsole();
    hide_console_window();
  }

  HANDLE singleton = nullptr;
  if (!ensure_single_instance(singleton)) {
    return 3;
  }

  const auto logdir = compute_log_dir();
  const auto logfile = (logdir / L"sunshine_display_helper.log");
  const auto goldenfile = (logdir / L"display_golden_restore.json");
  const auto sessionfile = (logdir / L"display_session_restore.json");
  const auto session_current = (logdir / L"display_session_current.json");
  const auto session_previous = (logdir / L"display_session_previous.json");
  const auto vibeshine_state_file = (logdir / L"vibeshine_state.json");
  auto _log_guard = logging::init(2 /*info*/, logfile);

  if (restore_mode) {
    BOOST_LOG(info) << "Display helper started in restore mode (--restore flag)";
    dd_log_bridge().install();
    ServiceState state;
    state.golden_path = goldenfile;
    state.session_path = sessionfile;
    state.session_current_path = session_current;
    state.session_previous_path = session_previous;
    {
      // Load snapshot exclusions from vibeshine_state.json (source of truth from Sunshine)
      std::vector<std::string> persisted;
      if (load_vibeshine_snapshot_exclusions(vibeshine_state_file, persisted)) {
        BOOST_LOG(info) << "Loaded snapshot exclusions from vibeshine_state.json (" << persisted.size() << ")";
        state.controller.set_snapshot_exclusions(persisted);
      }
    }

    {
      std::error_code ec_cur, ec_legacy;
      const bool cur_exists = std::filesystem::exists(state.session_current_path, ec_cur);
      const bool legacy_exists = std::filesystem::exists(state.session_path, ec_legacy);
      if (cur_exists && !ec_cur) {
        if (validate_session_snapshot(state, state.session_current_path)) {
          state.session_saved.store(true, std::memory_order_release);
          BOOST_LOG(info) << "Existing current session snapshot detected; will preserve until confirmed restore: "
                          << state.session_current_path.string();
        }
      } else if (legacy_exists && !ec_legacy) {
        std::error_code ec_copy;
        std::filesystem::create_directories(state.session_current_path.parent_path(), ec_copy);
        (void) ec_copy;
        std::filesystem::copy_file(state.session_path, state.session_current_path, std::filesystem::copy_options::overwrite_existing, ec_copy);
        if (!ec_copy) {
          if (validate_session_snapshot(state, state.session_current_path)) {
            state.session_saved.store(true, std::memory_order_release);
            BOOST_LOG(info) << "Migrated legacy session snapshot to current: " << state.session_current_path.string();
          }
          std::error_code ec_rm;
          (void) std::filesystem::remove(state.session_path, ec_rm);
        }
      }
    }
    {
      std::error_code ec_prev_check;
      if (std::filesystem::exists(state.session_previous_path, ec_prev_check) && !ec_prev_check) {
        (void) validate_session_snapshot(state, state.session_previous_path);
      }
    }

    std::atomic<bool> running {true};
    state.running_flag = &running;
    state.exit_after_revert.store(true, std::memory_order_release);
    state.restore_requested.store(true, std::memory_order_release);
    state.restore_origin_epoch.store(0, std::memory_order_release);

    state.ensure_restore_polling(ServiceState::RestoreWindow::Primary);

    while (running.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(500ms);
    }

    BOOST_LOG(info) << "Display helper restore mode completed; shutting down";
    logging::log_flush();
    return 0;
  }

  platf::dxgi::FramedPipeFactory pipe_factory(std::make_unique<platf::dxgi::AnonymousPipeFactory>());
  dd_log_bridge().install();
  ServiceState state;
  // Suppression of startup restore is deprecated; REVERTs are always allowed.
  state.golden_path = goldenfile;
  state.session_path = sessionfile;  // legacy
  state.session_current_path = session_current;
  state.session_previous_path = session_previous;
  {
    // Load snapshot exclusions from vibeshine_state.json (source of truth from Sunshine)
    std::vector<std::string> persisted;
    if (load_vibeshine_snapshot_exclusions(vibeshine_state_file, persisted)) {
      BOOST_LOG(info) << "Loaded snapshot exclusions from vibeshine_state.json (" << persisted.size() << ")";
      state.controller.set_snapshot_exclusions(persisted);
    }
  }
  {
    std::error_code ec_cur, ec_legacy;
    const bool cur_exists = std::filesystem::exists(state.session_current_path, ec_cur);
    const bool legacy_exists = std::filesystem::exists(state.session_path, ec_legacy);
    if (cur_exists && !ec_cur) {
      if (validate_session_snapshot(state, state.session_current_path)) {
        state.session_saved.store(true, std::memory_order_release);
        BOOST_LOG(info) << "Existing current session snapshot detected; will preserve until confirmed restore: "
                        << state.session_current_path.string();
      }
    } else if (legacy_exists && !ec_legacy) {
      std::error_code ec_copy;
      std::filesystem::create_directories(state.session_current_path.parent_path(), ec_copy);
      (void) ec_copy;
      std::filesystem::copy_file(state.session_path, state.session_current_path, std::filesystem::copy_options::overwrite_existing, ec_copy);
      if (!ec_copy) {
        if (validate_session_snapshot(state, state.session_current_path)) {
          state.session_saved.store(true, std::memory_order_release);
          BOOST_LOG(info) << "Migrated legacy session snapshot to current: " << state.session_current_path.string();
        }
        std::error_code ec_rm;
        (void) std::filesystem::remove(state.session_path, ec_rm);
      }
    }
  }
  {
    std::error_code ec_prev_check;
    if (std::filesystem::exists(state.session_previous_path, ec_prev_check) && !ec_prev_check) {
      (void) validate_session_snapshot(state, state.session_previous_path);
    }
  }
  // Topology-based retries disabled; no watcher needed anymore.

  std::atomic<bool> running {true};
  state.running_flag = &running;
  auto last_connect_wait_log = std::chrono::steady_clock::time_point::min();
  constexpr auto kReconnectLogInterval = std::chrono::hours(1);

  // Outer service loop: keep accepting new client sessions while running
  while (running.load(std::memory_order_acquire)) {
    auto ctrl_pipe = pipe_factory.create_server("sunshine_display_helper");
    if (!ctrl_pipe) {
      platf::dxgi::FramedPipeFactory fallback_factory(std::make_unique<platf::dxgi::NamedPipeFactory>());
      ctrl_pipe = fallback_factory.create_server("sunshine_display_helper");
      if (!ctrl_pipe) {
        BOOST_LOG(error) << "Failed to create control pipe; retrying in 500ms";
        std::this_thread::sleep_for(500ms);
        continue;
      }
    }

    platf::dxgi::AsyncNamedPipe async_pipe(std::move(ctrl_pipe));

    const auto connection_epoch = state.begin_connection_epoch();
    state.stop_restore_polling();
    state.begin_heartbeat_monitoring();

    // Reset and start per-connection command worker so IPC stays responsive even during heavy display work.
    state.command_worker_stop.store(true, std::memory_order_release);
    state.command_queue_cv.notify_all();
    if (state.command_worker.joinable()) {
      state.command_worker.join();
    }
    {
      std::lock_guard<std::mutex> lg(state.command_queue_mutex);
      state.command_queue.clear();
    }
    state.command_worker_stop.store(false, std::memory_order_release);
    state.command_worker_epoch.store(connection_epoch, std::memory_order_release);

    auto start_command_worker = [&](platf::dxgi::AsyncNamedPipe &pipe) {
      state.command_worker = std::jthread([&, connection_epoch](std::stop_token) {
        while (!state.command_worker_stop.load(std::memory_order_acquire) &&
               running.load(std::memory_order_acquire) &&
               state.is_connection_epoch_current(connection_epoch)) {
          std::vector<uint8_t> next;
          {
            std::unique_lock<std::mutex> lk(state.command_queue_mutex);
            state.command_queue_cv.wait(lk, [&]() {
              return state.command_worker_stop.load(std::memory_order_acquire) ||
                     !running.load(std::memory_order_acquire) ||
                     !state.command_queue.empty() ||
                     !state.is_connection_epoch_current(connection_epoch);
            });
            if (state.command_worker_stop.load(std::memory_order_acquire) ||
                !state.is_connection_epoch_current(connection_epoch) ||
                !running.load(std::memory_order_acquire)) {
              break;
            }
            if (state.command_queue.empty()) {
              continue;
            }
            next = std::move(state.command_queue.front());
            state.command_queue.pop_front();
          }
          if (!next.empty()) {
            try {
              process_incoming_frame(state, pipe, next, running);
            } catch (const std::exception &ex) {
              BOOST_LOG(error) << "IPC framing error in command worker: " << ex.what();
            }
          }
        }
      });
    };

    auto on_message = [&, connection_epoch](std::span<const uint8_t> bytes) {
      if (!state.is_connection_epoch_current(connection_epoch)) {
        return;
      }
      {
        std::lock_guard<std::mutex> lg(state.command_queue_mutex);
        if (state.command_worker_epoch.load(std::memory_order_acquire) == connection_epoch) {
          state.command_queue.emplace_back(bytes.begin(), bytes.end());
        }
      }
      state.command_queue_cv.notify_one();
    };

    // Track broken/disconnect events from the async worker thread without
    // attempting to stop/join from within the callback (which would deadlock).
    std::atomic<bool> broken {false};

    auto on_error = [&, connection_epoch](const std::string &err) {
      if (!state.is_connection_epoch_current(connection_epoch)) {
        BOOST_LOG(info) << "Ignoring async pipe error from stale connection (epoch=" << connection_epoch
                        << ", current=" << state.current_connection_epoch() << ")";
        return;
      }
      BOOST_LOG(error) << "Async pipe error: " << err << "; handling disconnect and revert policy.";
      broken.store(true, std::memory_order_release);
      state.command_worker_stop.store(true, std::memory_order_release);
      state.command_queue_cv.notify_all();
      attempt_revert_after_disconnect(state, running, connection_epoch);
    };

    auto on_broken = [&, connection_epoch]() {
      if (!state.is_connection_epoch_current(connection_epoch)) {
        BOOST_LOG(info) << "Ignoring disconnect notification from stale connection (epoch=" << connection_epoch
                        << ", current=" << state.current_connection_epoch() << ")";
        return;
      }
      BOOST_LOG(warning) << "Client disconnected; applying revert policy and staying alive until successful.";
      broken.store(true, std::memory_order_release);
      state.command_worker_stop.store(true, std::memory_order_release);
      state.command_queue_cv.notify_all();
      attempt_revert_after_disconnect(state, running, connection_epoch);
    };

    // Start async message loop (establish_connection is a no-op if already connected)
    async_pipe.start(on_message, on_error, on_broken);
    start_command_worker(async_pipe);

    // Stay in this inner loop until the client disconnects or service told to exit
    while (running.load(std::memory_order_acquire) && async_pipe.is_connected() && !broken.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(200ms);
      if (state.check_heartbeat_timeout() && state.is_connection_epoch_current(connection_epoch)) {
        BOOST_LOG(warning) << "Heartbeat timeout exceeded; applying revert policy.";
        broken.store(true, std::memory_order_release);
        attempt_revert_after_disconnect(state, running, connection_epoch);
        break;
      }
    }

    // Ensure the worker thread is stopped and the server handle is
    // disconnected before looping to accept a new session.
    state.end_heartbeat_monitoring();
    state.command_worker_stop.store(true, std::memory_order_release);
    state.command_queue_cv.notify_all();
    if (state.command_worker.joinable()) {
      state.command_worker.join();
    }
    {
      std::lock_guard<std::mutex> lg(state.command_queue_mutex);
      state.command_queue.clear();
    }
    async_pipe.stop();

    // If a successful restore requested exit, break outer loop
    if (!running.load(std::memory_order_acquire)) {
      break;
    }

    // Otherwise, loop around to create a fresh pipe for the next session
  }

  BOOST_LOG(info) << "Display settings helper shutting down";
  logging::log_flush();
  return 0;
}

#else
int main() {
  return 0;
}
#endif
