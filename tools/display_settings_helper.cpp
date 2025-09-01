/**
 * @file tools/display_settings_helper.cpp
 * @brief Detached helper to apply/revert Windows display settings via IPC.
 */

#ifdef _WIN32

// standard
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// third-party (libdisplaydevice)
#include <display_device/json.h>
#include <display_device/noop_audio_context.h>
#include <display_device/noop_settings_persistence.h>
#include <display_device/windows/settings_manager.h>
#include <display_device/windows/win_api_layer.h>
#include <display_device/windows/win_display_device.h>

// sunshine
#include "src/logging.h"
#include "src/platform/windows/ipc/pipes.h"

using namespace std::chrono_literals;
namespace bl = boost::log;

namespace {

// Simple framed protocol: [u32 length][u8 type][payload...]
enum class MsgType : uint8_t {
  Apply = 1,        // payload: JSON SingleDisplayConfiguration
  Revert = 2,       // no payload
  Reset = 3,        // clear persistence (best-effort)
  Ping = 0xFE,      // no payload, reply with Pong
  Stop = 0xFF       // no payload, terminate process
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
      if (len > 1024 * 1024) { // 1 MB guard
        throw std::runtime_error("IPC frame too large");
      }
      if (buf.size() < 4u + len) {
        break; // need more data
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
    std::shared_ptr<display_device::WinDisplayDeviceInterface> dd_api =
      std::make_shared<display_device::WinDisplayDevice>(std::make_shared<display_device::WinApiLayer>());

    // Use noop persistence and audio context here; Sunshine owns lifecycle across streams.
    m_sm = std::make_unique<display_device::SettingsManager>(
      std::move(dd_api),
      std::make_shared<display_device::NoopAudioContext>(),
      std::make_unique<display_device::PersistentState>(std::make_shared<display_device::NoopSettingsPersistence>()),
      display_device::WinWorkarounds{}
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

  // Get a coarse signature representing current topology; compares device IDs only.
  std::vector<std::string> current_topology_signature() const {
    std::vector<std::string> sig;
    try {
      auto devices = m_sm->enumAvailableDevices();
      sig.reserve(devices.size());
      for (auto &d : devices) {
        sig.emplace_back(d.m_device_id);
      }
      std::sort(sig.begin(), sig.end());
    } catch (...) {
      // leave empty on error
    }
    return sig;
  }

private:
  std::unique_ptr<display_device::SettingsManagerInterface> m_sm;
};

class TopologyWatcher {
public:
  using Callback = std::function<void()>;

  void start(Callback cb) {
    stop();
    _stop = false;
    _worker = std::jthread([this, cb = std::move(cb)](std::stop_token) {
      DisplayController ctrl; // lightweight wrapper to query devices
      auto last = ctrl.current_topology_signature();
      while (!_stop.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(1000ms);
        auto now = ctrl.current_topology_signature();
        if (now != last) {
          last = std::move(now);
          try { cb(); } catch (...) { /* ignore */ }
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

  ~TopologyWatcher() { stop(); }

private:
  std::atomic<bool> _stop{false};
  std::jthread _worker;
};

struct ServiceState {
  DisplayController controller;
  TopologyWatcher watcher;
  std::atomic<bool> retry_apply_on_topology{false};
  std::atomic<bool> retry_revert_on_topology{false};
  std::optional<display_device::SingleDisplayConfiguration> last_cfg;
  std::atomic<bool> exit_after_revert{false};
  std::atomic<bool>* running_flag{nullptr};
  std::jthread delayed_reapply_thread;  // Best-effort re-apply timer

  void on_topology_changed() {
    // Retry whichever hook is active
    if (retry_apply_on_topology.load(std::memory_order_acquire) && last_cfg) {
      BOOST_LOG(info) << "Topology changed: reattempting apply";
      if (controller.apply(*last_cfg)) {
        retry_apply_on_topology.store(false, std::memory_order_release);
      }
    } else if (retry_revert_on_topology.load(std::memory_order_acquire)) {
      BOOST_LOG(info) << "Topology changed: reattempting revert";
      if (controller.revert()) {
        retry_revert_on_topology.store(false, std::memory_order_release);
        if (exit_after_revert.load(std::memory_order_acquire) && running_flag) {
          // Successful revert after retries — request process exit
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
    delayed_reapply_thread = std::jthread([this](std::stop_token st){
      using namespace std::chrono_literals;
      // Try a short delay then a longer one
      const auto delays = {1s, 3s};
      for (auto d : delays) {
        if (st.stop_requested()) return;
        std::this_thread::sleep_for(d);
        if (st.stop_requested()) return;
        try {
          BOOST_LOG(info) << "Delayed re-apply attempt after activation";
          // Best-effort; ignore result
          if (last_cfg) {
            (void)controller.apply(*last_cfg);
          }
        } catch (...) {
          // ignore
        }
      }
    });
  }
};

} // namespace

int main() {
  // Initialize logging to a temp file; also logs to stdout
  auto init_log_path = []() -> std::string {
    std::wstring temp_path(MAX_PATH, L'\0');
    if (auto len = GetTempPathW(MAX_PATH, temp_path.data()); len == 0 || len > MAX_PATH) {
      return "sunshine_display_helper.log";
    }
    temp_path.resize(wcslen(temp_path.data()));
    std::wstring wlog = temp_path + L"sunshine_display_helper.log";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wlog.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string log_file(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wlog.c_str(), -1, &log_file[0], size_needed, nullptr, nullptr);
    if (!log_file.empty() && log_file.back() == '\0') {
      log_file.pop_back();
    }
    return log_file;
  }();
  auto _log_guard = logging::init(2 /*info*/, init_log_path);

  BOOST_LOG(info) << "Display settings helper starting (detached IPC server)";

  // Create anonymous pipe server for control handshake
  platf::dxgi::AnonymousPipeFactory pipe_factory;
  auto ctrl_pipe = pipe_factory.create_server("sunshine_display_helper");
  if (!ctrl_pipe) {
    BOOST_LOG(fatal) << "Failed to create control pipe";
    return 1;
  }

  platf::dxgi::AsyncNamedPipe async_pipe(std::move(ctrl_pipe));
  ServiceState state;
  state.watcher.start([&]() { state.on_topology_changed(); });

  FramedReader reader;
  std::atomic<bool> running{true};
  state.running_flag = &running;

  auto on_message = [&](std::span<const uint8_t> bytes) {
    try {
      reader.on_bytes(bytes, [&](std::span<const uint8_t> frame) {
        if (frame.size() < 1) return;
        auto type = static_cast<MsgType>(frame[0]);
        std::span<const uint8_t> payload = frame.subspan(1);
        switch (type) {
          case MsgType::Apply: {
            std::string json(reinterpret_cast<const char *>(payload.data()), payload.size());
            display_device::SingleDisplayConfiguration cfg{};
            std::string err;
            if (!display_device::fromJson(json, cfg, &err)) {
              BOOST_LOG(error) << "Failed to parse SingleDisplayConfiguration JSON: " << err;
              break;
            }
            state.last_cfg = cfg;
            // Cancel any pending revert retry
            state.retry_revert_on_topology.store(false, std::memory_order_release);
            state.exit_after_revert.store(false, std::memory_order_release);
            bool ok = state.controller.apply(cfg);
            state.retry_apply_on_topology.store(!ok, std::memory_order_release);
            // Regardless of immediate result, schedule delayed re-apply to defeat
            // post-activation native mode forcing by Windows/driver.
            state.schedule_delayed_reapply();
            break;
          }
          case MsgType::Revert: {
            // Cancel any pending apply retry to avoid undoing recent apply accidentally
            state.retry_apply_on_topology.store(false, std::memory_order_release);
            bool ok = state.controller.revert();
            state.retry_revert_on_topology.store(!ok, std::memory_order_release);
            if (ok) {
              // Successful revert — helper can exit now
              running.store(false, std::memory_order_release);
            } else {
              // Keep trying in background and exit once we succeed
              state.exit_after_revert.store(true, std::memory_order_release);
            }
            break;
          }
          case MsgType::Reset: {
            (void)state.controller.reset_persistence();
            // Also cancel retries to avoid interference with user manual actions
            state.retry_apply_on_topology.store(false, std::memory_order_release);
            state.retry_revert_on_topology.store(false, std::memory_order_release);
            break;
          }
          case MsgType::Ping: {
            // reply with Ping (echo)
            // send without extra payload
            send_frame(async_pipe, MsgType::Ping);
            break;
          }
          case MsgType::Stop: {
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

  auto on_error = [&](const std::string &err) {
    BOOST_LOG(error) << "Async pipe error: " << err;
  };

  auto on_broken = [&]() {
    BOOST_LOG(warning) << "Client disconnected";
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
  return 0;
}

#else
int main() { return 0; }
#endif
