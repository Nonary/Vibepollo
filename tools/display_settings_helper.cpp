/**
 * @file tools/display_settings_helper.cpp
 * @brief Detached helper to apply/revert Windows display settings via IPC.
 */

#ifdef _WIN32

  // standard
  #include <algorithm>
  #include <atomic>
  #include <chrono>
  #include <cstdint>
  #include <cstdio>
  #include <cstdlib>
  #include <cstring>
  #include <cwchar>
  #include <filesystem>
  #include <functional>
  #include <memory>
  #include <optional>
  #include <span>
  #include <set>
  #include <string>
  #include <thread>
  #include <vector>

// third-party (libdisplaydevice)
  #include "src/logging.h"
  #include "src/platform/windows/ipc/pipes.h"

  #include <display_device/json.h>
  #include <display_device/noop_audio_context.h>
  #include <display_device/noop_settings_persistence.h>
  #include <display_device/windows/settings_manager.h>
  #include <display_device/windows/win_api_layer.h>
  #include <display_device/windows/win_display_device.h>

  // platform
  #include <shlobj.h>
  #include <windows.h>
  #include <winerror.h>

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
      SendMessageTimeoutW(HWND_BROADCAST, msg, wParam, lParam,
                          SMTO_ABORTIFHUNG | SMTO_NORMAL, 100, &result);
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
    ExportGolden = 4, // no payload; export current settings snapshot as golden restore
    Ping = 0xFE,  // no payload, reply with Pong
    Stop = 0xFF  // no payload, terminate process
  };

  struct FramedReader {
    std::vector<uint8_t> buf;

    // Append chunk and extract complete frames.
    template<class Fn>
    void on_bytes(std::span<const uint8_t> chunk, Fn &&on_frame) {
      buf.insert(buf.end(), chunk.begin(), chunk.end());
      while (buf.size() >= 4) {
        uint32_t len = 0;
        std::memcpy(&len, buf.data(), sizeof(len));
        if (len > 1024 * 1024) {  // 1 MB guard
          throw std::runtime_error("IPC frame too large");
        }
        if (buf.size() < 4u + len) {
          break;  // need more data
        }
        std::vector<uint8_t> frame(buf.begin() + 4, buf.begin() + 4 + len);
        buf.erase(buf.begin(), buf.begin() + 4 + len);
        on_frame(std::span<const uint8_t>(frame.data(), frame.size()));
      }
    }
  };

  // Helper to send framed message
  inline void send_frame(platf::dxgi::AsyncNamedPipe &pipe, MsgType type, std::span<const uint8_t> payload = {}) {
    uint32_t len = static_cast<uint32_t>(1 + payload.size());
    std::vector<uint8_t> out;
    out.reserve(4 + len);
    out.insert(out.end(), reinterpret_cast<const uint8_t *>(&len), reinterpret_cast<const uint8_t *>(&len) + 4);
    out.push_back(static_cast<uint8_t>(type));
    out.insert(out.end(), payload.begin(), payload.end());
    pipe.send(out);
  }

  // Wrap SettingsManager for easy use in this helper
  class DisplayController {
  public:
    DisplayController() {
      // Build the Windows display device API and wrap with impersonation if running as SYSTEM.
      m_dd = std::make_shared<display_device::WinDisplayDevice>(std::make_shared<display_device::WinApiLayer>());

      // Use noop persistence and audio context here; Sunshine owns lifecycle across streams.
      m_sm = std::make_unique<display_device::SettingsManager>(
        m_dd,
        std::make_shared<display_device::NoopAudioContext>(),
        std::make_unique<display_device::PersistentState>(std::make_shared<display_device::NoopSettingsPersistence>()),
        display_device::WinWorkarounds {}
      );
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
          for (const auto &id : grp) {
            device_ids.insert(id);
          }
        }
        // If topology is empty, fall back to all enumerated devices
        if (device_ids.empty()) {
          for (const auto &d : m_sm->enumAvailableDevices()) {
            device_ids.insert(d.m_device_id);
          }
        }

        // Modes and HDR
        snap.m_modes = m_dd->getCurrentDisplayModes(device_ids);
        snap.m_hdr_states = m_dd->getCurrentHdrStates(device_ids);

        // Primary device
        for (const auto &id : device_ids) {
          if (m_dd->isPrimary(id)) {
            snap.m_primary_device = id;
            break;
          }
        }
      } catch (...) {
        // Leave whatever was collected; snapshot might be incomplete
      }
      return snap;
    }

    // Compute a simple signature string from snapshot for change detection.
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
        if (!kh.second.has_value()) {
          s += "null";
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
    bool save_golden(const std::filesystem::path &path) const {
      auto snap = snapshot();
      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);
      FILE *f = _wfopen(path.wstring().c_str(), L"wb");
      if (!f) {
        return false;
      }
      auto guard = std::unique_ptr<FILE, int(*)(FILE*)>(f, fclose);
      std::string out;
      out += "{\n  \"topology\": [";
      for (size_t i = 0; i < snap.m_topology.size(); ++i) {
        const auto &grp = snap.m_topology[i];
        out += "[";
        for (size_t j = 0; j < grp.size(); ++j) {
          out += "\"" + grp[j] + "\"";
          if (j + 1 < grp.size()) out += ",";
        }
        out += "]";
        if (i + 1 < snap.m_topology.size()) out += ",";
      }
      out += "],\n  \"modes\": {";
      size_t k = 0;
      for (const auto &kv : snap.m_modes) {
        out += "\n    \"" + kv.first + "\": { \"w\": " + std::to_string(kv.second.m_resolution.m_width)
               + ", \"h\": " + std::to_string(kv.second.m_resolution.m_height)
               + ", \"num\": " + std::to_string(kv.second.m_refresh_rate.m_numerator)
               + ", \"den\": " + std::to_string(kv.second.m_refresh_rate.m_denominator) + " }";
        if (++k < snap.m_modes.size()) out += ",";
      }
      out += "\n  },\n  \"hdr\": {";
      k = 0;
      for (const auto &kh : snap.m_hdr_states) {
        out += "\n    \"" + kh.first + "\": ";
        if (!kh.second.has_value()) out += "null";
        else out += (*kh.second == display_device::HdrState::Enabled) ? "\"on\"" : "\"off\"";
        if (++k < snap.m_hdr_states.size()) out += ",";
      }
      out += "\n  },\n  \"primary\": \"" + snap.m_primary_device + "\"\n}";
      const auto written = fwrite(out.data(), 1, out.size(), f);
      return written == out.size();
    }

    // Load snapshot from file.
    std::optional<display_device::DisplaySettingsSnapshot> load_golden(const std::filesystem::path &path) const {
      std::error_code ec;
      if (!std::filesystem::exists(path, ec)) {
        return std::nullopt;
      }
      FILE *f = _wfopen(path.wstring().c_str(), L"rb");
      if (!f) return std::nullopt;
      auto guard = std::unique_ptr<FILE, int(*)(FILE*)>(f, fclose);
      std::string data;
      char buf[4096];
      while (size_t n = fread(buf, 1, sizeof(buf), f)) {
        data.append(buf, n);
      }
      // Extremely lightweight parser sufficient for our own format.
      display_device::DisplaySettingsSnapshot snap;
      auto find_str = [&](const std::string &key) -> std::string {
        auto p = data.find("\"" + key + "\"");
        if (p == std::string::npos) return {};
        p = data.find(':', p);
        if (p == std::string::npos) return {};
        return data.substr(p + 1);
      };
      // Parse primary
      {
        auto prim = find_str("primary");
        auto q1 = prim.find('"');
        auto q2 = prim.find('"', q1 == std::string::npos ? 0 : q1 + 1);
        if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1) {
          snap.m_primary_device = prim.substr(q1 + 1, q2 - q1 - 1);
        }
      }
      // Parse topology
      {
        auto topo_s = find_str("topology");
        // Expect [["id1","id2"],["id3"]]
        snap.m_topology.clear();
        size_t i = topo_s.find('[');
        if (i != std::string::npos) {
          ++i; // skip [
          while (i < topo_s.size() && topo_s[i] != ']') {
            while (i < topo_s.size() && topo_s[i] != '[' && topo_s[i] != ']') ++i;
            if (i >= topo_s.size() || topo_s[i] == ']') break;
            ++i; // skip [
            std::vector<std::string> grp;
            while (i < topo_s.size() && topo_s[i] != ']') {
              while (i < topo_s.size() && topo_s[i] != '"' && topo_s[i] != ']') ++i;
              if (i >= topo_s.size() || topo_s[i] == ']') break;
              auto q1 = i + 1;
              auto q2 = topo_s.find('"', q1);
              if (q2 == std::string::npos) break;
              grp.emplace_back(topo_s.substr(q1, q2 - q1));
              i = q2 + 1;
            }
            while (i < topo_s.size() && topo_s[i] != ']') ++i;
            if (i < topo_s.size() && topo_s[i] == ']') ++i; // skip ]
            snap.m_topology.emplace_back(std::move(grp));
          }
        }
      }
      // Parse modes
      {
        auto modes_s = find_str("modes");
        size_t i = modes_s.find('{');
        snap.m_modes.clear();
        if (i != std::string::npos) {
          ++i;
          while (i < modes_s.size() && modes_s[i] != '}') {
            while (i < modes_s.size() && modes_s[i] != '"' && modes_s[i] != '}') ++i;
            if (i >= modes_s.size() || modes_s[i] == '}') break;
            auto q1 = i + 1;
            auto q2 = modes_s.find('"', q1);
            if (q2 == std::string::npos) break;
            std::string id = modes_s.substr(q1, q2 - q1);
            i = modes_s.find('{', q2);
            if (i == std::string::npos) break;
            auto end = modes_s.find('}', i);
            if (end == std::string::npos) break;
            auto obj = modes_s.substr(i, end - i);
            auto get_num = [&](const char *key) -> unsigned int {
              auto p = obj.find(key);
              if (p == std::string::npos) return 0;
              p = obj.find(':', p);
              if (p == std::string::npos) return 0;
              return static_cast<unsigned int>(std::stoul(obj.substr(p + 1)));
            };
            display_device::DisplayMode dm;
            dm.m_resolution.m_width = get_num("\"w\"");
            dm.m_resolution.m_height = get_num("\"h\"");
            dm.m_refresh_rate.m_numerator = get_num("\"num\"");
            dm.m_refresh_rate.m_denominator = get_num("\"den\"");
            snap.m_modes.emplace(id, dm);
            i = end + 1;
          }
        }
      }
      // Parse HDR
      {
        auto hdr_s = find_str("hdr");
        size_t i = hdr_s.find('{');
        snap.m_hdr_states.clear();
        if (i != std::string::npos) {
          ++i;
          while (i < hdr_s.size() && hdr_s[i] != '}') {
            while (i < hdr_s.size() && hdr_s[i] != '"' && hdr_s[i] != '}') ++i;
            if (i >= hdr_s.size() || hdr_s[i] == '}') break;
            auto q1 = i + 1;
            auto q2 = hdr_s.find('"', q1);
            if (q2 == std::string::npos) break;
            std::string id = hdr_s.substr(q1, q2 - q1);
            i = hdr_s.find(':', q2);
            if (i == std::string::npos) break;
            ++i;
            while (i < hdr_s.size() && (hdr_s[i] == ' ' || hdr_s[i] == '"')) ++i;
            std::optional<display_device::HdrState> val;
            if (hdr_s.compare(i, 2, "on") == 0) val = display_device::HdrState::Enabled;
            else if (hdr_s.compare(i, 3, "off") == 0) val = display_device::HdrState::Disabled;
            else val = std::nullopt;
            snap.m_hdr_states.emplace(id, val);
            while (i < hdr_s.size() && hdr_s[i] != ',' && hdr_s[i] != '}') ++i;
            if (i < hdr_s.size() && hdr_s[i] == ',') ++i;
          }
        }
      }
      return snap;
    }

    // Apply snapshot best-effort.
    bool apply_snapshot(const display_device::DisplaySettingsSnapshot &snap) {
      try {
        // Set topology first
        (void) m_dd->setTopology(snap.m_topology);
        // Then set modes with fallback to closest supported
        (void) m_dd->setDisplayModesWithFallback(snap.m_modes);
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
    std::unique_ptr<display_device::SettingsManagerInterface> m_sm;
    std::shared_ptr<display_device::WinDisplayDeviceInterface> m_dd;
  };

  class TopologyWatcher {
  public:
    using Callback = std::function<void()>;

    void start(Callback cb) {
      stop();
      _stop = false;
      _worker = std::jthread([this, cb = std::move(cb)](std::stop_token) {
        DisplayController ctrl;  // lightweight wrapper to query devices
        auto last = ctrl.current_topology_signature();
        while (!_stop.load(std::memory_order_acquire)) {
          std::this_thread::sleep_for(1000ms);
          auto now = ctrl.current_topology_signature();
          if (now != last) {
            last = std::move(now);
            try {
              cb();
            } catch (...) { /* ignore */
            }
          }
        }
      });
    }

    void stop() {
      _stop.store(true, std::memory_order_release);
      if (_worker.joinable()) {
        _worker.request_stop();
        _worker.join();
      }
    }

    ~TopologyWatcher() {
      stop();
    }

  private:
    std::atomic<bool> _stop {false};
    std::jthread _worker;
  };

  struct ServiceState {
    DisplayController controller;
    TopologyWatcher watcher;
    std::atomic<bool> retry_apply_on_topology {false};
    std::atomic<bool> retry_revert_on_topology {false};
    std::optional<display_device::SingleDisplayConfiguration> last_cfg;
    std::atomic<bool> exit_after_revert {false};
    std::atomic<bool> *running_flag {nullptr};
    std::jthread delayed_reapply_thread;  // Best-effort re-apply timer
    std::filesystem::path golden_path;  // file to store golden snapshot
    // Track last APPLY to suppress revert-on-topology within a grace window
    std::atomic<long long> last_apply_ms {0};
    // If a REVERT was requested directly by Sunshine, bypass grace
    std::atomic<bool> direct_revert_bypass_grace {false};

    void on_topology_changed() {
      // Retry whichever hook is active
      if (retry_apply_on_topology.load(std::memory_order_acquire) && last_cfg) {
        BOOST_LOG(info) << "Topology changed: reattempting apply";
        if (controller.apply(*last_cfg)) {
          retry_apply_on_topology.store(false, std::memory_order_release);
          refresh_shell_after_display_change();
        }
      } else if (retry_revert_on_topology.load(std::memory_order_acquire)) {
        // If we recently received an APPLY, defer automatic revert triggered by
        // topology changes for a short grace period to avoid flapping when
        // resolution/HDR/other parameters settle.
        constexpr auto kGraceMs = 10'000LL; // 10 seconds
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch()).count();
        const auto last_ms = last_apply_ms.load(std::memory_order_acquire);
        const bool within_grace = (last_ms != 0) && (now_ms - last_ms < kGraceMs);
        const bool bypass = direct_revert_bypass_grace.load(std::memory_order_acquire);

        if (within_grace && !bypass) {
          BOOST_LOG(info) << "Topology changed: revert deferred due to recent APPLY (within 10s grace).";
          return;
        }

        BOOST_LOG(info) << "Topology changed: reattempting revert";
        if (controller.revert()) {
          retry_revert_on_topology.store(false, std::memory_order_release);
          refresh_shell_after_display_change();
          if (bypass) {
            // Clear bypass flag once revert succeeds
            direct_revert_bypass_grace.store(false, std::memory_order_release);
          }
          if (exit_after_revert.load(std::memory_order_acquire) && running_flag) {
            // Successful revert after retries - request process exit
            running_flag->store(false, std::memory_order_release);
          }
        }
      }
    }

    // Schedule a couple of delayed re-apply attempts to work around Windows
    // sometimes forcing native resolution immediately after activating a display.
    void schedule_delayed_reapply() {
      // Stop any existing worker
      if (delayed_reapply_thread.joinable()) {
        delayed_reapply_thread.request_stop();
        delayed_reapply_thread.join();
      }
      if (!last_cfg) {
        return;
      }
      delayed_reapply_thread = std::jthread([this](std::stop_token st) {
        using namespace std::chrono_literals;
        // Try a short delay then a longer one
        const auto delays = {1s, 3s};
        for (auto d : delays) {
          if (st.stop_requested()) {
            return;
          }
          std::this_thread::sleep_for(d);
          if (st.stop_requested()) {
            return;
          }
          try {
            BOOST_LOG(info) << "Delayed re-apply attempt after activation";
            // Best-effort; ignore result
            if (last_cfg) {
              (void) controller.apply(*last_cfg);
              refresh_shell_after_display_change();
            }
          } catch (...) {
            // ignore
          }
        }
      });
    }

    void cancel_delayed_reapply() {
      if (delayed_reapply_thread.joinable()) {
        delayed_reapply_thread.request_stop();
        delayed_reapply_thread.join();
      }
    }
  };

}  // namespace

int main() {
  // Enforce single instance across the machine/session using a named mutex.
  // Prefer Global\ to avoid multiple helpers when launched from different sessions; if denied, fall back to Local\.
  auto make_mutex = [](const wchar_t *name) -> HANDLE {
    SECURITY_ATTRIBUTES sa {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    return CreateMutexW(&sa, FALSE, name);
  };

  HANDLE singleton = make_mutex(L"Global\\SunshineDisplayHelper");
  if (!singleton && GetLastError() == ERROR_ACCESS_DENIED) {
    singleton = make_mutex(L"Local\\SunshineDisplayHelper");
  }
  if (!singleton) {
    // As early as it gets; we don't have logging yet. Try a message box as last resort for debug builds.
    // Proceed without hard-fail but we may end up with multiple instances in rare cases.
  } else if (GetLastError() == ERROR_ALREADY_EXISTS) {
    // Another instance is already running; exit quietly.
    return 3;
  }

  // Initialize logging to a dedicated helper log file
  // Try %APPDATA%\Sunshine first via SHGetFolderPath; fall back to %APPDATA% env var, then %TEMP%
  std::filesystem::path logdir;
  {
    std::wstring appdataW;
    appdataW.resize(MAX_PATH);
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appdataW.data()))) {
      appdataW.resize(wcslen(appdataW.c_str()));
      logdir = std::filesystem::path(appdataW) / L"Sunshine";
    } else {
      // Try APPDATA environment variable
      std::wstring envAppData;
      DWORD needed = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
      if (needed > 0) {
        envAppData.resize(needed);
        DWORD written = GetEnvironmentVariableW(L"APPDATA", envAppData.data(), needed);
        if (written > 0) {
          envAppData.resize(written);
          logdir = std::filesystem::path(envAppData) / L"Sunshine";
        }
      }
      if (logdir.empty()) {
        // Final fallback: %TEMP%\Sunshine
        std::wstring tempW;
        tempW.resize(MAX_PATH);
        DWORD tlen = GetTempPathW(MAX_PATH, tempW.data());
        if (tlen > 0 && tlen < MAX_PATH) {
          tempW.resize(tlen);
          logdir = std::filesystem::path(tempW) / L"Sunshine";
        } else {
          // Extremely unlikely; last resort to current dir
          logdir = std::filesystem::path(L".") / L"Sunshine";
        }
      }
    }
  }
  std::error_code ec;
  std::filesystem::create_directories(logdir, ec);
  auto logfile = (logdir / L"sunshine_display_helper.log");
  auto goldenfile = (logdir / L"display_golden_restore.json");
  // Use append-mode logging to avoid cross-process truncation races with the cleanup watcher
  auto _log_guard = logging::init_append(2 /*info*/, logfile);

  // Create anonymous pipe server for control handshake
  platf::dxgi::AnonymousPipeFactory pipe_factory;
  auto ctrl_pipe = pipe_factory.create_server("sunshine_display_helper");
  if (!ctrl_pipe) {
    BOOST_LOG(fatal) << "Failed to create control pipe";
    return 1;
  }

  platf::dxgi::AsyncNamedPipe async_pipe(std::move(ctrl_pipe));
  ServiceState state;
  state.golden_path = goldenfile;
  state.watcher.start([&]() {
    state.on_topology_changed();
  });

  FramedReader reader;
  std::atomic<bool> running {true};
  state.running_flag = &running;

  auto on_message = [&](std::span<const uint8_t> bytes) {
    try {
      reader.on_bytes(bytes, [&](std::span<const uint8_t> frame) {
        if (frame.size() < 1) {
          return;
        }
        auto type = static_cast<MsgType>(frame[0]);
        std::span<const uint8_t> payload = frame.subspan(1);
        switch (type) {
          case MsgType::Apply:
            {
              std::string json(reinterpret_cast<const char *>(payload.data()), payload.size());
              display_device::SingleDisplayConfiguration cfg {};
              std::string err;
              if (!display_device::fromJson(json, cfg, &err)) {
                BOOST_LOG(error) << "Failed to parse SingleDisplayConfiguration JSON: " << err;
                break;
              }
              // Record time of APPLY to gate revert-on-topology changes for a short grace window
              state.last_apply_ms.store(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now().time_since_epoch()).count(),
                std::memory_order_release);
              state.last_cfg = cfg;
              // Cancel any pending revert retry
              state.retry_revert_on_topology.store(false, std::memory_order_release);
              state.exit_after_revert.store(false, std::memory_order_release);
              // Capture signature before applying
              const auto before_sig = state.controller.signature(state.controller.snapshot());
              bool ok = state.controller.apply(cfg);
              // Give the OS a brief moment to apply changes before checking
              std::this_thread::sleep_for(300ms);
              const auto after_sig = state.controller.signature(state.controller.snapshot());
              bool changed = (after_sig != before_sig);
              if (!changed) {
                BOOST_LOG(info) << "No change detected after APPLY; attempting golden restore fallback.";
                // Try golden restore if available
                auto snap = state.controller.load_golden(state.golden_path);
                if (snap) {
                  (void) state.controller.apply_snapshot(*snap);
                  std::this_thread::sleep_for(300ms);
                  const auto gsig = state.controller.signature(state.controller.snapshot());
                  changed = (gsig != before_sig);
                } else {
                  BOOST_LOG(warning) << "No golden restore snapshot available.";
                }
              }
              if (!changed) {
                BOOST_LOG(warning) << "No display changes detected; will retry on topology changes.";
              }
              state.retry_apply_on_topology.store(!changed, std::memory_order_release);
              // Regardless of immediate result, schedule delayed re-apply to defeat
              // post-activation native mode forcing by Windows/driver.
              state.schedule_delayed_reapply();
              // Nudge Explorer/shell so desktop/taskbar icons refresh sizes/metrics.
              refresh_shell_after_display_change();
              break;
            }
          case MsgType::Revert:
            {
              // Cancel any pending apply retry to avoid undoing recent apply accidentally
              state.retry_apply_on_topology.store(false, std::memory_order_release);
              // This is an explicit REVERT request from Sunshine; bypass grace window
              state.direct_revert_bypass_grace.store(true, std::memory_order_release);
              bool ok = state.controller.revert();
              state.retry_revert_on_topology.store(!ok, std::memory_order_release);
              // Attempt to refresh shell regardless; revert often restores primary/topology.
              refresh_shell_after_display_change();
              if (ok) {
                // Successful revert - helper can exit now
                running.store(false, std::memory_order_release);
              } else {
                // Keep trying in background and exit once we succeed
                state.exit_after_revert.store(true, std::memory_order_release);
              }
              break;
            }
          case MsgType::ExportGolden:
            {
              const bool saved = state.controller.save_golden(state.golden_path);
              BOOST_LOG(info) << "Export golden restore snapshot result=" << (saved ? "true" : "false");
              break;
            }
          case MsgType::Reset:
            {
              (void) state.controller.reset_persistence();
              // Also cancel retries to avoid interference with user manual actions
              state.retry_apply_on_topology.store(false, std::memory_order_release);
              state.retry_revert_on_topology.store(false, std::memory_order_release);
              break;
            }
          case MsgType::Ping:
            {
              // reply with Ping (echo)
              // send without extra payload
              send_frame(async_pipe, MsgType::Ping);
              break;
            }
          case MsgType::Stop:
            {
              running.store(false, std::memory_order_release);
              break;
            }
          default:
            BOOST_LOG(warning) << "Unknown message type: " << static_cast<int>(type);
            break;
        }
      });
    } catch (const std::exception &ex) {
      BOOST_LOG(error) << "IPC framing error: " << ex.what();
    }
  };

  auto attempt_revert_after_disconnect = [&]() {
    // Stop any future re-apply actions and cancel apply retries.
    state.retry_apply_on_topology.store(false, std::memory_order_release);
    state.cancel_delayed_reapply();

    // If we have ever applied a config, or already are in a revert retry state,
    // ensure we try reverting now and only exit once revert has succeeded.
    const bool potentially_modified = state.last_cfg.has_value() ||
                                      state.retry_revert_on_topology.load(std::memory_order_acquire) ||
                                      state.exit_after_revert.load(std::memory_order_acquire);

    if (potentially_modified) {
      BOOST_LOG(info) << "Client disconnected; attempting immediate revert and will remain alive until success.";
      const bool ok = state.controller.revert();
      state.retry_revert_on_topology.store(!ok, std::memory_order_release);
      state.exit_after_revert.store(true, std::memory_order_release);
      if (ok) {
        running.store(false, std::memory_order_release);
      } else {
        BOOST_LOG(warning) << "Revert not successful yet; will keep running and retry on topology changes.";
      }
    } else {
      // Nothing to revert -> safe to exit.
      running.store(false, std::memory_order_release);
    }
  };

  auto on_error = [&](const std::string &err) {
    BOOST_LOG(error) << "Async pipe error: " << err << "; handling disconnect and revert policy.";
    attempt_revert_after_disconnect();
  };

  auto on_broken = [&]() {
    BOOST_LOG(warning) << "Client disconnected; applying revert policy and staying alive until successful.";
    attempt_revert_after_disconnect();
  };

  // Wait for Sunshine to connect to the control pipe and transition to data pipe
  async_pipe.start(on_message, on_error, on_broken);
  async_pipe.wait_for_client_connection(5000);
  if (!async_pipe.is_connected()) {
    BOOST_LOG(error) << "No client connection within timeout; exiting";
    return 2;
  }

  while (running.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(200ms);
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
