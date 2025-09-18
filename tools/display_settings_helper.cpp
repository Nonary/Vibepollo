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
  #include <cstdio>
  #include <cstdlib>
  #include <cstring>
  #include <cwchar>
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
  #include <dbt.h>
  #include <devguid.h>
  #include <powrprof.h>
  #include <shlobj.h>
  #include <windows.h>
  #include <winerror.h>

namespace {
  static const GUID kMonitorInterfaceGuid = {0xe6f07b5f, 0xee97, 0x4a90, {0xb0, 0x76, 0x33, 0xf5, 0x7b, 0xf4, 0xea, 0xa7}};
}

using namespace std::chrono_literals;
namespace bl = boost::log;

namespace {

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
    DisplayController() {
      // Build the Windows display device API and keep a direct WinApiLayer handle as well.
      m_wapi = std::make_shared<display_device::WinApiLayer>();
      m_dd = std::make_shared<display_device::WinDisplayDevice>(m_wapi);

      // Use noop persistence and audio context here; Sunshine owns lifecycle across streams.
      m_sm = std::make_unique<display_device::SettingsManager>(
        m_dd,
        std::make_shared<display_device::NoopAudioContext>(),
        std::make_unique<display_device::PersistentState>(std::make_shared<display_device::NoopSettingsPersistence>()),
        display_device::WinWorkarounds {}
      );
    }

    // Enumerate all currently available display device IDs (active or inactive).
    std::set<std::string> enum_all_device_ids() const {
      std::set<std::string> ids;
      try {
        for (const auto &d : m_sm->enumAvailableDevices()) {
          ids.insert(d.m_device_id);
        }
      } catch (...) {
        // best-effort; return what we have
      }
      return ids;
    }

    // Validate whether a snapshot's topology is currently applicable.
    bool is_topology_valid(const display_device::DisplaySettingsSnapshot &snap) const {
      try {
        return m_dd->isTopologyValid(snap.m_topology);
      } catch (...) {
        return false;
      }
    }

    // Apply display configuration; returns whether applied OK.
    bool apply(const display_device::SingleDisplayConfiguration &cfg) {
      using enum display_device::SettingsManagerInterface::ApplyResult;
      const auto res = m_sm->applySettings(cfg);
      BOOST_LOG(info) << "ApplySettings result: " << static_cast<int>(res);
      return res == Ok;
    }

    // Revert display configuration; returns whether reverted OK.
    bool revert() {
      using enum display_device::SettingsManagerInterface::RevertResult;
      const auto res = m_sm->revertSettings();
      BOOST_LOG(info) << "RevertSettings result: " << static_cast<int>(res);
      return res == Ok;
    }

    // Reset persistence file; best-effort noop persistence returns true.
    bool reset_persistence() {
      return m_sm->resetPersistence();
    }

    // Capture a full snapshot of current settings.
    display_device::DisplaySettingsSnapshot snapshot() const {
      display_device::DisplaySettingsSnapshot snap;
      try {
        // Topology
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

    // Build and validate the topology implied by a SingleDisplayConfiguration without applying it.
    bool soft_test_display_settings(const display_device::SingleDisplayConfiguration &cfg) const {
      try {
        const auto topo_before = m_dd->getCurrentTopology();
        if (!m_dd->isTopologyValid(topo_before)) {
          return false;
        }
        const auto devices = m_sm->enumAvailableDevices();
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

    // Compute the topology that would be requested by cfg based on the current initial state.
    std::optional<display_device::ActiveTopology> compute_expected_topology(const display_device::SingleDisplayConfiguration &cfg) const {
      try {
        const auto topo_before = m_dd->getCurrentTopology();
        if (!m_dd->isTopologyValid(topo_before)) {
          return std::nullopt;
        }
        const auto devices = m_sm->enumAvailableDevices();
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

    bool is_topology_the_same(const display_device::ActiveTopology &a, const display_device::ActiveTopology &b) const {
      try {
        return m_dd->isTopologyTheSame(a, b);
      } catch (...) {
        return false;
      }
    }

    // Apply the HDR blank workaround synchronously (call from a background thread)
    void blank_hdr_states(std::chrono::milliseconds delay) {
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
      return snap;
    }

    // Apply snapshot best-effort.
    bool apply_snapshot(const display_device::DisplaySettingsSnapshot &snap) {
      try {
        // Set topology first
        (void) m_dd->setTopology(snap.m_topology);
        // Then set modes temporarily (no DB persistence) with internal relaxed/strict strategy
        (void) m_dd->setDisplayModesTemporary(snap.m_modes);
        // Then HDR
        (void) m_dd->setHdrStates(snap.m_hdr_states);
        // Then primary
        if (!snap.m_primary_device.empty()) {
          (void) m_dd->setAsPrimary(snap.m_primary_device);
        }
        return true;
      } catch (...) {
        return false;
      }
    }

  private:
    std::shared_ptr<display_device::WinApiLayer> m_wapi;

    // Collect devices from full enumeration
    void collect_all_device_ids(std::set<std::string> &out) const {
      for (const auto &d : m_sm->enumAvailableDevices()) {
        out.insert(d.m_device_id);
      }
    }

    // Find primary device (if any)
    std::optional<std::string> find_primary_in_set(const std::set<std::string> &ids) const {
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

    std::unique_ptr<display_device::SettingsManagerInterface> m_sm;
    std::shared_ptr<display_device::WinDisplayDeviceInterface> m_dd;
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

  struct ServiceState {
    enum class RestoreWindow {
      Primary,
      Event
    };

    DisplayController controller;
    DisplayEventPump event_pump;
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
    // Guard: if a session restore succeeded recently, suppress Golden for a cooldown
    std::atomic<long long> last_session_restore_success_ms {0};

    // Polling-based restore loop state (replaces topology-change-triggered retries)
    std::jthread restore_poll_thread;
    std::atomic<bool> restore_poll_active {false};
    std::atomic<uint64_t> next_connection_epoch {1};
    std::atomic<uint64_t> active_connection_epoch {0};
    std::atomic<uint64_t> restore_origin_epoch {0};

    static constexpr auto kRestoreWindowPrimary = std::chrono::minutes(2);
    static constexpr auto kRestoreWindowEvent = std::chrono::seconds(30);
    static constexpr auto kRestoreEventDebounce = std::chrono::milliseconds(500);
    static constexpr int kMaxRestoreStages = 3;

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
          BOOST_LOG(warning) << "Failed to promote previous → current (copy error); will snapshot current instead.";
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

    // Apply the golden snapshot (if available) and verify the system now matches it.
    // Performs up to two attempts (initial + one retry) with short pauses to allow
    // Windows to settle. Returns true only if the post-apply signature exactly
    // matches the golden snapshot signature.
    bool apply_golden_and_confirm(std::stop_token st = {}) {
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
        // Golden won: copy golden snapshot to previous
        std::error_code ec_prev_rm;
        (void) std::filesystem::remove(session_previous_path, ec_prev_rm);
        std::error_code ec_copy;
        std::filesystem::copy_file(golden_path, session_previous_path, std::filesystem::copy_options::overwrite_existing, ec_copy);
        BOOST_LOG(info) << "Golden restore confirmed; copied golden snapshot to previous. copy_ok=" << (!ec_copy ? "true" : "false");
        return true;
      }

      // Attempt 2 (double-check) after a short delay
      if (should_cancel()) {
        return false;
      }
      std::this_thread::sleep_for(700ms);
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
        std::error_code ec_prev_rm;
        (void) std::filesystem::remove(session_previous_path, ec_prev_rm);
        std::error_code ec_copy2;
        std::filesystem::copy_file(golden_path, session_previous_path, std::filesystem::copy_options::overwrite_existing, ec_copy2);
        BOOST_LOG(info) << "Golden restore confirmed (retry); copied golden snapshot to previous. copy_ok=" << (!ec_copy2 ? "true" : "false");
      }
      return ok;
    }

    // Apply the session baseline snapshot (if available) and verify the system now matches it.
    bool apply_session_and_confirm(std::stop_token st = {}) {
      auto base = controller.load_display_settings_snapshot(session_current_path);
      if (!base) {
        BOOST_LOG(info) << "Session baseline snapshot not available.";
        return false;
      }
      const auto before_sig = controller.signature(controller.snapshot());

      const auto should_cancel = [&]() {
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
      BOOST_LOG(info) << "Session restore attempt #1: before_sig=" << before_sig
                      << ", current_sig=" << controller.signature(cur)
                      << ", baseline_sig=" << controller.signature(*base)
                      << ", match=" << (ok ? "true" : "false");
      if (!ok) {
        if (should_cancel()) {
          return false;
        }
        std::this_thread::sleep_for(700ms);
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
        BOOST_LOG(info) << "Session restore attempt #2: current_sig=" << controller.signature(cur2)
                        << ", baseline_sig=" << controller.signature(*base) << ", match=" << (ok ? "true" : "false");
      }

      if (ok) {
        // Successful restore: delete current session snapshot and set guard timestamp
        std::error_code ec_rm;
        bool removed = std::filesystem::remove(session_current_path, ec_rm);
        BOOST_LOG(info) << "Session restore confirmed; delete current session snapshot path='"
                        << session_current_path.string() << "' result=" << (removed && !ec_rm ? "true" : "false");
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch()
        )
                              .count();
        last_session_restore_success_ms.store(now_ms, std::memory_order_release);
      }
      return ok;
    }

    // Attempt a restore once if a valid topology is present. Returns true on
    // confirmed success, false otherwise. Prefers session baseline, then golden.
    bool try_restore_once_if_valid(std::stop_token st) {
      if (st.stop_possible() && st.stop_requested()) {
        return false;
      }
      // Session snapshot first
      if (auto base = controller.load_display_settings_snapshot(session_current_path)) {
        if (st.stop_possible() && st.stop_requested()) {
          return false;
        }
        if (controller.is_topology_valid(*base)) {
          if (apply_session_and_confirm(st)) {
            return true;
          }
        }
      }
      if (auto golden = controller.load_display_settings_snapshot(golden_path)) {
        if (st.stop_possible() && st.stop_requested()) {
          return false;
        }
        if (controller.validate_topology_with_os(golden->m_topology)) {
          if (apply_golden_and_confirm(st)) {
            return true;
          }
        }
      }
      return false;
    }

    // Start a background polling loop that checks every ~3s whether the
    // requested restore topology is valid; if so, perform the restore and
    // confirm success. Logging is throttled (~15 minutes) to avoid noise.
    void ensure_restore_polling(RestoreWindow window = RestoreWindow::Primary) {
      if (!restore_requested.load(std::memory_order_acquire)) {
        return;
      }
      bool expected = false;
      if (!restore_poll_active.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        signal_restore_event("initial", window, true);
        BOOST_LOG(debug) << "Restore loop already active; ignoring additional trigger.";
        return;
      }
      event_pump.start([this](const char *reason) {
        signal_restore_event(reason, RestoreWindow::Event);
      });
      signal_restore_event("initial", window, true);
      restore_poll_thread = std::jthread(&ServiceState::restore_poll_proc, this);
    }

    void stop_restore_polling() {
      restore_poll_active.store(false, std::memory_order_release);
      event_pump.stop();
      signal_restore_event(nullptr);
      restore_active_until_ms.store(0, std::memory_order_release);
      last_restore_event_ms.store(0, std::memory_order_release);
      restore_active_window.store(RestoreWindow::Event, std::memory_order_release);
      restore_stage_running.store(false, std::memory_order_release);
      if (restore_poll_thread.joinable()) {
        restore_poll_thread.request_stop();
        restore_poll_thread.join();
      }
      restore_requested.store(false, std::memory_order_release);
      restore_origin_epoch.store(0, std::memory_order_release);
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

      // If there is no session or golden snapshot, there is nothing to restore.
      try {
        std::error_code ec1, ec2;
        const bool has_session = std::filesystem::exists(self->session_current_path, ec1);
        const bool has_golden = std::filesystem::exists(self->golden_path, ec2);
        if (!has_session && !has_golden) {
          BOOST_LOG(info) << "Restore polling: no session or golden snapshot present; exiting helper.";
          if (self->running_flag) {
            self->running_flag->store(false, std::memory_order_release);
          }
          self->event_pump.stop();
          self->restore_poll_active.store(false, std::memory_order_release);
          self->restore_requested.store(false, std::memory_order_release);
          self->clear_restore_origin();
          return;
        }
      } catch (...) {
        // fall through
      }

      // Initial one-shot attempt before entering the loop
      try {
        if (!st.stop_requested() && self->try_restore_once_if_valid(st)) {
          self->retry_revert_on_topology.store(false, std::memory_order_release);
          refresh_shell_after_display_change();
          const bool exit_helper = self->should_exit_after_restore();
          if (exit_helper && self->running_flag) {
            BOOST_LOG(info) << "Restore confirmed (initial attempt); exiting helper.";
            self->running_flag->store(false, std::memory_order_release);
          } else if (!exit_helper) {
            BOOST_LOG(info) << "Restore confirmed (initial attempt); keeping helper alive for newer connection.";
          }
          self->event_pump.stop();
          self->restore_poll_active.store(false, std::memory_order_release);
          self->restore_active_until_ms.store(0, std::memory_order_release);
          self->restore_active_window.store(RestoreWindow::Event, std::memory_order_release);
          self->last_restore_event_ms.store(0, std::memory_order_release);
          self->restore_requested.store(false, std::memory_order_release);
          self->clear_restore_origin();
          return;
        }
      } catch (...) {}

      while (!st.stop_requested()) {
        const auto now = std::chrono::steady_clock::now();
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        auto active_until_ms = self->restore_active_until_ms.load(std::memory_order_acquire);
        const auto active_window_kind = self->restore_active_window.load(std::memory_order_acquire);
        bool active_window = (active_until_ms != 0 && now_ms <= active_until_ms);
        if (!active_window && active_until_ms != 0 && now_ms > active_until_ms) {
          self->restore_active_until_ms.store(0, std::memory_order_release);
          self->restore_active_window.store(RestoreWindow::Event, std::memory_order_release);
          active_until_ms = 0;
        }

        const auto wait_timeout = active_window ? 500ms : kPoll;

        bool triggered = false;
        try {
          triggered = self->wait_for_restore_event(st, wait_timeout);
        } catch (...) {}
        if (!triggered && active_window && active_window_kind == RestoreWindow::Primary) {
          triggered = true;
        }
        if (st.stop_requested()) {
          break;
        }
        if (!triggered) {
          const auto now2 = std::chrono::steady_clock::now();
          if (now2 - last_log >= kLogThrottle) {
            last_log = now2;
            BOOST_LOG(info) << "Restore polling: waiting for event-driven topology changes.";
          }
          continue;
        }

        self->restore_stage_running.store(true, std::memory_order_release);
        bool success = false;
        int stages_attempted = 0;
        const auto window_deadline_ms = self->restore_active_until_ms.load(std::memory_order_acquire);

        for (; stages_attempted < kMaxRestoreStages && !st.stop_requested(); ++stages_attempted) {
          if (self->try_restore_once_if_valid(st)) {
            success = true;
            break;
          }
          const auto stage_now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch()
          )
                                      .count();
          if (window_deadline_ms != 0 && stage_now_ms > window_deadline_ms) {
            break;
          }
        }

        self->restore_stage_running.store(false, std::memory_order_release);

        if (success) {
          self->retry_revert_on_topology.store(false, std::memory_order_release);
          refresh_shell_after_display_change();
          const bool exit_helper = self->should_exit_after_restore();
          if (exit_helper && self->running_flag) {
            BOOST_LOG(info) << "Restore confirmed; exiting helper.";
            self->running_flag->store(false, std::memory_order_release);
          } else if (!exit_helper) {
            BOOST_LOG(info) << "Restore confirmed while newer connection active; helper remains running.";
          }
          self->restore_poll_active.store(false, std::memory_order_release);
          self->event_pump.stop();
          self->restore_active_until_ms.store(0, std::memory_order_release);
          self->restore_active_window.store(RestoreWindow::Event, std::memory_order_release);
          self->last_restore_event_ms.store(0, std::memory_order_release);
          self->restore_requested.store(false, std::memory_order_release);
          self->clear_restore_origin();
          return;
        }

        const auto post_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch()
        )
                               .count();
        if (window_deadline_ms != 0 && post_ms > window_deadline_ms) {
          self->restore_active_until_ms.store(0, std::memory_order_release);
          self->restore_active_window.store(RestoreWindow::Event, std::memory_order_release);
        }

        if (stages_attempted >= kMaxRestoreStages && active_window_kind == RestoreWindow::Event) {
          BOOST_LOG(info) << "Restore polling: staged event attempts exhausted (" << kMaxRestoreStages
                          << "); awaiting next trigger.";
        }
      }
      self->restore_stage_running.store(false, std::memory_order_release);
      self->event_pump.stop();
      self->restore_poll_active.store(false, std::memory_order_release);
      self->restore_active_until_ms.store(0, std::memory_order_release);
      self->restore_active_window.store(RestoreWindow::Event, std::memory_order_release);
      self->last_restore_event_ms.store(0, std::memory_order_release);
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

    // Schedule a couple of delayed re-apply attempts to work around Windows
    // sometimes forcing native resolution immediately after activating a display.
    void schedule_delayed_reapply() {
      if (delayed_reapply_thread.joinable()) {
        delayed_reapply_thread.request_stop();
        delayed_reapply_thread.join();
      }
      if (!last_cfg) {
        return;
      }
      delayed_reapply_thread = std::jthread(&ServiceState::delayed_reapply_proc, this);
    }

    void cancel_delayed_reapply() {
      if (delayed_reapply_thread.joinable()) {
        delayed_reapply_thread.request_stop();
        delayed_reapply_thread.join();
      }
    }

    static void delayed_reapply_proc(std::stop_token st, ServiceState *self) {
      using namespace std::chrono_literals;
      const auto delays = {1s, 3s};
      for (auto d : delays) {
        if (st.stop_requested()) {
          return;
        }
        std::this_thread::sleep_for(d);
        if (st.stop_requested()) {
          return;
        }
        BOOST_LOG(info) << "Delayed re-apply attempt after activation";
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

  void handle_apply(ServiceState &state, std::span<const uint8_t> payload) {
    // Cancel any ongoing restore activity since a new APPLY supersedes it
    state.stop_restore_polling();
    state.cancel_delayed_reapply();
    state.exit_after_revert.store(false, std::memory_order_release);

    std::string json(reinterpret_cast<const char *>(payload.data()), payload.size());
    bool wa_hdr_toggle = false;
    std::string sanitized_json = json;  // may strip helper-only fields
    try {
      auto j = nlohmann::json::parse(json);
      if (j.is_object()) {
        // Extract helper-only flags, then erase them so strict parsers accept the payload
        if (j.contains("wa_hdr_toggle")) {
          wa_hdr_toggle = j["wa_hdr_toggle"].get<bool>();
          j.erase("wa_hdr_toggle");
        }
        sanitized_json = j.dump();
      }
    } catch (...) {
      // If parsing fails, proceed with the original string (legacy senders)
    }

    display_device::SingleDisplayConfiguration cfg {};
    std::string err;
    if (!display_device::fromJson(sanitized_json, cfg, &err)) {
      BOOST_LOG(error) << "Failed to parse SingleDisplayConfiguration JSON: " << err;
      return;
    }
    state.last_apply_ms.store(
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
      )
        .count(),
      std::memory_order_release
    );
    state.last_cfg = cfg;
    if (!state.session_saved.load(std::memory_order_acquire)) {
      std::error_code ec_exist_cur, ec_exist_prev;
      const bool cur_exists = std::filesystem::exists(state.session_current_path, ec_exist_cur);
      const bool prev_exists = std::filesystem::exists(state.session_previous_path, ec_exist_prev);

      if (cur_exists && !ec_exist_cur) {
        state.session_saved.store(true, std::memory_order_release);
        BOOST_LOG(info) << "Session current baseline already exists; preserving existing snapshot: "
                        << state.session_current_path.string();
      } else {
        auto pre_snap = state.controller.snapshot();
        const auto pre_devices = ServiceState::snapshot_device_set(pre_snap);
        const bool skip_snapshot = state.should_skip_session_snapshot(cfg, pre_snap);

        auto copy_prev_to_current = [&]() -> bool {
          if (!prev_exists || ec_exist_prev) {
            return false;
          }
          std::error_code ec_copy;
          std::filesystem::create_directories(state.session_current_path.parent_path(), ec_copy);
          (void) ec_copy;
          std::filesystem::remove(state.session_current_path, ec_copy);
          std::filesystem::copy_file(state.session_previous_path, state.session_current_path, std::filesystem::copy_options::overwrite_existing, ec_copy);
          const bool ok = !ec_copy && std::filesystem::exists(state.session_current_path, ec_copy);
          BOOST_LOG(info) << "Copied previous session baseline to current: result=" << (ok ? "true" : "false");
          return ok;
        };

        bool saved = false;
        bool used_previous = false;

        if (prev_exists && !ec_exist_prev) {
          bool should_copy_prev = skip_snapshot;
          if (!should_copy_prev) {
            if (auto prev_snap = state.controller.load_display_settings_snapshot(state.session_previous_path)) {
              const auto prev_devices = ServiceState::snapshot_device_set(*prev_snap);
              if (prev_devices != pre_devices) {
                should_copy_prev = true;
              }
            }
          }
          if (should_copy_prev && copy_prev_to_current()) {
            saved = true;
            used_previous = true;
          }
        }

        if (!saved && !skip_snapshot) {
          saved = state.controller.save_display_settings_snapshot_to_file(state.session_current_path);
        }

        state.session_saved.store(saved, std::memory_order_release);
        if (used_previous) {
          BOOST_LOG(info) << "Initialized current session baseline from previous.";
        }
        if (skip_snapshot && !saved) {
          BOOST_LOG(info) << "Skipped session baseline snapshot; current layout matches requested single-display configuration.";
        } else {
          BOOST_LOG(info) << "Established current session baseline: " << (saved ? "true" : "false");
        }
      }
    }
    state.retry_revert_on_topology.store(false, std::memory_order_release);
    state.exit_after_revert.store(false, std::memory_order_release);
    if (state.controller.soft_test_display_settings(cfg)) {
      const auto before_sig = state.controller.signature(state.controller.snapshot());
      (void) state.controller.apply(cfg);
      display_device::DisplaySettingsSnapshot cur;
      const bool got_stable = state.read_stable_snapshot(cur);
      const bool changed = got_stable ? (state.controller.signature(cur) != before_sig) : false;
      if (!changed) {
      }
      state.retry_apply_on_topology.store(false, std::memory_order_release);
      state.schedule_delayed_reapply();
      refresh_shell_after_display_change();
      state.schedule_hdr_blank_if_needed(wa_hdr_toggle);
    } else {
      BOOST_LOG(error) << "Display helper: configuration failed SDC_VALIDATE soft-test; not applying.";
    }
  }

  void handle_revert(ServiceState &state, std::atomic<bool> &running) {
    state.retry_apply_on_topology.store(false, std::memory_order_release);
    state.direct_revert_bypass_grace.store(true, std::memory_order_release);
    state.exit_after_revert.store(true, std::memory_order_release);
    state.restore_requested.store(true, std::memory_order_release);
    state.restore_origin_epoch.store(state.current_connection_epoch(), std::memory_order_release);
    // Begin polling every ~3s until restore confirmed successful.
    state.ensure_restore_polling(ServiceState::RestoreWindow::Primary);
  }

  void handle_misc(ServiceState &state, platf::dxgi::AsyncNamedPipe &async_pipe, MsgType type) {
    if (type == MsgType::ExportGolden) {
      const bool saved = state.controller.save_display_settings_snapshot_to_file(state.golden_path);
      BOOST_LOG(info) << "Export golden restore snapshot result=" << (saved ? "true" : "false");
    } else if (type == MsgType::Reset) {
      (void) state.controller.reset_persistence();
      state.retry_apply_on_topology.store(false, std::memory_order_release);
      state.retry_revert_on_topology.store(false, std::memory_order_release);
    } else if (type == MsgType::Ping) {
      send_framed_content(async_pipe, MsgType::Ping);
    } else {
      BOOST_LOG(warning) << "Unknown message type: " << static_cast<int>(type);
    }
  }

  void handle_frame(ServiceState &state, platf::dxgi::AsyncNamedPipe &async_pipe, MsgType type, std::span<const uint8_t> payload, std::atomic<bool> &running) {
    if (type == MsgType::Apply) {
      handle_apply(state, payload);
    } else if (type == MsgType::Revert) {
      handle_revert(state, running);
    } else if (type == MsgType::Stop) {
      running.store(false, std::memory_order_release);
    } else {
      handle_misc(state, async_pipe, type);
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

int main() {
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
  auto _log_guard = logging::init_append(2 /*info*/, logfile);

  platf::dxgi::FramedPipeFactory pipe_factory(std::make_unique<platf::dxgi::AnonymousPipeFactory>());
  dd_log_bridge().install();
  ServiceState state;
  state.golden_path = goldenfile;
  state.session_path = sessionfile;  // legacy
  state.session_current_path = session_current;
  state.session_previous_path = session_previous;
  {
    std::error_code ec_cur, ec_legacy;
    const bool cur_exists = std::filesystem::exists(state.session_current_path, ec_cur);
    const bool legacy_exists = std::filesystem::exists(state.session_path, ec_legacy);
    if (cur_exists && !ec_cur) {
      state.session_saved.store(true, std::memory_order_release);
      BOOST_LOG(info) << "Existing current session snapshot detected; will preserve until confirmed restore: "
                      << state.session_current_path.string();
    } else if (legacy_exists && !ec_legacy) {
      std::error_code ec_copy;
      std::filesystem::create_directories(state.session_current_path.parent_path(), ec_copy);
      (void) ec_copy;
      std::filesystem::copy_file(state.session_path, state.session_current_path, std::filesystem::copy_options::overwrite_existing, ec_copy);
      if (!ec_copy) {
        state.session_saved.store(true, std::memory_order_release);
        BOOST_LOG(info) << "Migrated legacy session snapshot to current: " << state.session_current_path.string();
        std::error_code ec_rm;
        (void) std::filesystem::remove(state.session_path, ec_rm);
      }
    }
  }
  // Topology-based retries disabled; no watcher needed anymore.

  std::atomic<bool> running {true};
  state.running_flag = &running;

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

    // For anonymous-pipe server, give the client ample time to connect to the data pipe
    async_pipe.wait_for_client_connection(60000);  // 60s window after handshake

    if (!async_pipe.is_connected()) {
      BOOST_LOG(warning) << "Display helper: client connection not established within 60s window; retrying.";
      std::this_thread::sleep_for(500ms);
      continue;
    }

    const auto connection_epoch = state.begin_connection_epoch();
    state.stop_restore_polling();

    auto on_message = [&, connection_epoch](std::span<const uint8_t> bytes) {
      if (!state.is_connection_epoch_current(connection_epoch)) {
        return;
      }
      try {
        process_incoming_frame(state, async_pipe, bytes, running);
      } catch (const std::exception &ex) {
        BOOST_LOG(error) << "IPC framing error: " << ex.what();
      }
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
      attempt_revert_after_disconnect(state, running, connection_epoch);
    };

    // Start async message loop (establish_connection is a no-op if already connected)
    async_pipe.start(on_message, on_error, on_broken);

    // Stay in this inner loop until the client disconnects or service told to exit
    while (running.load(std::memory_order_acquire) && async_pipe.is_connected() && !broken.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(200ms);
    }

    // Ensure the worker thread is stopped and the server handle is
    // disconnected before looping to accept a new session.
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
