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
#include <boost/log/sources/severity_logger.hpp>
#include "src/platform/windows/ipc/pipes.h"

using namespace std::chrono_literals;
namespace bl = boost::log;

// Provide minimal global severity loggers to satisfy BOOST_LOG usage
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
bl::sources::severity_logger<int> verbose(0);
bl::sources::severity_logger<int> debug(1);
bl::sources::severity_logger<int> info(2);
bl::sources::severity_logger<int> warning(3);
bl::sources::severity_logger<int> error(4);
bl::sources::severity_logger<int> fatal(5);
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

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
      }
    }
  }
};

} // namespace

int main() {

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
            bool ok = state.controller.apply(cfg);
            state.retry_apply_on_topology.store(!ok, std::memory_order_release);
            break;
          }
          case MsgType::Revert: {
            // Cancel any pending apply retry to avoid undoing recent apply accidentally
            state.retry_apply_on_topology.store(false, std::memory_order_release);
            bool ok = state.controller.revert();
            state.retry_revert_on_topology.store(!ok, std::memory_order_release);
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
